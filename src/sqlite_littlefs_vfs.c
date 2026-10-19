/* SPDX-License-Identifier: MIT */
#include "sqlite_littlefs_vfs.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define LFS_VFS_MAGIC 0x4c465356u
#define JOURNAL_SUFFIX "-journal"

/* No allocator or OS layer is needed here. SQLite supplies the whole file
 * object; littlefs uses the cache through lfs_file_opencfg. */
typedef struct lfs_sqlite_file {
    sqlite3_file base;
    sqlite3_lfs_vfs *vfs;
    lfs_file_t file;
    struct lfs_file_config config;
    int is_database;
    int read_only;
    int lock_level;
    unsigned char cache[SQLITE_LFS_VFS_CACHE_MAX];
} lfs_sqlite_file;

/* The documented single owner serializes this registry as well as I/O. It
 * prevents registering aliases that bypass the single-connection guard. */
static sqlite3_lfs_vfs *registered_vfs;

static int failure(sqlite3_lfs_vfs *vfs, int sqlite_error, int lfs_error) {
    vfs->_last_lfs_error = lfs_error;
    vfs->_last_sqlite_error = sqlite_error;
    return sqlite_error;
}

static void latch_fault(sqlite3_lfs_vfs *vfs, int error) {
    if (!vfs->_fault_lfs_error)
        vfs->_fault_lfs_error = error;
    for (sqlite3_lfs_vfs *p = registered_vfs; p; p = p->_next) {
        if (p->_config.lfs == vfs->_config.lfs && !p->_fault_lfs_error)
            p->_fault_lfs_error = error;
    }
}

static int fault_failure(sqlite3_lfs_vfs *vfs, int code) {
    return failure(vfs, code, vfs->_fault_lfs_error);
}

static int lfs_failure(sqlite3_lfs_vfs *vfs, int error, int fallback) {
    /* These namespace/argument errors do not indicate an interrupted device
     * operation. Treat every other negative result conservatively, including
     * NOSPC: the global program cache may still contain an unfinished commit.
     * NAMETOOLONG is fatal too: littlefs can translate a create-commit NOSPC
     * into that code after attempting metadata writes. */
    if (error < 0 && error != LFS_ERR_NOENT && error != LFS_ERR_EXIST && error != LFS_ERR_NOTDIR &&
        error != LFS_ERR_ISDIR && error != LFS_ERR_NOTEMPTY && error != LFS_ERR_BADF &&
        error != LFS_ERR_INVAL && error != LFS_ERR_NOATTR)
        latch_fault(vfs, error);
    int code = fallback;
    if (error == LFS_ERR_NOSPC || error == LFS_ERR_FBIG)
        code = SQLITE_FULL;
    else if (error == LFS_ERR_NOMEM)
        code = SQLITE_NOMEM;
#ifdef SQLITE_IOERR_CORRUPTFS
    else if (error == LFS_ERR_CORRUPT)
        code = SQLITE_IOERR_CORRUPTFS;
#endif
    return failure(vfs, code, error);
}

static sqlite3_lfs_vfs *owner(sqlite3_vfs *base) {
    return (sqlite3_lfs_vfs *)base->pAppData;
}

/* No relative paths, empty components, dot components, or aliases. */
static int canonical_path(const char *path) {
    if (!path || path[0] != '/' || path[1] == '\0')
        return 0;
    const char *component = path + 1;
    for (const char *p = component;; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - component);
            if (!n || (n == 1 && component[0] == '.') ||
                (n == 2 && component[0] == '.' && component[1] == '.'))
                return 0;
            if (!*p)
                return 1;
            component = p + 1;
        }
    }
}

/* 1 main database, 2 rollback journal, 0 denied. */
static int path_kind(const sqlite3_lfs_vfs *vfs, const char *path) {
    if (!path)
        return 0;
    if (strcmp(path, vfs->_db_path) == 0)
        return 1;
    if (strcmp(path, vfs->_journal_path) == 0)
        return 2;
    return 0;
}

