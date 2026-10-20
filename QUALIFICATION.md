# Initial host qualification

Recorded 2026-10-19 for the initial implementation. These are software fault
simulation results, not physical power-loss qualification or a production
reliability guarantee.

## Configuration

- macOS 27.0.1, arm64; Apple Clang 21.0.0.
- SQLite 3.54.1 and LittleFS 2.11.3, verified by `tools/fetch_deps.py --check`.
- LittleFS disk format 2.1; 512 KiB synthetic NOR, 4 KiB erase blocks,
  256-byte programming, byte reads, 256-byte caches, 32-byte lookahead.
- 64 KiB SQLite `memsys5` heap with no system allocation fallback; 1 KiB pages,
  two-page workload cache, memory temporary storage, `synchronous=EXTRA`.
- Upstream LittleFS assertions enabled, including in the optimized build.
- Embedded SQLite options: [tests/sqlite_config.h](tests/sqlite_config.h).
  A second build exercises API behavior with the less-trimmed desktop SQLite
  profile in [CMakeLists.txt](CMakeLists.txt), including WAL and extension probes.

## Results

The optimized CTest run passed all three tests: embedded API checks, the complete
crash/recovery matrix, and desktop-profile API checks. Total runtime was 96 seconds
on this host; timings are not a benchmark.

The Debug build with AddressSanitizer and UndefinedBehaviorSanitizer also passed
all three tests and the same complete fault matrix, with no sanitizer findings
(527 seconds). The targeted partial-write regression was then rerun with
sanitizers after final test-helper cleanup and passed.

| Scenario | Initial/action cuts | Cuts during recovery | Returned I/O errors |
| --- | ---: | ---: | ---: |
| First database/schema creation | 976 | — | — |
| DELETE journal | 468 | 8,984 | 348 |
| TRUNCATE journal | 460 | 8,984 | 342 |
| PERSIST journal | 624 | 23,944 | 465 |
| **Total** | **2,528** | **41,912** | **1,155** |

That is **44,440 simulated power cuts** plus **1,155 injected device errors**.
Cuts cover before/after operations, prefix writes/erases, sparse partial changes,
and a final boundary after commit acknowledgement. Repeated recovery testing
uses distinct damaged-image/acknowledgement combinations. Every reboot starts
with fresh SQLite and LittleFS state in a new process.

The oracle requires complete old or complete new transactions, preserves every
acknowledged transaction and its pending outbox row, checks row/index consistency
and `PRAGMA integrity_check`, and verifies unrelated sentinel files. Device-error
tests also check that no backend callback occurs after the VFS fault latch, then
remount and recover. API tests exercise rollback/savepoints, JSON, short reads,
invalid paths, connection restrictions, read-only recovery, allocator exhaustion,
read errors, a full filesystem, space reclamation, and existing-WAL refusal.

The library-only CMake consumer build passes. Adapter, example, SQLite, and
LittleFS also compile as freestanding Cortex-M33 objects with Arm GNU 13.2.Rel1.
That check does not link or execute firmware.

## Initial GitHub Actions timing

At commit `c8ad5f5a290f141f4b3fb2a91dfec400e878f3d0`,
[run 39342503436](https://github.com/kobiebotha/sqlite-littlefs-vfs/actions/runs/39342503436)
passed Linux with and without sanitizers and macOS without sanitizers. The
macOS sanitizer build succeeded, then its CTest step failed with exit code 8
after 906 seconds (23:35:57–23:51:03 UTC on 2026-10-19). The passing CTest steps
took 79 seconds on Linux without sanitizers, 398 seconds on Linux with
sanitizers, and 131 seconds on macOS without sanitizers.

The 906-second duration closely matches the original 900-second crash-test
limit plus API-test overhead, so timeout is the likely cause. This remains an
inference: the unauthenticated job-log API returned HTTP 403, and the public
check annotations did not include the detailed CTest result. It is not evidence
of a sanitizer finding or a passing macOS sanitizer CI run. The same Debug
sanitizer matrix passed locally in 527 seconds, as recorded above.

CI now allows 3,600 seconds for the full sanitizer crash matrix, retains the
900-second normal limit, and uploads CTest failure logs. No cases are skipped.
The timeout settings were checked by configuring both profiles and inspecting
CTest's generated test properties; the remote matrix must run again to confirm
the outcome.

## Regression captured

A partial device write followed by an I/O error originally let SQLite retry
LittleFS cleanup against an inconsistent program cache. LittleFS's debug
assertion caught this even though a prior optimized build had continued. The
VFS now latches filesystem faults, refuses more I/O, discards failed handles
without flushing, and requires the owner to remount before retrying. Assertions
remain enabled in all qualification builds. The original targeted case passes:

```sh
./build/host_test --fault 44 6 DELETE
```

Here `6` selects a returned error after a partial device operation. Full tests
are the commands documented in [README.md](README.md).

## Limits and next qualification

The NOR model makes completed operations immediately durable. It does not model
an asynchronous flash controller cache, arbitrary electrical damage, wear,
brownouts, or out-of-spec power behavior. The capacity test fills storage before
a transaction; it is not an exhaustive mid-transaction out-of-space matrix.

GitHub Actions is configured for macOS/Linux with and without sanitizers; remote
CI results are separate from these local results. Board integration still needs
its own driver, partition preservation, sleep/reboot, and controlled physical
power-cut qualification. Preserve existing files and never format on recovery
failure. See [DESIGN.md](DESIGN.md) for the required storage contract.
