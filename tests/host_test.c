/* Synthetic NOR integration and power-cut tests. No device or user files. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "sqlite_littlefs_vfs.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define BLOCK_SIZE 4096u
#define BLOCK_COUNT 128u
#define FLASH_SIZE (BLOCK_SIZE * BLOCK_COUNT)
#define PROG_SIZE 256u
#define HEAP_SIZE (64u * 1024u)
#define CRASH_EXIT 77
#define DB_PATH "/db/main.sqlite"
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr,                                                                        \
                    "FAIL %s:%d: %s [run=%s cut=%u mode=%d op=%u original-cut=%u "                 \
                    "original-mode=%d action-cut=%u action-mode=%d]\n",                            \
                    __FILE__, __LINE__, #c, run_label, cut, fault, report ? report->events : 0,    \
                    scenario_cut, scenario_mode, action_cut, action_fault);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

enum Fault { NONE, BEFORE, PREFIX, SPARSE, AFTER, ERROR_BEFORE, ERROR_PARTIAL, ERROR_AFTER };
enum Job {
    SEED,
    ACTION,
    RECOVER,
    API_CHECKS,
    FILL,
    RECLAIM,
    READ_ERROR,
    INITIALIZE,
    PREPARE,
    READONLY_RECOVER
};
typedef struct Report {
    unsigned events, programs, erases, syncs, action_events, hit, ack;
    unsigned candidate, recovered_events, schema_present, backend_calls;
    int result;
} Report;
static unsigned char *flash;
static Report *report;
static enum Fault fault;
static unsigned cut, scenario_cut, action_cut;
static enum Fault action_fault;
static enum Fault scenario_mode;
static int armed, read_fault, require_new, allow_empty;
static const char *run_label = "startup";
static const char *journal_mode = "DELETE";
static lfs_t filesystem;
static struct lfs_config block_config;
static unsigned char read_cache[256], prog_cache[256], lookahead[32];
static sqlite3_lfs_vfs vfs;
static int vfs_active;
_Alignas(8) static unsigned char heap[HEAP_SIZE];

static void crash(void) {
    _exit(CRASH_EXIT);
}
static int begin_event(void) {
    if (!armed)
        return 0;
    ++report->events;
    if (report->events != cut || fault == NONE)
        return 0;
    report->hit = 1;
    if (fault == BEFORE)
        crash();
    if (fault == ERROR_BEFORE)
        return -1;
    return 1;
}
static int end_event(int selected) {
    if (selected > 0 && (fault == AFTER || fault == PREFIX || fault == SPARSE))
        crash();
    return selected > 0 && (fault == ERROR_PARTIAL || fault == ERROR_AFTER) ? LFS_ERR_IO : 0;
}
static void backend_entry(void) {
    CHECK(!vfs_active || !sqlite3_lfs_vfs_needs_remount(&vfs));
    ++report->backend_calls;
}
static void bounds(lfs_block_t block, lfs_off_t offset, lfs_size_t size) {
    CHECK(block < BLOCK_COUNT && offset <= BLOCK_SIZE && size <= BLOCK_SIZE - offset);
}
static int nor_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t offset, void *data,
                    lfs_size_t size) {
    (void)c;
    backend_entry();
    bounds(block, offset, size);
    if (read_fault && --read_fault == 0)
        return LFS_ERR_IO;
    memcpy(data, flash + block * BLOCK_SIZE + offset, size);
    return 0;
}
static int nor_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t offset,
                    const void *data, lfs_size_t size) {
    (void)c;
    backend_entry();
    bounds(block, offset, size);
    CHECK(offset % PROG_SIZE == 0 && size % PROG_SIZE == 0);
    if (armed)
        ++report->programs;
    int selected = begin_event();
    if (selected < 0)
        return LFS_ERR_IO;
    unsigned char *to = flash + block * BLOCK_SIZE + offset;
    const unsigned char *from = data;
    lfs_size_t count = (selected && (fault == PREFIX || fault == ERROR_PARTIAL)) ? size / 2 : size;
    for (lfs_size_t i = 0; i < count; ++i) {
        /* NOR programming cannot restore a zero bit, even if the caller
         * requests one; retain the physical AND result for recovery to see. */
        to[i] &= selected && fault == SPARSE ? (unsigned char)(from[i] | 0xaa) : from[i];
    }
    return end_event(selected);
}
static int nor_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    backend_entry();
    bounds(block, 0, BLOCK_SIZE);
    if (armed)
        ++report->erases;
    int selected = begin_event();
    if (selected < 0)
        return LFS_ERR_IO;
    unsigned char *to = flash + block * BLOCK_SIZE;
    unsigned count =
        (selected && (fault == PREFIX || fault == ERROR_PARTIAL)) ? BLOCK_SIZE / 2 : BLOCK_SIZE;
    for (unsigned i = 0; i < count; ++i)
        if (!(selected && fault == SPARSE) || i % 2 == 0)
            to[i] = 0xff;
    return end_event(selected);
}
static int nor_sync(const struct lfs_config *c) {
    (void)c;
    backend_entry();
    if (armed)
        ++report->syncs;
    int selected = begin_event();
    if (selected < 0)
        return LFS_ERR_IO;
    return end_event(selected); /* NOR is durable when a completed op returns. */
}
static void configure(void) {
    memset(&filesystem, 0, sizeof(filesystem));
    memset(&block_config, 0, sizeof(block_config));
    block_config.read = nor_read;
    block_config.prog = nor_prog;
    block_config.erase = nor_erase;
    block_config.sync = nor_sync;
    block_config.read_size = 1;
    block_config.prog_size = PROG_SIZE;
    block_config.block_size = BLOCK_SIZE;
    block_config.block_count = BLOCK_COUNT;
    block_config.block_cycles = 100;
    block_config.cache_size = 256;
    block_config.lookahead_size = sizeof(lookahead);
    block_config.read_buffer = read_cache;
    block_config.prog_buffer = prog_cache;
    block_config.lookahead_buffer = lookahead;
}
static int entropy(void *context, unsigned char *out, int size) {
    (void)context;
    static uint32_t x = 0x2350;
    for (int i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = (unsigned char)x;
    }
    return size;
}
static void start_sqlite(int readonly) {
    CHECK(sqlite3_config(SQLITE_CONFIG_HEAP, heap, sizeof(heap), 32) == SQLITE_OK);
    CHECK(sqlite3_initialize() == SQLITE_OK);
    sqlite3_lfs_vfs_config cfg = {0};
    cfg.lfs = &filesystem;
    cfg.name = "test-lfs";
    cfg.db_path = DB_PATH;
    cfg.randomness = entropy;
    cfg.read_only = readonly;
    CHECK(sqlite3_lfs_vfs_init(&vfs, &cfg) == SQLITE_OK);
    CHECK(sqlite3_lfs_vfs_register(&vfs, 1) == SQLITE_OK);
    vfs_active = 1;
}
static void stop_sqlite(void) {
    CHECK(sqlite3_lfs_vfs_unregister(&vfs) == SQLITE_OK);
    vfs_active = 0;
    CHECK(sqlite3_shutdown() == SQLITE_OK);
    CHECK(sqlite3_memory_used() == 0);
}
static void sql(sqlite3 *db, const char *text) {
    char *error = NULL;
    int rc = sqlite3_exec(db, text, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        fprintf(stderr, "SQL rc=%d: %s: %s\n", rc, error ? error : "", text);
    sqlite3_free(error);
    CHECK(rc == SQLITE_OK);
}
static int integer(sqlite3 *db, const char *text) {
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db, text, -1, &s, NULL);
    if (rc != SQLITE_OK)
        fprintf(stderr, "prepare rc=%d (%s): %s\n", rc, sqlite3_errmsg(db), text);
    CHECK(rc == SQLITE_OK);
    rc = sqlite3_step(s);
    if (rc != SQLITE_ROW)
        fprintf(stderr, "step rc=%d (%s): %s\n", rc, sqlite3_errmsg(db), text);
    CHECK(rc == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    CHECK(sqlite3_finalize(s) == SQLITE_OK);
    return value;
}
static void text_is(sqlite3 *db, const char *text, const char *expected) {
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db, text, -1, &s, NULL);
    if (rc != SQLITE_OK)
        fprintf(stderr, "text prepare rc=%d (%s), heap=%lld peak=%lld: %s\n", rc,
                sqlite3_errmsg(db), (long long)sqlite3_memory_used(),
                (long long)sqlite3_memory_highwater(0), text);
    CHECK(rc == SQLITE_OK);
    rc = sqlite3_step(s);
    if (rc != SQLITE_ROW)
        fprintf(stderr, "text step rc=%d (%s): %s\n", rc, sqlite3_errmsg(db), text);
    CHECK(rc == SQLITE_ROW);
    CHECK(strcmp((const char *)sqlite3_column_text(s, 0), expected) == 0);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    CHECK(sqlite3_finalize(s) == SQLITE_OK);
}
static sqlite3 *open_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "test-lfs");
    if (rc != SQLITE_OK)
        fprintf(stderr, "open rc=%d: %s\n", rc, sqlite3_errmsg(db));
    CHECK(rc == SQLITE_OK);
    char pragmas[160];
    snprintf(pragmas, sizeof(pragmas),
             "PRAGMA page_size=1024; PRAGMA cache_size=2; PRAGMA synchronous=EXTRA; PRAGMA "
             "journal_mode=%s;",
             journal_mode);
    sql(db, pragmas);
    CHECK(integer(db, "PRAGMA synchronous") == 3);
    return db;
}
static void create_schema(sqlite3 *db) {
    sql(db, "BEGIN; CREATE TABLE state(id INTEGER PRIMARY KEY CHECK(id=1),energy INTEGER NOT NULL);"
            "CREATE TABLE pending(id TEXT PRIMARY KEY,delta INTEGER NOT NULL,status TEXT NOT NULL);"
            "CREATE INDEX pending_status ON pending(status,id);"
            "INSERT INTO state VALUES(1,1); INSERT INTO pending "
            "VALUES('acknowledged',1,'pending'); COMMIT;");
}
static int action(sqlite3 *db) {
    return sqlite3_exec(db,
                        "BEGIN IMMEDIATE; INSERT INTO pending VALUES('candidate',1,'pending');"
                        "UPDATE state SET energy=energy+1 WHERE id=1; COMMIT;",
                        NULL, NULL, NULL);
}
static void validate_db(sqlite3 *db) {
    sql(db, "PRAGMA shrink_memory");
    text_is(db, "PRAGMA integrity_check", "ok");
    if (allow_empty && integer(db, "SELECT count(*) FROM sqlite_schema WHERE name='state'") == 0) {
        CHECK(integer(db, "SELECT count(*) FROM sqlite_schema") == 0);
        report->candidate = 0;
        return;
    }
    report->schema_present = 1;
    int energy = integer(db, "SELECT energy FROM state WHERE id=1");
    int count = integer(db, "SELECT count(*) FROM pending");
    CHECK(energy == count);
    CHECK(integer(db, "SELECT count(*) FROM state") == 1);
    CHECK(integer(db, "SELECT count(*) FROM pending WHERE delta<>1") == 0);
    CHECK(integer(db, "SELECT sum(delta) FROM pending") == energy);
    CHECK(
        integer(db,
                "SELECT count(*) FROM pending INDEXED BY pending_status WHERE status='pending'") ==
        count);
    CHECK(integer(db, "SELECT count(*) FROM pending WHERE id='acknowledged'") == 1);
    CHECK(count == 1 || count == 2);
    if (allow_empty)
        CHECK(count == 1);
    if (require_new)
        CHECK(count == 2);
    CHECK(integer(db, "SELECT count(*) FROM pending WHERE id='candidate'") == count - 1);
    report->candidate = (unsigned)(count - 1);
}
static unsigned char sentinel_byte(unsigned i) {
    return (unsigned char)((i * 37u) ^ (i >> 4) ^ 0xa5u);
}
static void sentinel(int create) {
    const char *paths[] = {"/unrelated.bin", "/assets/keep.txt"};
    for (unsigned f = 0; f < 2; ++f) {
        unsigned char cache[256], buffer[256];
        lfs_file_t file = {0};
        struct lfs_file_config cfg = {0};
        cfg.buffer = cache;
        CHECK(lfs_file_opencfg(&filesystem, &file, paths[f],
                               create ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL) : LFS_O_RDONLY,
                               &cfg) == 0);
        unsigned size = f ? 113 : 12307;
        for (unsigned offset = 0; offset < size;) {
            unsigned n = size - offset;
            if (n > sizeof(buffer))
                n = sizeof(buffer);
            if (create) {
                for (unsigned i = 0; i < n; ++i)
                    buffer[i] = sentinel_byte(offset + i + f);
                CHECK(lfs_file_write(&filesystem, &file, buffer, n) == (lfs_ssize_t)n);
            } else {
                CHECK(lfs_file_read(&filesystem, &file, buffer, n) == (lfs_ssize_t)n);
                for (unsigned i = 0; i < n; ++i)
                    CHECK(buffer[i] == sentinel_byte(offset + i + f));
            }
            offset += n;
        }
        CHECK(lfs_file_size(&filesystem, &file) == (lfs_soff_t)size);
        CHECK(lfs_file_close(&filesystem, &file) == 0);
    }
}
static void arm(void) {
    memset(report, 0, sizeof(*report));
    armed = 1;
}
static void stock_compatibility(sqlite3 *db) {
    if (!sqlite3_compileoption_used("OMIT_WAL")) {
        const char *locks[] = {"PRAGMA locking_mode=NORMAL", "PRAGMA locking_mode=EXCLUSIVE"};
        const char *expected = strcmp(journal_mode, "DELETE") == 0     ? "delete"
                               : strcmp(journal_mode, "TRUNCATE") == 0 ? "truncate"
                                                                       : "persist";
        for (unsigned i = 0; i < 2; ++i) {
            sql(db, locks[i]);
            CHECK(sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) == SQLITE_ERROR);
            text_is(db, "PRAGMA journal_mode", expected);
        }
        sql(db, "PRAGMA locking_mode=NORMAL");
        validate_db(db);
    }