static int is_sidecar(const char *path, const char *database) {
    size_t n = strlen(database);
    return strncmp(path, database, n) == 0 &&
           (strcmp(path + n, JOURNAL_SUFFIX) == 0 || strcmp(path + n, "-wal") == 0 ||
            strcmp(path + n, "-shm") == 0);
}

static int valid_range(lfs_sqlite_file *f, sqlite3_int64 offset, int amount) {
    sqlite3_int64 max = f->vfs->_config.lfs->file_max;
    if (max > INT32_MAX)
        max = INT32_MAX;
    return offset >= 0 && amount >= 0 && offset <= max && amount <= max - offset;
}

static int file_close(sqlite3_file *base) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    sqlite3_lfs_vfs *vfs = f->vfs;
    if (vfs->_fault_lfs_error) {
        /* LittleFS 2.11.3's official close skips all sync/flush work for an
         * ERRED file, then unlinks it from mlist. Do not reset its global cache
         * or retry a partial program operation. The caller must remount. */
        f->file.flags |= LFS_F_ERRED;
    }
    int rc = lfs_file_close(vfs->_config.lfs, &f->file);
    /* littlefs removes the handle even when its final sync fails. */
    if (f->is_database)
        vfs->_db_file = 0;
    else
        vfs->_journal_open = 0;
    --vfs->_open_handles;
    base->pMethods = 0;
    return rc < 0 ? lfs_failure(vfs, rc, SQLITE_IOERR_CLOSE) : SQLITE_OK;
}

static int file_read(sqlite3_file *base, void *buffer, int amount, sqlite3_int64 offset) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (amount > 0)
        memset(buffer, 0, (size_t)amount);
    if (f->vfs->_fault_lfs_error)
        return fault_failure(f->vfs, SQLITE_IOERR_READ);
    if (!valid_range(f, offset, amount))
        return failure(f->vfs, SQLITE_IOERR_READ, 0);
    if (amount == 0)
        return SQLITE_OK;
    lfs_soff_t pos = lfs_file_seek(f->vfs->_config.lfs, &f->file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (pos < 0)
        return lfs_failure(f->vfs, pos, SQLITE_IOERR_SEEK);
    if (pos != offset)
        return failure(f->vfs, SQLITE_IOERR_SEEK, 0);
    lfs_ssize_t got = lfs_file_read(f->vfs->_config.lfs, &f->file, buffer, (lfs_size_t)amount);
    if (got < 0)
        return lfs_failure(f->vfs, got, SQLITE_IOERR_READ);
    if (got != amount) {
        /* Explicitly zero the missing tail even if a future backend touched it. */
        if (got >= 0 && got < amount)
            memset((unsigned char *)buffer + got, 0, (size_t)(amount - got));
        return failure(f->vfs, SQLITE_IOERR_SHORT_READ, 0);
    }
    return SQLITE_OK;
}

static int file_write(sqlite3_file *base, const void *buffer, int amount, sqlite3_int64 offset) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (f->vfs->_fault_lfs_error)
        return fault_failure(f->vfs, SQLITE_IOERR_WRITE);
    if (f->read_only)
        return failure(f->vfs, SQLITE_READONLY, 0);
    if (!valid_range(f, offset, amount))
        return failure(f->vfs, SQLITE_FULL, 0);
    if (amount == 0)
        return SQLITE_OK;
    lfs_soff_t pos = lfs_file_seek(f->vfs->_config.lfs, &f->file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (pos < 0)
        return lfs_failure(f->vfs, pos, SQLITE_IOERR_SEEK);
    if (pos != offset)
        return failure(f->vfs, SQLITE_IOERR_SEEK, 0);
    lfs_ssize_t wrote = lfs_file_write(f->vfs->_config.lfs, &f->file, buffer, (lfs_size_t)amount);
    if (wrote < 0)
        return lfs_failure(f->vfs, wrote, SQLITE_IOERR_WRITE);
    if (wrote != amount)
        return failure(f->vfs, SQLITE_IOERR_WRITE, 0);
    return SQLITE_OK;
}

