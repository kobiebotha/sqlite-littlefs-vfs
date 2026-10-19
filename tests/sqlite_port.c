/* SQLITE_OS_OTHER initialization for the host tests. Embedded applications
 * provide these two entry points themselves. The VFS registers explicitly. */
#include "sqlite3.h"
int sqlite3_os_init(void) {
    return SQLITE_OK;
}
int sqlite3_os_end(void) {
    return SQLITE_OK;
}
