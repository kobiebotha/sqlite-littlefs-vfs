/* SPDX-License-Identifier: MIT
 * Minimal integration with a caller-mounted LittleFS volume. This is not a
 * flash driver. Configure SQLite's heap before invoking this function.
 * Supply stable caller-owned storage, zero-initialized on first use:
 *     sqlite3_lfs_vfs vfs = {0};
 * On failure, inspect sqlite3_lfs_vfs_needs_remount(vfs) before any further
 * filesystem access; cleanup below does not clear a storage fault. */
#include "sqlite_littlefs_vfs.h"

int example_open_database(lfs_t *filesystem, int (*randomness)(void *, unsigned char *, int),
                          void *random_context, sqlite3_lfs_vfs *vfs, sqlite3 **database) {
    sqlite3_lfs_vfs_config config = {0};
    config.lfs = filesystem;
    config.name = "example-lfs";
    config.db_path = "/example.sqlite";
    config.context = random_context;
    config.randomness = randomness;
    config.sector_size = 4096;
    *database = 0;
    int rc = sqlite3_lfs_vfs_init(vfs, &config);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_lfs_vfs_register(vfs, 0);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_open_v2(config.db_path, database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         config.name);
    if (rc == SQLITE_OK) {
        rc = sqlite3_exec(*database,
                          "PRAGMA page_size=1024;"
                          "PRAGMA temp_store=MEMORY;"
                          "PRAGMA journal_mode=DELETE;"
                          "PRAGMA synchronous=EXTRA;"
                          "PRAGMA locking_mode=NORMAL;",
                          0, 0, 0);
    }
    if (rc != SQLITE_OK) {
        /* No statements have escaped, so close cannot be BUSY here. */
        (void)sqlite3_close(*database);
        *database = 0;
        (void)sqlite3_lfs_vfs_unregister(vfs);
    }
    return rc;
}

int example_close_database(sqlite3_lfs_vfs *vfs, sqlite3 *database) {
    /* Finalize every prepared statement before closing. On BUSY the owner
     * must retain both objects and complete cleanup; do not unmount storage. */
    int rc = sqlite3_close(database);
    return rc == SQLITE_OK ? sqlite3_lfs_vfs_unregister(vfs) : rc;
}