static int file_truncate(sqlite3_file *base, sqlite3_int64 size) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (f->vfs->_fault_lfs_error)
        return fault_failure(f->vfs, SQLITE_IOERR_TRUNCATE);
    if (f->read_only)
        return failure(f->vfs, SQLITE_READONLY, 0);
    if (!valid_range(f, size, 0))
        return failure(f->vfs, SQLITE_FULL, 0);
    int rc = lfs_file_truncate(f->vfs->_config.lfs, &f->file, (lfs_off_t)size);
    if (rc < 0)
        return lfs_failure(f->vfs, rc, SQLITE_IOERR_TRUNCATE);
    /* A zero-length journal is a commit marker. Commit its metadata here,
     * including when a SQLite build/mode does not issue a subsequent xSync. */
    rc = lfs_file_sync(f->vfs->_config.lfs, &f->file);
    return rc < 0 ? lfs_failure(f->vfs, rc, SQLITE_IOERR_FSYNC) : SQLITE_OK;
}

static int file_sync(sqlite3_file *base, int flags) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    (void)flags; /* All SQLite sync modes receive a full littlefs commit. */
    if (f->vfs->_fault_lfs_error)
        return fault_failure(f->vfs, SQLITE_IOERR_FSYNC);
    int rc = lfs_file_sync(f->vfs->_config.lfs, &f->file);
    return rc < 0 ? lfs_failure(f->vfs, rc, SQLITE_IOERR_FSYNC) : SQLITE_OK;
}

static int file_size(sqlite3_file *base, sqlite3_int64 *size) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    *size = 0;
    if (f->vfs->_fault_lfs_error)
        return fault_failure(f->vfs, SQLITE_IOERR_FSTAT);
    lfs_soff_t rc = lfs_file_size(f->vfs->_config.lfs, &f->file);
    if (rc < 0)
        return lfs_failure(f->vfs, rc, SQLITE_IOERR_FSTAT);
    *size = rc;
    return SQLITE_OK;
}

static int file_lock(sqlite3_file *base, int level) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (!f->is_database || level < SQLITE_LOCK_SHARED || level > SQLITE_LOCK_EXCLUSIVE)
        return failure(f->vfs, SQLITE_IOERR_LOCK, 0);
    if (level > f->lock_level)
        f->lock_level = level;
    return SQLITE_OK;
}

static int file_unlock(sqlite3_file *base, int level) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (!f->is_database || (level != SQLITE_LOCK_NONE && level != SQLITE_LOCK_SHARED))
        return failure(f->vfs, SQLITE_IOERR_UNLOCK, 0);
    if (level < f->lock_level)
        f->lock_level = level;
    return SQLITE_OK;
}

static int file_reserved(sqlite3_file *base, int *reserved) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    lfs_sqlite_file *db = (lfs_sqlite_file *)f->vfs->_db_file;
    *reserved = db && db->lock_level >= SQLITE_LOCK_RESERVED;
    return SQLITE_OK;
}

static int file_control(sqlite3_file *base, int operation, void *argument) {
    lfs_sqlite_file *f = (lfs_sqlite_file *)base;
    if (operation == SQLITE_FCNTL_LOCKSTATE) {
        *(int *)argument = f->lock_level;
        return SQLITE_OK;
    }
    if (operation == SQLITE_FCNTL_PRAGMA) {
        char **args = (char **)argument;
        if (args[1] && args[2] && sqlite3_stricmp(args[1], "journal_mode") == 0 &&
            sqlite3_stricmp(args[2], "wal") == 0) {
            args[0] = sqlite3_mprintf("WAL is unsupported by this littlefs VFS");
            return failure(f->vfs, args[0] ? SQLITE_ERROR : SQLITE_NOMEM, 0);
        }
    }
    /* NOTFOUND is required for unimplemented controls. In particular, never
     * claim atomic writes, powersafe overwrite, or batch atomic support. */
    return SQLITE_NOTFOUND;
}

static int file_sector_size(sqlite3_file *base) {
    return ((lfs_sqlite_file *)base)->vfs->_config.sector_size;
}

static int file_characteristics(sqlite3_file *base) {
    (void)base;
    return 0;
}

