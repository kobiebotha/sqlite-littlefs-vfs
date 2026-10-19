#!/usr/bin/env python3
"""Fetch or verify pinned upstream sources for examples and qualification tests.

The VFS itself does not vendor or fork either dependency. Consumers may provide
their own compatible builds. This script is stdlib-only and never executes
downloaded code. --check requires an already verified cache (no network).
"""
import argparse
import hashlib
import io
from pathlib import Path
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LFS_VERSION = "v2.11.3"
LFS_FILES = {
    "lfs.c": "a36d6a095785ddea9571d541d68d3e4ef01d5b255a99d17d3f07fb6ea60ea132",
    "lfs.h": "b1befd7288d08815accc8f9af744c55686c0b9e3ac0061c32ceee38a1b3eb96d",
    "lfs_util.c": "f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec",
    "lfs_util.h": "548d46aa524dc7449e16739286c1a422a52f9de727ff0be0c2ffc5593f5ca981",
    "LICENSE.md": "0cb4ff1daf5fdc1359c6a6ee3116092f08fc100c9d58b1b77ab17bfd801f856d",
}
SQLITE_VERSION = "3.54.1"
SQLITE_ARCHIVE = "sqlite-amalgamation-3540100"
SQLITE_SHA3 = "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30"
SQLITE_FILES = {
    "sqlite3.c": "0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52",
    "sqlite3.h": "d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c",
    "sqlite3ext.h": "48963abfefb41ac329ef9d706acb5eadba952a99a748285c9ded6b5e0942721b",
}


def verified(directory, files):
    return all((directory / name).is_file() and
               hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
               for name, digest in files.items())


def download(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def install(directory, files, contents):
    for name, digest in files.items():
        if hashlib.sha256(contents[name]).hexdigest() != digest:
            sys.exit(f"Checksum mismatch: {name}; refusing to install")
    directory.mkdir(parents=True, exist_ok=True)
    for name, content in contents.items():
        temporary = directory / (name + ".tmp")
        temporary.write_bytes(content)
        temporary.replace(directory / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify cache without network access")
    args = parser.parse_args()
    sqlite = ROOT / "vendor/sqlite"
    littlefs = ROOT / "vendor/littlefs"
    missing = [name for name, path, files in [
        ("SQLite", sqlite, SQLITE_FILES), ("LittleFS", littlefs, LFS_FILES)]
        if not verified(path, files)]
    if args.check and missing:
        sys.exit("Missing or modified dependencies: " + ", ".join(missing))
    if "SQLite" in missing:
        archive = download(f"https://sqlite.org/2026/{SQLITE_ARCHIVE}.zip")
        if hashlib.sha3_256(archive).hexdigest() != SQLITE_SHA3:
            sys.exit("SQLite archive SHA3-256 mismatch; refusing to extract")
        with zipfile.ZipFile(io.BytesIO(archive)) as contents:
            install(sqlite, SQLITE_FILES, {
                name: contents.read(f"{SQLITE_ARCHIVE}/{name}") for name in SQLITE_FILES})
    if "LittleFS" in missing:
        install(littlefs, LFS_FILES, {name: download(
            f"https://raw.githubusercontent.com/littlefs-project/littlefs/{LFS_VERSION}/{name}")
            for name in LFS_FILES})
    print(f"Verified SQLite {SQLITE_VERSION}; LittleFS {LFS_VERSION}")


if __name__ == "__main__":
    main()
