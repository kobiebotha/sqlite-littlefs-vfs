# Storage contract and qualification

## Layer boundaries

```text
one SQLite owner
    │  database + rollback journal
    ▼
sqlite-littlefs-vfs
    │  lfs_file_read/write/sync/truncate, lfs_remove
    ▼
caller-mounted LittleFS
    │  read / program / erase / sync
    ▼
caller-owned block device
```

The library accepts a mounted `lfs_t`. It never mounts, formats, grows, or
repairs-by-formatting a volume. Keeping partition selection outside the adapter
makes it reusable and prevents a database open failure from erasing unrelated
files. This also allows an application to preserve an existing filesystem's
disk format, allocation policy, and wear-leveling configuration.

## Recommended transaction profile

Use SQLite rollback journaling with `journal_mode=DELETE`,
`synchronous=EXTRA`, `locking_mode=NORMAL`, and memory-only temporary files.
`TRUNCATE` and `PERSIST` journals are also implemented and tested.
`xDeviceCharacteristics` reports zero; SQLite must not assume atomic writes,
safe append, powersafe overwrite, or sequential storage. The default logical
sector size is 4096 bytes. It must conservatively represent the storage stack's
failure domain; it is not a claim that SQLite file offsets map directly to
physical erase sectors under LittleFS.

SQLite first makes the rollback journal durable, then writes and syncs the
database, then removes the journal. The VFS maps sync to `lfs_file_sync` and
deletion to `lfs_remove`, whose metadata commit is synchronous. Truncation
also syncs before returning, so a truncated journal is durably invalidated. LittleFS and
its block driver must preserve this ordering. The journal and database must
live on the same mounted filesystem. File reads that hit EOF zero-fill the
remaining buffer and return `SQLITE_IOERR_SHORT_READ`, as SQLite requires.

There is one database connection per VFS. Lock methods track SQLite's lock
state within that ownership boundary, not across processes, cores, or other
LittleFS mounts. Filesystem access and all SQLite API calls must be serialized
by the application. No raw LittleFS file or directory handle may remain open
across SQLite calls, including unrelated files; a fault requires discarding the
whole mounted state. Never access database or journal files through another API.
`LFS_THREADSAFE` is unsupported: lock failure during close can prevent unlinking
a file object that SQLite will free. Unsupported paths/file kinds fail explicitly.

After a device I/O/corruption or capacity error, the VFS latches a fault across
all instances on that filesystem. It refuses further I/O until the caller
closes/unregisters, unmounts/remounts LittleFS, and reinitializes the VFS.
Poisoned file handles are marked `LFS_F_ERRED` before `lfs_file_close`, so
LittleFS removes them without flushing. This avoids retrying writes against
a program cache left inconsistent by a failed block operation. It deliberately
does not clear or repair LittleFS cache internals. Raw filesystem users must
also stop accessing that volume until remount.

An empty or missing database is created only through SQLite's normal create
flag. A corrupt filesystem must fail at the application's mount step. A
corrupt database must be reported and preserved; neither layer should silently
replace it with an empty store.

## Memory, space, and lifecycle

The VFS uses fixed-size file caches, with no direct system allocation. SQLite
owns file-object allocation through its configured allocator. LittleFS volume
caches and lookahead buffers belong to the application. The test profile uses
a 64 KiB SQLite heap, 1 KiB database pages, and a 16 KiB default page cache.
The crash workload reduces its connection cache to two pages (2 KiB) to force
cache spills.

Reserve space for the database, its rollback journal, LittleFS copy-on-write
blocks, and metadata. Free bytes alone do not guarantee a transaction can
commit. Return full/storage errors to the application and keep previously
committed data; never discard pending actions to make a transaction succeed.
Large random updates can cause substantial LittleFS write amplification, so
measure latency and flash wear using the actual workload before deployment.

For sleep: stop accepting requests, finish or roll back the transaction, close
the database, unregister the VFS, and unmount the filesystem before powering
down storage. Wake/reboot remounts and reopens normally so SQLite can recover
a hot journal. That sequence is application integration, not a sleep driver
provided by this repository.

## Test model

Host tests use a synthetic NOR image. Program operations can only clear bits,
erase restores a block to `0xff`, and all accesses obey configured bounds and
alignment. Every reboot uses a fresh process; no SQLite state, LittleFS cache,
or open file handle survives. The test image contains unrelated sentinel files
whose contents must remain unchanged.

Fault tests cut execution at block-operation boundaries and during selected
partial program/erase operations. The transaction oracle checks application
rows, indexes, and integrity after reopening. A transaction acknowledged as
committed must survive; an interrupted unacknowledged transaction must recover
entirely before or entirely after, never partly applied. Recovery is itself
subject to interruption. Capacity and I/O failures are separate from abrupt
process death and require testing cleanup and subsequent reopen. The capacity
test fills the volume before a transaction, verifies rejection and preservation,
then reclaims space and retries. It is not an exhaustive matrix of mid-transaction
capacity exhaustion. Device-error tests verify that cleanup performs no further
backend I/O after a fault and that remounting allows recovery.

This model checks the adapter and upstream software under the modeled faults.
It does not reproduce every possible electrical failure, corrupted read,
flash-controller cache, out-of-spec erase, wear-out mode, or brownout. Hardware
qualification must record the chip, driver, SQLite/LittleFS versions, compile
options, geometry, power-cut mechanism, and observed results. Host success must
not be presented as evidence of physical power-loss qualification.

## References

- [SQLite VFS contract](https://sqlite.org/c3ref/vfs.html)
- [SQLite file methods and short-read contract](https://sqlite.org/c3ref/io_methods.html)
- [SQLite atomic commit](https://sqlite.org/atomiccommit.html)
- [SQLite synchronous pragma](https://sqlite.org/pragma.html#pragma_synchronous)
- [LittleFS synchronization and durability](https://github.com/littlefs-project/littlefs/blob/v2.11.3/README.md)