static const sqlite3_io_methods file_methods = {
    .iVersion = 1,
    .xClose = file_close,
    .xRead = file_read,
    .xWrite = file_write,
    .xTruncate = file_truncate,
    .xSync = file_sync,
    .xFileSize = file_size,
    .xLock = file_lock,
    .xUnlock = file_unlock,
    .xCheckReservedLock = file_reserved,
    .xFileControl = file_control,
    .xSectorSize = file_sector_size,
    .xDeviceCharacteristics = file_characteristics,
};

static int vfs_open(sqlite3_vfs *base, const char *path, sqlite3_file *file, int flags,
                    int *out_flags) {
    sqlite3_lfs_vfs *vfs = owner(base);
    lfs_sqlite_file *f = (lfs_sqlite_file *)file;
    memset(f, 0, sizeof(*f));
    if (out_flags)
        *out_flags = 0;
    if (vfs->_fault_lfs_error)
        return fault_failure(vfs, SQLITE_IOERR);
    int kind = path_kind(vfs, path);
    int file_type = flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL | SQLITE_OPEN_TEMP_DB |
                             SQLITE_OPEN_TEMP_JOURNAL | SQLITE_OPEN_TRANSIENT_DB |
                             SQLITE_OPEN_SUBJOURNAL | SQLITE_OPEN_SUPER_JOURNAL | SQLITE_OPEN_WAL);
    int access = flags & (SQLITE_OPEN_READONLY | SQLITE_OPEN_READWRITE);
    if (!vfs->_registered || !kind || (kind == 1 && file_type != SQLITE_OPEN_MAIN_DB) ||
        (kind == 2 && file_type != SQLITE_OPEN_MAIN_JOURNAL) ||
        (access != SQLITE_OPEN_READONLY && access != SQLITE_OPEN_READWRITE) ||
        (flags & SQLITE_OPEN_DELETEONCLOSE) ||
        ((flags & SQLITE_OPEN_CREATE) && access != SQLITE_OPEN_READWRITE) ||
        ((flags & SQLITE_OPEN_EXCLUSIVE) && !(flags & SQLITE_OPEN_CREATE)))
        return failure(vfs, SQLITE_CANTOPEN, 0);
    if (vfs->_config.read_only && access == SQLITE_OPEN_READWRITE)
        return failure(vfs, SQLITE_READONLY, 0);
    if ((kind == 1 && vfs->_db_file) || (kind == 2 && vfs->_journal_open))
        return failure(vfs, SQLITE_BUSY, 0);
    if (kind == 2 && !vfs->_db_file)
        return failure(vfs, SQLITE_CANTOPEN, 0);

    if (kind == 1) {
        /* OMIT_WAL SQLite does not probe for an existing WAL. Never let that
         * profile silently ignore acknowledged transactions from another build. */
        char wal_path[SQLITE_LFS_VFS_PATH_MAX + 1];
        strcpy(wal_path, vfs->_db_path);
        strcat(wal_path, "-wal");
        struct lfs_info info;
        int rc = lfs_stat(vfs->_config.lfs, wal_path, &info);
        if (rc == 0)
            return failure(vfs, SQLITE_CANTOPEN, 0);
        if (rc != LFS_ERR_NOENT)
            return lfs_failure(vfs, rc, SQLITE_CANTOPEN);
    }

    int lfs_flags = access == SQLITE_OPEN_READONLY ? LFS_O_RDONLY : LFS_O_RDWR;
    if (flags & SQLITE_OPEN_CREATE)
        lfs_flags |= LFS_O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE)
        lfs_flags |= LFS_O_EXCL;
    f->vfs = vfs;
    f->is_database = kind == 1;
    f->read_only = access == SQLITE_OPEN_READONLY;
    f->config.buffer = f->cache;
    int rc = lfs_file_opencfg(vfs->_config.lfs, &f->file, path, lfs_flags, &f->config);
    if (rc < 0)
        return lfs_failure(vfs, rc, SQLITE_CANTOPEN);
    ++vfs->_open_handles;
    if (kind == 1)
        vfs->_db_file = file;
    else
        vfs->_journal_open = 1;
    file->pMethods = &file_methods;
    if (out_flags)
        *out_flags = flags;
    return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *base, const char *path, int sync_dir) {
    sqlite3_lfs_vfs *vfs = owner(base);
    int kind = path_kind(vfs, path);
    (void)sync_dir;
    if (vfs->_fault_lfs_error)
        return fault_failure(vfs, SQLITE_IOERR_DELETE);
    if (!kind)
        return failure(vfs, SQLITE_IOERR_DELETE, 0);
    if (vfs->_config.read_only)
        return failure(vfs, SQLITE_READONLY, 0);
    if ((kind == 1 && vfs->_db_file) || (kind == 2 && vfs->_journal_open))
        return failure(vfs, SQLITE_IOERR_DELETE, 0);
    /* littlefs remove commits metadata and calls the block-device sync before
     * returning. Thus sync_dir=1 is fulfilled too; it is never a deferred unlink. */
    int rc = lfs_remove(vfs->_config.lfs, path);
    if (rc == LFS_ERR_NOENT)
        return SQLITE_OK;
    return rc < 0 ? lfs_failure(vfs, rc, SQLITE_IOERR_DELETE) : SQLITE_OK;
}

