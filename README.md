# sqlite-littlefs-vfs

A portable SQLite VFS for [LittleFS](https://github.com/littlefs-project/littlefs)
on bare-metal systems.

The adapter calls LittleFS directly. It requires no POSIX filesystem, RTOS,
Rust allocator, or platform SDK. Applications supply an already-mounted
filesystem, entropy, and the SQLite memory configuration. It works through
SQLite's public VFS interface; neither SQLite nor LittleFS is forked.

**Status: experimental.** Host qualification covers a simulated NOR device.
Physical power cuts, brownouts, flash wear, and board-specific drivers require
separate testing. A successful build or SQL query does not establish durability.

## Scope

- One database and its rollback journal per VFS, with one open database connection.
- One serialized owner for SQLite and the mounted filesystem; no interrupt or
  second-core calls, and no second mount or other access to the database files.
- Fixed-size file caches, supplied to `lfs_file_opencfg`. The adapter itself does
  not call `malloc`; SQLite allocates its fixed-size file objects using its
  configured allocator. Tests use a 64 KiB `memsys5` heap with no malloc fallback.
- `xSync` calls `lfs_file_sync`. Journal deletion uses LittleFS's synchronous
  metadata commit. No unsupported atomic-write or powersafe-overwrite claims.
- Mounting, formatting, flash partitions, wear policy, and sleep are application
  responsibilities. This library cannot format a filesystem or choose a partition.

The recommended durability profile is `journal_mode=DELETE`,
`synchronous=EXTRA`, `locking_mode=NORMAL`, and memory-only temporary storage.
`TRUNCATE` and `PERSIST` journals are also implemented and tested. WAL,
shared-memory locking, concurrent connections, and disk-backed temporary files
are outside this version's scope. Do not disable journaling or syncing.

## Build and test

Requirements: Python 3, CMake 3.16+, and a C11 compiler. Host crash tests require
macOS or Linux because they discard all process state with `fork`/`_exit`.

```sh
python3 tools/fetch_deps.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The recovery matrix can take several minutes, especially with sanitizers.
The crash test timeout defaults to 900 seconds, or 3,600 seconds in a fresh
sanitizer build. Override the CMake cache setting with
`-DSQLITE_LITTLEFS_CRASH_TIMEOUT=3600` when reconfiguring an existing build.
The timeout does not reduce the fault matrix.
For just the API checks, use `ctest --test-dir build -R api --output-on-failure`.
To compile for Cortex-M33 using an installed Arm GNU toolchain, run
`sh tools/check_freestanding.sh`; this compiles objects and does not flash or
execute firmware.

The fetcher verifies SQLite 3.54.1 and LittleFS 2.11.3 using pinned checksums.
Fetched sources remain in ignored `vendor/`. To verify the cache without network
access, run `python3 tools/fetch_deps.py --check`.

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DSQLITE_LITTLEFS_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

The tests use synthetic filesystems and do not access physical devices. See
[DESIGN.md](DESIGN.md) for the storage contract and fault model, and
[QUALIFICATION.md](QUALIFICATION.md) for recorded test results and limitations.

## Use as a dependency

Compile `src/sqlite_littlefs_vfs.c` and add `include/`, the directory containing
`sqlite3.h`, and the directory containing `lfs.h` to your include path. Link
your SQLite and LittleFS builds. CMake exposes the target
`sqlite_littlefs_vfs::sqlite_littlefs_vfs`; set `SQLITE_LITTLEFS_BUILD_TESTS=OFF` for an embedded
consumer and provide `SQLITE_LITTLEFS_SQLITE_INCLUDE_DIR` and
`SQLITE_LITTLEFS_LITTLEFS_INCLUDE_DIR` if you are not using the test cache.

Compile LittleFS, the adapter, and every caller with matching LittleFS options,
particularly options that change public struct layouts. `LFS_NO_MALLOC` is
supported; `LFS_THREADSAFE` is rejected because a failed lock callback can
prevent safe handle cleanup. Serialize access through one owner. Use the same
`SQLITE_LFS_VFS_CACHE_MAX` across adapter and consumers if overriding its
256-byte default; the mounted filesystem's `cache_size` must fit. The adapter does not change the filesystem's on-disk version.

For an OS-free SQLite build, enable `SQLITE_OS_OTHER=1`, provide
`sqlite3_os_init()` / `sqlite3_os_end()`, and register this VFS explicitly.
Use `SQLITE_TEMP_STORE=3`. The tested OS-free profile omits WAL and
`SQLITE_OMIT_DESERIALIZE=1` disables the optional memdb VFS, allowing the
no-op OS initialization hooks in `tests/sqlite_port.c`. If retaining memdb,
register a default VFS from your OS initialization hook before memdb initializes.
The complete tested profile is in
[tests/sqlite_config.h](tests/sqlite_config.h). Applications may change SQLite
limits, but must requalify the resulting build and workload.

See [examples/mounted.c](examples/mounted.c) for registration and opening an
already-mounted volume. Zero-initialize VFS storage on first use
(`sqlite3_lfs_vfs vfs = {0};`). Keep the `sqlite3_lfs_vfs`, `lfs_t`, LittleFS config,
callback context, and underlying device alive until all connections are closed
and the VFS is unregistered. Create any parent directory before opening the
database. The adapter only accepts the configured absolute database pathname
and its `-journal` companion, with no path aliases. It also checks WAL
sidecars read-only and refuses to open a database with an existing `-wal`,
including in builds that omit WAL. Existing sidecars are preserved.

For `no_std` Rust, call this C ABI using bindings generated from
`include/sqlite_littlefs_vfs.h` with the same compile definitions as LittleFS.
Keep the VFS and filesystem at stable addresses, owned by one task. An Embassy
owner can receive requests through a channel and yield between transactions;
SQLite calls themselves remain synchronous. Set `busy_timeout=0`; the VFS
sleep callback does not block the executor. No Rust wrapper or board driver is
included in this initial library release.

## Durability boundary

Only report an action as committed after SQLite returns `SQLITE_OK` for
`COMMIT`. Store application state and its pending sync/outbox record in that
same transaction. After an interrupted commit, the transaction may be absent
or present; retry with a stable operation ID. A failed commit is not proof the
action is absent, so do not blindly repeat an increment.

LittleFS's `sync` callback must flush all buffered device writes to durable
media and make subsequent reads coherent. Propagate hardware errors. Do not
replace this callback with a no-op unless every program/erase is already
durable and no cache needs flushing. Never mount storage concurrently from
another core, firmware, or USB mass-storage host. Multiple VFS instances may
attach distinct database paths on the same mounted filesystem only under the
same serialized owner; overlapping database/sidecar paths are rejected. Close
all raw LittleFS file and directory handles before calling SQLite, even handles
for unrelated files. This makes it possible to discard the entire mounted
filesystem state after a storage fault without leaving raw handles to flush.

## Recovery after a device error

The adapter latches storage faults. Check `sqlite3_lfs_vfs_needs_remount()`
when an operation fails. Once a filesystem is faulted, every VFS attached to
it refuses further storage I/O. Closing handles discards their buffered state
instead of retrying writes through a possibly inconsistent LittleFS program cache.

Finalize statements, close all database connections, unregister every VFS on
that filesystem, unmount and remount LittleFS, then initialize/register fresh
VFS state and reopen the database. SQLite performs journal recovery on reopen.
Remounting is a caller obligation; the VFS cannot detect whether it happened.
Do not just clear the VFS object and continue using the same mounted state.
Do not operate on unrelated files on that filesystem until recovery completes.
All errors still reach the caller; a failed transaction must not be presented
as committed.

This cleanup relies on LittleFS's `LFS_F_ERRED` close behavior in the pinned
2.11.3 version: it unlinks a failed handle without syncing it. Requalify that
behavior when upgrading LittleFS. The adapter does not modify LittleFS source
or attempt to reset its internal program cache.

## Prior art and license

[pico-sqlite](https://github.com/oyama/pico-sqlite) already runs SQLite on
LittleFS through the Pico SDK's POSIX adaptation, and
[sqlite-rs-embedded](https://github.com/vlcn-io/sqlite-rs-embedded) provides
`no_std` Rust bindings. This project focuses on a small direct VFS and a
reproducible recovery test suite. It does not claim to be the first SQLite
port for microcontrollers.

Original code is [MIT licensed](LICENSE). LittleFS uses BSD-3-Clause and
SQLite is [public domain](https://sqlite.org/copyright.html). Dependency
sources and their notices are fetched separately and are not part of the
library's MIT grant.