#ifndef SQLITE_OMIT_LOAD_EXTENSION
    CHECK(sqlite3_enable_load_extension(db, 1) == SQLITE_OK);
    char *message = NULL;
    CHECK(sqlite3_load_extension(db, "this-extension-must-not-load", NULL, &message) ==
          SQLITE_ERROR);
    CHECK(message != NULL);
    sqlite3_free(message);
    CHECK(sqlite3_enable_load_extension(db, 0) == SQLITE_OK);
#endif
}
static void existing_wal(void) {
    unsigned char cache[256], data[64], got[64];
    memset(data, 0xc3, sizeof(data));
    struct lfs_file_config cfg = {0};
    cfg.buffer = cache;
    lfs_file_t file = {0};
    CHECK(lfs_file_opencfg(&filesystem, &file, DB_PATH "-wal",
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL, &cfg) == 0);
    CHECK(lfs_file_write(&filesystem, &file, data, sizeof(data)) == sizeof(data));
    CHECK(lfs_file_close(&filesystem, &file) == 0);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READWRITE, "test-lfs");
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db, "SELECT * FROM state", NULL, NULL, NULL);
    CHECK(rc != SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    memset(&file, 0, sizeof(file));
    CHECK(lfs_file_opencfg(&filesystem, &file, DB_PATH "-wal", LFS_O_RDONLY, &cfg) == 0);
    CHECK(lfs_file_read(&filesystem, &file, got, sizeof(got)) == sizeof(got));
    CHECK(memcmp(data, got, sizeof(data)) == 0);
    CHECK(lfs_file_size(&filesystem, &file) == sizeof(data));
    CHECK(lfs_file_close(&filesystem, &file) == 0);
    CHECK(lfs_remove(&filesystem, DB_PATH "-wal") == 0);
    db = open_db();
    validate_db(db);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static void api_checks(sqlite3 *db) {
    stock_compatibility(db);
    CHECK(sqlite3_threadsafe() == 0);
    CHECK(sqlite3_compileoption_used("ENABLE_MEMSYS5"));
    CHECK(sqlite3_compileoption_used("ZERO_MALLOC"));
    CHECK(integer(db, "SELECT date('now') IS NULL") == 1);
    text_is(db, "SELECT json_extract('{\"kind\":\"feed\"}','$.kind')", "feed");
    sql(db, "BEGIN; INSERT INTO pending VALUES('rolled-back',5,'pending'); UPDATE state SET "
            "energy=6; ROLLBACK;");
    validate_db(db);
    sql(db, "SAVEPOINT nested; INSERT INTO pending VALUES('savepoint',3,'pending'); ROLLBACK TO "
            "nested; RELEASE nested;");
    validate_db(db);
    CHECK(sqlite3_exec(db, "INSERT INTO pending VALUES('acknowledged',1,'pending')", NULL, NULL,
                       NULL) == SQLITE_CONSTRAINT);
    CHECK(sqlite3_exec(db, "SELECT hex(zeroblob(30000))", NULL, NULL, NULL) == SQLITE_NOMEM);
    validate_db(db);
    sqlite3 *other = NULL;
    int rc = sqlite3_open_v2(DB_PATH, &other, SQLITE_OPEN_READWRITE, "test-lfs");
    CHECK(rc != SQLITE_OK);
    CHECK(sqlite3_close(other) == SQLITE_OK);
    const char *bad_paths[] = {"/unrelated.bin", "/db/../db/main.sqlite", "/db/main.sqlite.other",
                               "/db/main.sqlite-journal"};
    for (unsigned i = 0; i < sizeof(bad_paths) / sizeof(*bad_paths); ++i) {
        other = NULL;
        rc = sqlite3_open_v2(bad_paths[i], &other, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             "test-lfs");
        CHECK(rc != SQLITE_OK);
        CHECK(sqlite3_close(other) == SQLITE_OK);
    }
    sqlite3_lfs_vfs alias = {0};
    sqlite3_lfs_vfs_config cfg = {0};
    cfg.lfs = &filesystem;
    cfg.name = "alias-lfs";
    cfg.db_path = DB_PATH;
    cfg.randomness = entropy;
    CHECK(sqlite3_lfs_vfs_init(&alias, &cfg) == SQLITE_OK);
    CHECK(sqlite3_lfs_vfs_register(&alias, 0) != SQLITE_OK);
    CHECK(sqlite3_lfs_vfs_unregister(&vfs) == SQLITE_BUSY);
    sql(db, "PRAGMA temp.page_size=1024; PRAGMA temp.cache_size=2; PRAGMA shrink_memory;");
    int temp_rc = sqlite3_exec(
        db, "CREATE TEMP TABLE scratch(n INTEGER); INSERT INTO scratch VALUES(3),(1),(2);", NULL,
        NULL, NULL);
    CHECK(temp_rc == SQLITE_OK || temp_rc == SQLITE_NOMEM);
    if (temp_rc == SQLITE_OK)
        CHECK(integer(db, "SELECT n FROM scratch ORDER BY n LIMIT 1") == 1);
    sql(db, "DROP TABLE IF EXISTS scratch; PRAGMA shrink_memory;");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    /* Direct VFS contracts: EOF zero-fill, negative/oversize offsets, strict
     * paths and exclusive creation. File storage is test-process allocation. */
    sqlite3_file *file = calloc(1, (size_t)vfs.base.szOsFile);
    CHECK(file != NULL);
    int flags = 0;
    CHECK(vfs.base.xOpen(&vfs.base, DB_PATH, file,
                         SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_EXCLUSIVE,
                         &flags) != SQLITE_OK);
    CHECK(vfs.base.xOpen(&vfs.base, DB_PATH "-wal", file,
                         SQLITE_OPEN_WAL | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         &flags) != SQLITE_OK);
    CHECK(vfs.base.xOpen(&vfs.base, DB_PATH, file, SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE,
                         &flags) == SQLITE_OK);
    sqlite3_int64 size = 0;
    CHECK(file->pMethods->xFileSize(file, &size) == SQLITE_OK);
    CHECK(size > 0);
    unsigned char buf[64];
    memset(buf, 0x55, sizeof(buf));
    CHECK(file->pMethods->xRead(file, buf, sizeof(buf), size - 7) == SQLITE_IOERR_SHORT_READ);
    for (unsigned i = 7; i < sizeof(buf); ++i)
        CHECK(buf[i] == 0);
    memset(buf, 0x55, sizeof(buf));
    CHECK(file->pMethods->xRead(file, buf, sizeof(buf), size + 13) == SQLITE_IOERR_SHORT_READ);
    for (unsigned i = 0; i < sizeof(buf); ++i)
        CHECK(buf[i] == 0);
    CHECK(file->pMethods->xWrite(file, buf, 1, ((sqlite3_int64)1 << 32)) != SQLITE_OK);
    CHECK(file->pMethods->xTruncate(file, ((sqlite3_int64)1 << 32)) != SQLITE_OK);
    CHECK(file->pMethods->xRead(file, buf, 1, -1) != SQLITE_OK);
    CHECK(file->pMethods->xDeviceCharacteristics(file) == 0);
    CHECK(file->pMethods->xClose(file) == SQLITE_OK);
    free(file);
    db = open_db();
    validate_db(db);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    existing_wal();
    sentinel(0);
}
static void fill_device(void) {
    lfs_file_t file = {0};
    unsigned char cache[256], data[4096];
    memset(data, 0x59, sizeof(data));
    struct lfs_file_config cfg = {0};
    cfg.buffer = cache;
    CHECK(lfs_file_opencfg(&filesystem, &file, "/db/filler", LFS_O_WRONLY | LFS_O_CREAT, &cfg) ==
          0);
    int rc = 0;
    unsigned blocks = 0;
    while (blocks++ < BLOCK_COUNT + 1) {
        rc = (int)lfs_file_write(&filesystem, &file, data, sizeof(data));
        if (rc < 0)
            break;
        CHECK(rc == sizeof(data));
        rc = lfs_file_sync(&filesystem, &file);
        if (rc < 0)
            break;
    }
    CHECK(rc == LFS_ERR_NOSPC);
    file.flags |= LFS_F_ERRED;
    CHECK(lfs_file_close(&filesystem, &file) == 0);
}
static void remount(void) {
    CHECK(lfs_unmount(&filesystem) == 0);
    configure();
    CHECK(lfs_mount(&filesystem, &block_config) == 0);
}
static void child(enum Job job) {
    configure();
    armed = 0;
    if (job == SEED || job == PREPARE) {
        CHECK(lfs_format(&filesystem, &block_config) == 0);
        CHECK(lfs_mount(&filesystem, &block_config) == 0);
        CHECK(lfs_mkdir(&filesystem, "/db") == 0);
        CHECK(lfs_mkdir(&filesystem, "/assets") == 0);
        sentinel(1);
    } else {
        if (job == RECOVER || job == INITIALIZE || job == READONLY_RECOVER)
            arm();
        CHECK(lfs_mount(&filesystem, &block_config) == 0);
    }
    if (job == PREPARE) {
        CHECK(lfs_unmount(&filesystem) == 0);
        _exit(0);
    }
    if (job == RECLAIM)
        CHECK(lfs_remove(&filesystem, "/db/filler") == 0);
    if (job == FILL) {
        fill_device();
        remount();
    }
    start_sqlite(job == READONLY_RECOVER);
    if (job == READONLY_RECOVER) {
        sqlite3 *db = NULL;
        int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, "test-lfs");
        if (rc == SQLITE_OK)
            rc =
                sqlite3_exec(db, "PRAGMA cache_size=2; SELECT energy FROM state", NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            validate_db(db);
            CHECK((sqlite3_exec(db, "UPDATE state SET energy=99", NULL, NULL, NULL) & 255) ==
                  SQLITE_READONLY);
        } else
            CHECK((rc & 255) == SQLITE_READONLY);
        report->result = rc;
        CHECK(sqlite3_close(db) == SQLITE_OK);
        CHECK(report->events == 0);
        armed = 0;
        stop_sqlite();
        sentinel(0);
        CHECK(lfs_unmount(&filesystem) == 0);
        _exit(0);
    }
    if (job == READ_ERROR) {
        sqlite3 *db = NULL;
        read_fault = 1;
        int rc = sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READWRITE, "test-lfs");
        if (rc == SQLITE_OK)
            rc = sqlite3_exec(db, "SELECT * FROM state", NULL, NULL, NULL);
        CHECK(rc != SQLITE_OK);
        report->result = rc;
        read_fault = 0;
        CHECK(sqlite3_lfs_vfs_needs_remount(&vfs));
        unsigned calls = report->backend_calls;
        CHECK(sqlite3_close(db) == SQLITE_OK);
        stop_sqlite();
        CHECK(report->backend_calls == calls);
        remount();
        start_sqlite(0);
        db = open_db();
        validate_db(db);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        stop_sqlite();
        sentinel(0);
        CHECK(lfs_unmount(&filesystem) == 0);
        _exit(0);
    }
    int poisoned = 0;
    unsigned failure_calls = 0;
    sqlite3 *db = open_db();
    if (job == SEED || job == INITIALIZE) {
        create_schema(db);
        if (job == INITIALIZE)
            report->ack = 1;
    } else if (job == API_CHECKS) {
        api_checks(db);
        stop_sqlite();
        CHECK(lfs_unmount(&filesystem) == 0);
        _exit(0);
    } else if (job == ACTION || job == RECLAIM || job == FILL) {
        arm();
        int rc = action(db);
        report->result = rc;
        report->action_events = report->events;
        if (rc == SQLITE_OK)
            report->ack = 1;
        else {
            CHECK(job == FILL || fault >= ERROR_BEFORE);
            /* A failed block operation requires remount: rollback and close
             * must refuse further backend calls, including implicit syncs. */
            poisoned = sqlite3_lfs_vfs_needs_remount(&vfs);
            CHECK(poisoned);
            failure_calls = report->backend_calls;
            armed = 0;
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        }
        if (job == FILL)
            CHECK(rc != SQLITE_OK && ((rc & 255) == SQLITE_FULL || (rc & 255) == SQLITE_IOERR));
        else if (fault < ERROR_BEFORE)
            CHECK(rc == SQLITE_OK);
    } else if (job == RECOVER)
        validate_db(db);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    if (job == ACTION || job == INITIALIZE) {
        int selected = begin_event();
        if (selected < 0)
            report->result = SQLITE_IOERR;
        (void)end_event(selected);
    }
    if (job == RECOVER)
        report->recovered_events = report->events;
    armed = 0;
    stop_sqlite();
    if (poisoned) {
        CHECK(report->backend_calls == failure_calls);
        remount();
        start_sqlite(0);
        db = open_db();
        validate_db(db);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        stop_sqlite();
    }
    sentinel(0);
    CHECK(lfs_unmount(&filesystem) == 0);
    _exit(0);
}
static int run(enum Job job, enum Fault kind, unsigned at, const char *label) {
    if (kind != NONE && (job == ACTION || job == INITIALIZE)) {
        action_cut = at;
        action_fault = kind;
    }
    if (kind != NONE) {
        scenario_cut = at;
        scenario_mode = kind;
    }
    run_label = label;
    fault = kind;
    cut = at;
    memset(report, 0, sizeof(*report));
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        child(job);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    int code = WEXITSTATUS(status);
    CHECK(code == 0 || code == CRASH_EXIT);
    if (kind == NONE)
        CHECK(code == 0);
    return code;
}
static uint64_t digest(const unsigned char *bytes) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < FLASH_SIZE; ++i) {
        h ^= bytes[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}
static unsigned recovery_matrix(const unsigned char *damaged, int ack) {
    memcpy(flash, damaged, FLASH_SIZE);
    require_new = ack;
    run(READONLY_RECOVER, NONE, 0, "read-only-hot-journal");
    run(RECOVER, NONE, 0, "recover-trace");
    unsigned events = report->recovered_events;
    for (unsigned i = 1; i <= events; ++i)
        for (enum Fault mode = BEFORE; mode <= AFTER; ++mode) {
            memcpy(flash, damaged, FLASH_SIZE);
            run(RECOVER, mode, i, "recovery-cut");
            CHECK(report->hit);
            run(RECOVER, NONE, 0, "recover-after-second-cut");
        }
    return events * 4;
}
static void crash_matrix(const unsigned char *baseline) {
    unsigned char *damaged = malloc(FLASH_SIZE);
    CHECK(damaged);
    uint64_t seen[4096];
    int seen_ack[4096];
    unsigned seen_count = 0, second_cuts = 0;
    require_new = 0;
    memcpy(flash, baseline, FLASH_SIZE);
    run(ACTION, NONE, 0, "action-trace");
    unsigned events = report->events, action_events = report->action_events;
    printf("  %s trace: %u boundaries (%u program, %u erase, %u sync), action=%u\n", journal_mode,
           events, report->programs, report->erases, report->syncs, action_events);
    CHECK(events > 1 && report->programs && report->erases && report->syncs && report->ack);
    for (unsigned i = 1; i <= events; ++i)
        for (enum Fault mode = BEFORE; mode <= AFTER; ++mode) {
            memcpy(flash, baseline, FLASH_SIZE);
            run(ACTION, mode, i, "action-cut");
            CHECK(report->hit);
            int ack = (int)report->ack;
            memcpy(damaged, flash, FLASH_SIZE);
            uint64_t hash = digest(damaged);
            int duplicate = 0;
            for (unsigned s = 0; s < seen_count; ++s)
                if (seen[s] == hash && seen_ack[s] == ack)
                    duplicate = 1;
            if (!duplicate) {
                CHECK(seen_count < sizeof(seen) / sizeof(*seen));
                seen_ack[seen_count] = ack;
                seen[seen_count++] = hash;
                second_cuts += recovery_matrix(damaged, ack);
            } else {
                require_new = ack;
                run(RECOVER, NONE, 0, "recover-action-cut");
            }
        }
    for (unsigned i = 1; i <= action_events; ++i)
        for (enum Fault mode = ERROR_BEFORE; mode <= ERROR_AFTER; ++mode) {
            memcpy(flash, baseline, FLASH_SIZE);
            require_new = 0;
            run(ACTION, mode, i, "io-error");
            CHECK(report->hit);
            CHECK(report->result != SQLITE_OK);
            CHECK(report->ack == 0);
            run(RECOVER, NONE, 0, "recover-io-error");
        }
    printf("  PASS %s: %u action cuts, %u recovery cuts, %u IO errors; %u unique crash images\n",
           journal_mode, events * 4, second_cuts, action_events * 3, seen_count);
    free(damaged);
}
static void initialization_matrix(unsigned char *baseline) {
    memset(flash, 0xff, FLASH_SIZE);
    run(PREPARE, NONE, 0, "prepare-existing-filesystem");
    memcpy(baseline, flash, FLASH_SIZE);
    run(INITIALIZE, NONE, 0, "initialize-trace");
    unsigned events = report->events;
    allow_empty = 1;
    for (unsigned i = 1; i <= events; ++i)
        for (enum Fault mode = BEFORE; mode <= AFTER; ++mode) {
            memcpy(flash, baseline, FLASH_SIZE);
            run(INITIALIZE, mode, i, "initialize-cut");
            CHECK(report->hit);
            unsigned ack = report->ack;
            run(RECOVER, NONE, 0, "recover-initialization");
            if (ack)
                CHECK(report->schema_present);
        }
    allow_empty = 0;
    printf("  PASS first database creation: %u cuts, existing files preserved\n", events * 4);
}
int main(int argc, char **argv) {
    int do_api = 1, do_crash = 1, targeted = 0;
    unsigned target_cut = 0;
    enum Fault target_mode = NONE;
    if (argc == 2 && strcmp(argv[1], "--quick") == 0)
        do_crash = 0;
    else if (argc == 2 && strcmp(argv[1], "--crash") == 0)
        do_api = 0;
    else if (argc == 5 && strcmp(argv[1], "--fault") == 0) {
        targeted = 1;
        target_cut = (unsigned)strtoul(argv[2], NULL, 10);
        target_mode = (enum Fault)strtoul(argv[3], NULL, 10);
        journal_mode = argv[4];
        if (!target_cut || target_mode < BEFORE || target_mode > ERROR_AFTER)
            return 2;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--quick|--crash|--fault CUT MODE JOURNAL_MODE]\n", argv[0]);
        return 2;
    }
    flash = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(flash != MAP_FAILED);
    report = mmap(NULL, sizeof(*report), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(report != MAP_FAILED);
    unsigned char *baseline = malloc(FLASH_SIZE);
    CHECK(baseline);
    printf("SQLite %s / littlefs 0x%08x: %u-byte NOR, fixed %u-byte SQLite heap\n",
           sqlite3_libversion(), LFS_VERSION, FLASH_SIZE, HEAP_SIZE);
    if (targeted) {
        memset(flash, 0xff, FLASH_SIZE);
        run(SEED, NONE, 0, "target-seed");
        run(ACTION, target_mode, target_cut, "target-action");
        CHECK(report->hit);
        require_new = (int)report->ack;
        run(RECOVER, NONE, 0, "target-recovery");
        free(baseline);
        CHECK(munmap(flash, FLASH_SIZE) == 0);
        CHECK(munmap(report, sizeof(*report)) == 0);
        puts("PASS targeted fault and recovery");
        return 0;
    }
    if (do_crash)
        initialization_matrix(baseline);
    const char *modes[] = {"DELETE", "TRUNCATE", "PERSIST"};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        journal_mode = modes[i];
        memset(flash, 0xff, FLASH_SIZE);
        require_new = 0;
        run(SEED, NONE, 0, "seed");
        memcpy(baseline, flash, FLASH_SIZE);
        if (do_api)
            run(API_CHECKS, NONE, 0, "api-contracts");
        memcpy(flash, baseline, FLASH_SIZE);
        if (do_crash)
            crash_matrix(baseline);
        if (!do_api)
            continue;
        memcpy(flash, baseline, FLASH_SIZE);
        require_new = 0;
        run(READONLY_RECOVER, NONE, 0, "read-only");
        memcpy(flash, baseline, FLASH_SIZE);
        require_new = 0;
        run(READ_ERROR, NONE, 0, "read-error");
        memcpy(flash, baseline, FLASH_SIZE);
        run(FILL, NONE, 0, "disk-full");
        run(RECOVER, NONE, 0, "recover-full");
        run(RECLAIM, NONE, 0, "reclaim-space");
        require_new = 1;
        run(RECOVER, NONE, 0, "recover-after-reclaim");
        printf("  PASS %s: SQL, rollback, index consistency, EOF, path/connection restrictions, "
               "OOM, read errors, real disk full and space reclamation\n",
               journal_mode);
    }
    free(baseline);
    CHECK(munmap(flash, FLASH_SIZE) == 0);
    CHECK(munmap(report, sizeof(*report)) == 0);
    puts("PASS: acknowledged transactions durable; interrupted transactions atomic; sentinel files "
         "preserved");
    return 0;
}