static int vfs_access(sqlite3_vfs *base, const char *path, int flags, int *exists) {
    sqlite3_lfs_vfs *vfs = owner(base);
    *exists = 0;
    if (vfs->_fault_lfs_error)
        return fault_failure(vfs, SQLITE_IOERR_ACCESS);
    size_t db_length = strlen(vfs->_db_path);
    /* Stock SQLite probes for a surviving WAL before opening a rollback-mode
     * database. Inspect these sidecars read-only: pretending a real WAL does
     * not exist could hide committed data. Opening/creating them is refused. */
    int wal_probe =
        path && strncmp(path, vfs->_db_path, db_length) == 0 &&
        (strcmp(path + db_length, "-wal") == 0 || strcmp(path + db_length, "-shm") == 0);
    if ((!path_kind(vfs, path) && !wal_probe) ||
        (flags != SQLITE_ACCESS_EXISTS && flags != SQLITE_ACCESS_READ &&
         flags != SQLITE_ACCESS_READWRITE))
        return failure(vfs, SQLITE_IOERR_ACCESS, 0);
    struct lfs_info info;
    int rc = lfs_stat(vfs->_config.lfs, path, &info);
    if (rc == LFS_ERR_NOENT)
        return SQLITE_OK;
    if (rc < 0)
        return lfs_failure(vfs, rc, SQLITE_IOERR_ACCESS);
    if (info.type != LFS_TYPE_REG)
        return failure(vfs, SQLITE_IOERR_ACCESS, 0);
    *exists = !(flags == SQLITE_ACCESS_READWRITE && vfs->_config.read_only);
    return SQLITE_OK;
}

static int vfs_full_path(sqlite3_vfs *base, const char *path, int capacity, char *out) {
    sqlite3_lfs_vfs *vfs = owner(base);
    if (capacity > 0)
        out[0] = '\0';
    if (!path_kind(vfs, path) || capacity <= 0 || strlen(path) >= (size_t)capacity)
        return failure(vfs, SQLITE_CANTOPEN, 0);
    memcpy(out, path, strlen(path) + 1);
    return SQLITE_OK;
}

static void *vfs_dl_open(sqlite3_vfs *base, const char *name) {
    (void)name;
    failure(owner(base), SQLITE_ERROR, 0);
    return 0;
}

static void vfs_dl_error(sqlite3_vfs *base, int capacity, char *out) {
    (void)base;
    if (capacity > 0)
        sqlite3_snprintf(capacity, out, "Dynamic extensions are unsupported");
}

static void (*vfs_dl_sym(sqlite3_vfs *base, void *handle, const char *symbol))(void) {
    (void)base;
    (void)handle;
    (void)symbol;
    return 0;
}

