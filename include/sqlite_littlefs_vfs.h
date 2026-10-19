/* SPDX-License-Identifier: MIT */
#ifndef SQLITE_LITTLEFS_VFS_H
#define SQLITE_LITTLEFS_VFS_H

#include "sqlite3.h"
#include "lfs.h"

/* A failed littlefs lock callback can make close return without unlinking its
 * file object. SQLite frees that object regardless of xClose's result. This
 * adapter therefore requires external single-owner serialization instead. */
#ifdef LFS_THREADSAFE
#error "sqlite-littlefs-vfs does not support LFS_THREADSAFE; serialize through one owner"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* These affect object layout. Compile the library and every caller with the
 * same values. The mounted filesystem's cache_size must not exceed this. */
#ifndef SQLITE_LFS_VFS_CACHE_MAX
#define SQLITE_LFS_VFS_CACHE_MAX 256
#endif
#ifndef SQLITE_LFS_VFS_PATH_MAX
#define SQLITE_LFS_VFS_PATH_MAX 128
#endif
#ifndef SQLITE_LFS_VFS_NAME_MAX
#define SQLITE_LFS_VFS_NAME_MAX 32
#endif

typedef struct sqlite3_lfs_vfs_config {
    lfs_t *lfs;          /* Already mounted; the library never mounts/formats. */
    const char *name;    /* Unique VFS name; copied during init. */
    const char *db_path; /* Canonical absolute path; copied during init. */
    void *context;
    /* Required: return the number of bytes supplied (0..count). The caller
     * supplies real production entropy; tests may supply deterministic data. */
    int (*randomness)(void *context, unsigned char *out, int count);
    /* Optional: return 0 and Julian milliseconds on success, nonzero on
     * failure. NULL means SQL date('now') is unavailable, not fake UTC. */
    int (*current_time_ms)(void *context, sqlite3_int64 *julian_ms);
    int sector_size; /* 0 defaults to 4096; power of two 512..65536. */
    int read_only;   /* Requires SQLITE_OPEN_READONLY when opening. */
} sqlite3_lfs_vfs_config;

/* Caller-owned, zero-initialized object; treat underscore members as private.
 * Keep this object, lfs, its configuration/buffers, and callback context alive
 * until successful unregister. All access to the VFS and filesystem must be
 * serialized by one owner; this library supplies no thread/core mutexes.
 * Do not modify, rename, or remove its database/journal through other APIs. */
typedef struct sqlite3_lfs_vfs {
    sqlite3_vfs base;
    sqlite3_lfs_vfs_config _config;
    struct sqlite3_lfs_vfs *_next;
    sqlite3_file *_db_file;
    unsigned int _magic;
    unsigned int _open_handles;
    int _journal_open;
    int _registered;
    int _last_lfs_error;
    int _last_sqlite_error;
    int _fault_lfs_error;
    char _name[SQLITE_LFS_VFS_NAME_MAX + 1];
    char _db_path[SQLITE_LFS_VFS_PATH_MAX + 1];
    char _journal_path[SQLITE_LFS_VFS_PATH_MAX + 1];
} sqlite3_lfs_vfs;

/* Return SQLite result codes. init validates without touching storage. It
 * permits I/O only to db_path and db_path-journal. Stock SQLite's -wal/-shm
 * existence probes are read-only; an existing WAL cannot be opened/ignored.
 * SQLite allocates each fixed-size file object from its configured allocator;
 * the object contains its own littlefs cache (lfs_file_opencfg, no malloc).
 * Configure SQLite temporary storage in memory and busy_timeout=0. The
 * sleep callback does not block an executor. WAL is not supported.
 * Use rollback journal DELETE/TRUNCATE/PERSIST and synchronous=EXTRA.
 * A mounted block backend must implement durable sync, not merely a CPU fence. */
int sqlite3_lfs_vfs_init(sqlite3_lfs_vfs *vfs, const sqlite3_lfs_vfs_config *config);
int sqlite3_lfs_vfs_register(sqlite3_lfs_vfs *vfs, int make_default);
/* Returns SQLITE_BUSY without unregistering while any file is still open. */
int sqlite3_lfs_vfs_unregister(sqlite3_lfs_vfs *vfs);

/* Sticky diagnostics for the most recent failure, reset by successful init.
 * lfs_error is zero for failures rejected before calling littlefs. */
int sqlite3_lfs_vfs_last_lfs_error(const sqlite3_lfs_vfs *vfs);
int sqlite3_lfs_vfs_last_sqlite_error(const sqlite3_lfs_vfs *vfs);

/* Backend I/O, corruption, or capacity failures can leave littlefs caches in
 * a state that must not be retried. All VFS instances sharing that lfs_t are
 * latched closed to further I/O. Close connections, unregister all instances,
 * unmount/remount LittleFS, then call init/register again. Never clear this by
 * reinitializing the VFS without remounting. Poisoned close only unlinks the
 * handle using LFS_F_ERRED; this is qualified against LittleFS 2.11.3. */
int sqlite3_lfs_vfs_needs_remount(const sqlite3_lfs_vfs *vfs);

#ifdef __cplusplus
}
#endif
#endif
