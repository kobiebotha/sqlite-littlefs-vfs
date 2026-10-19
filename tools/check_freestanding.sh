#!/bin/sh
# Compile the adapter, its example, and both dependencies for Cortex-M33.
# This is a compile check, not a firmware image or hardware qualification.
set -eu
cd "$(dirname "$0")/.."
python3 tools/fetch_deps.py --check
arm_cc=${ARM_CC:-arm-none-eabi-gcc}
build_dir=${ARM_BUILD_DIR:-build-arm}
mkdir -p "$build_dir"
for source in src/sqlite_littlefs_vfs.c examples/mounted.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c vendor/sqlite/sqlite3.c tests/sqlite_port.c; do
    object=$(basename "$source" .c)
    "$arm_cc" -std=c11 -Os -ffreestanding -ffunction-sections -fdata-sections \
        -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard \
        -DLFS_NO_MALLOC -DLFS_MULTIVERSION -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
        -include tests/sqlite_config.h -Iinclude -Ivendor/sqlite -Ivendor/littlefs \
        -c "$source" -o "$build_dir/$object.o"
done
echo "PASS: Cortex-M33 freestanding compile (adapter, example, SQLite, LittleFS)"