static void vfs_dl_close(sqlite3_vfs *base, void *handle) {
    (void)base;
    (void)handle;
}

static int vfs_randomness(sqlite3_vfs *base, int count, char *out) {
    sqlite3_lfs_vfs *vfs = owner(base);
    if (count <= 0)
        return 0;
    memset(out, 0, (size_t)count);
    int result = vfs->_config.randomness(vfs->_config.context, (unsigned char *)out, count);
    if (result < 0 || result > count) {
        memset(out, 0, (size_t)count);
        failure(vfs, SQLITE_ERROR, 0);
        return 0;
    }
    return result;
}

static int vfs_sleep(sqlite3_vfs *base, int microseconds) {
    (void)base;
    (void)microseconds;
    /* No OS/executor blocking. Configure busy_timeout=0 and retry in the
     * owning application's request loop, if it chooses to retry at all. */
    return 0;
}

static int vfs_time64(sqlite3_vfs *base, sqlite3_int64 *julian_ms) {
    sqlite3_lfs_vfs *vfs = owner(base);
    *julian_ms = 0;
    if (!vfs->_config.current_time_ms ||
        vfs->_config.current_time_ms(vfs->_config.context, julian_ms) != 0) {
        *julian_ms = 0;
        return failure(vfs, SQLITE_ERROR, 0);
    }
    return SQLITE_OK;
}

static int vfs_time(sqlite3_vfs *base, double *julian_date) {
    sqlite3_int64 ms;
    int rc = vfs_time64(base, &ms);
    *julian_date = (double)ms / 86400000.0;
    return rc;
}

static int vfs_last_error(sqlite3_vfs *base, int capacity, char *out) {
    sqlite3_lfs_vfs *vfs = owner(base);
    if (capacity > 0)
        sqlite3_snprintf(capacity, out, "littlefs=%d sqlite=%d", vfs->_last_lfs_error,
                         vfs->_last_sqlite_error);
    return vfs->_last_lfs_error;
}

int sqlite3_lfs_vfs_init(sqlite3_lfs_vfs *vfs, const sqlite3_lfs_vfs_config *config) {
    if (!vfs)
        return SQLITE_MISUSE;
    if (vfs->_registered || vfs->_open_handles)
        return failure(vfs, SQLITE_BUSY, 0);
    if (!config || !config->lfs || !config->lfs->cfg || !config->lfs->cfg->read ||
        !config->lfs->cfg->prog || !config->lfs->cfg->erase || !config->lfs->cfg->sync ||
        !config->lfs->cfg->cache_size || config->lfs->cfg->cache_size > SQLITE_LFS_VFS_CACHE_MAX ||
        !config->randomness || !config->name || !config->name[0] ||
        strlen(config->name) > SQLITE_LFS_VFS_NAME_MAX || !canonical_path(config->db_path) ||
        strlen(config->db_path) + sizeof(JOURNAL_SUFFIX) - 1 > SQLITE_LFS_VFS_PATH_MAX)
        return failure(vfs, SQLITE_MISUSE, 0);
    int sector = config->sector_size ? config->sector_size : 4096;
    if (sector < 512 || sector > 65536 || (sector & (sector - 1)))
        return failure(vfs, SQLITE_MISUSE, 0);
    /* Names are copied so callers may release their input config/strings. */
    sqlite3_lfs_vfs_config saved = *config;
    char name[SQLITE_LFS_VFS_NAME_MAX + 1];
    char path[SQLITE_LFS_VFS_PATH_MAX + 1];
    memcpy(name, config->name, strlen(config->name) + 1);
    memcpy(path, config->db_path, strlen(config->db_path) + 1);
    memset(vfs, 0, sizeof(*vfs));
    vfs->_config = saved;
    strcpy(vfs->_name, name);
    strcpy(vfs->_db_path, path);
    strcpy(vfs->_journal_path, path);
    strcat(vfs->_journal_path, JOURNAL_SUFFIX);
    vfs->_config.name = vfs->_name;
    vfs->_config.db_path = vfs->_db_path;
    vfs->_config.sector_size = sector;
    vfs->_magic = LFS_VFS_MAGIC;
    vfs->base.iVersion = 2;
    vfs->base.szOsFile = sizeof(lfs_sqlite_file);
    vfs->base.mxPathname = SQLITE_LFS_VFS_PATH_MAX;
    vfs->base.zName = vfs->_name;
    vfs->base.pAppData = vfs;
    vfs->base.xOpen = vfs_open;
    vfs->base.xDelete = vfs_delete;
    vfs->base.xAccess = vfs_access;
    vfs->base.xFullPathname = vfs_full_path;
    vfs->base.xDlOpen = vfs_dl_open;
    vfs->base.xDlError = vfs_dl_error;
    vfs->base.xDlSym = vfs_dl_sym;
    vfs->base.xDlClose = vfs_dl_close;
    vfs->base.xRandomness = vfs_randomness;
    vfs->base.xSleep = vfs_sleep;
    vfs->base.xCurrentTime = vfs_time;
    vfs->base.xGetLastError = vfs_last_error;
    vfs->base.xCurrentTimeInt64 = vfs_time64;
    return SQLITE_OK;
}

int sqlite3_lfs_vfs_register(sqlite3_lfs_vfs *vfs, int make_default) {
    if (!vfs || vfs->_magic != LFS_VFS_MAGIC)
        return SQLITE_MISUSE;
    if (vfs->_registered || vfs->_open_handles)
        return failure(vfs, SQLITE_BUSY, 0);
    if (vfs->_fault_lfs_error)
        return fault_failure(vfs, SQLITE_IOERR);
    for (sqlite3_lfs_vfs *p = registered_vfs; p; p = p->_next) {
        if (p->_config.lfs == vfs->_config.lfs && p->_fault_lfs_error) {
            latch_fault(vfs, p->_fault_lfs_error);
            return fault_failure(vfs, SQLITE_IOERR);
        }
        if (p->_config.lfs == vfs->_config.lfs &&
            (strcmp(p->_db_path, vfs->_db_path) == 0 || is_sidecar(p->_db_path, vfs->_db_path) ||
             is_sidecar(vfs->_db_path, p->_db_path)))
            return failure(vfs, SQLITE_BUSY, 0);
    }
    /* sqlite3_vfs_register itself allows duplicate names; reject shadowing. */
    if (sqlite3_vfs_find(vfs->_name))
        return failure(vfs, SQLITE_BUSY, 0);
    int rc = sqlite3_vfs_register(&vfs->base, make_default != 0);
    if (rc != SQLITE_OK)
        return failure(vfs, rc, 0);
    vfs->_next = registered_vfs;
    registered_vfs = vfs;
    vfs->_registered = 1;
    return SQLITE_OK;
}

int sqlite3_lfs_vfs_unregister(sqlite3_lfs_vfs *vfs) {
    if (!vfs || vfs->_magic != LFS_VFS_MAGIC)
        return SQLITE_MISUSE;
    if (vfs->_open_handles)
        return failure(vfs, SQLITE_BUSY, 0);
    if (!vfs->_registered)
        return SQLITE_OK;
    int rc = sqlite3_vfs_unregister(&vfs->base);
    if (rc != SQLITE_OK)
        return failure(vfs, rc, 0);
    sqlite3_lfs_vfs **p = &registered_vfs;
    while (*p && *p != vfs)
        p = &(*p)->_next;
    if (*p)
        *p = vfs->_next;
    vfs->_next = 0;
    vfs->_registered = 0;
    return SQLITE_OK;
}

int sqlite3_lfs_vfs_last_lfs_error(const sqlite3_lfs_vfs *vfs) {
    return vfs ? vfs->_last_lfs_error : LFS_ERR_INVAL;
}

int sqlite3_lfs_vfs_last_sqlite_error(const sqlite3_lfs_vfs *vfs) {
    return vfs ? vfs->_last_sqlite_error : SQLITE_MISUSE;
}

int sqlite3_lfs_vfs_needs_remount(const sqlite3_lfs_vfs *vfs) {
    return vfs && vfs->_fault_lfs_error != 0;
}
