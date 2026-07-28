#!/usr/bin/env python3
"""Recover per-book reading stats left behind by CrossInk when moving to CrossEyed.

Why this is needed
------------------
Both firmwares keep a book's cache (including its reading stats) in a directory
named after a hash of the book's path:

    /.crosspoint/epub_<hash>/stats_v5.bin

CrossInk changed the hash. CrossEyed is based on CrossPoint 1.5.0 and still uses
the original:

    CrossPoint / CrossEyed  epub_<std::hash<std::string>>   (MurmurHash2, 32-bit)
    CrossInk                epub_<FNV-1a 64-bit>

CrossInk also *renames* the old directory to the new name on first open, so
after running CrossInk the card only has the FNV-named directories. CrossEyed
does not recognise those, silently re-indexes each book into a fresh
std::hash-named directory, and the old stats sit there orphaned.

Nothing was deleted. This script copies each book's stats file from the CrossInk
directory into the matching CrossEyed one.

Global stats (/.crosspoint/global_stats.bin) are NOT affected -- that path and
its file format are identical in both firmwares, so total reading time, pages
turned and streaks carried over untouched.

Usage
-----
    # See what would happen (default -- makes no changes):
    python3 recover_crossink_stats.py /media/you/SDCARD

    # Actually copy:
    python3 recover_crossink_stats.py /media/you/SDCARD --apply

Existing CrossEyed stats files are never overwritten unless --force is given;
a timestamped .bak copy is made first either way.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from datetime import datetime
from pathlib import Path

CACHE_DIR_NAME = ".crosspoint"
# BookReadingStats::STATS_FILE_VERSION is 5, with documented fallbacks to the
# previous versioned name and the original unversioned one. Try newest first;
# the firmware migrates whichever it finds.
STATS_FILE_NAMES = ("stats_v5.bin", "stats_v4.bin", "stats.bin")
EPUB_SUFFIX = ".epub"


def std_hash_string(s: str) -> int:
    """libstdc++ std::hash<std::string> on a 32-bit target (ESP32-C3).

    Mirrors _Hash_bytes() in libsupc++/hash_bytes.cc: MurmurHash2, 32-bit
    variant, seeded with 0xc70f6907. Used by CrossPoint/CrossEyed in
    lib/Epub/Epub.h to build the cache key.
    """
    data = s.encode("utf-8")
    m = 0x5BD1E995
    mask = 0xFFFFFFFF
    length = len(data)
    h = (0xC70F6907 ^ length) & mask

    i = 0
    while length - i >= 4:
        k = int.from_bytes(data[i : i + 4], "little")
        k = (k * m) & mask
        k ^= k >> 24
        k = (k * m) & mask
        h = (h * m) & mask
        h ^= k
        i += 4

    tail = length - i
    if tail == 3:
        h ^= data[i + 2] << 16
    if tail >= 2:
        h ^= data[i + 1] << 8
    if tail >= 1:
        h ^= data[i]
        h = (h * m) & mask

    h ^= h >> 13
    h = (h * m) & mask
    h ^= h >> 15
    return h & mask


def fnv_hash_64(s: str) -> int:
    """FNV-1a 64-bit, matching ZipFile::fnvHash64 in CrossInk."""
    h = 14695981039346656037
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def crosseyed_dir_name(book_path: str) -> str:
    # std::to_string(size_t) -- unsigned decimal, no sign.
    return f"epub_{std_hash_string(book_path)}"


def crossink_dir_name(book_path: str) -> str:
    return f"epub_{fnv_hash_64(book_path)}"


def device_path(book: Path, sd_root: Path) -> str:
    """The path string the firmware hashes: SD-root-relative, leading slash."""
    return "/" + book.relative_to(sd_root).as_posix()


def find_stats_file(cache_dir: Path) -> Path | None:
    for name in STATS_FILE_NAMES:
        candidate = cache_dir / name
        if candidate.is_file():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Copy orphaned CrossInk per-book stats into their CrossEyed cache directories."
    )
    parser.add_argument("sd_root", type=Path, help="Mount point of the SD card (the folder containing .crosspoint)")
    parser.add_argument("--apply", action="store_true", help="Actually copy files (default is a dry run)")
    parser.add_argument(
        "--force", action="store_true", help="Overwrite a CrossEyed stats file that already exists (backed up first)"
    )
    args = parser.parse_args()

    sd_root: Path = args.sd_root.resolve()
    cache_root = sd_root / CACHE_DIR_NAME
    if not cache_root.is_dir():
        print(f"error: no {CACHE_DIR_NAME}/ directory under {sd_root}", file=sys.stderr)
        print("       Point this at the SD card root, not at a subfolder.", file=sys.stderr)
        return 1

    books = sorted(p for p in sd_root.rglob(f"*{EPUB_SUFFIX}") if CACHE_DIR_NAME not in p.parts)
    if not books:
        print(f"No {EPUB_SUFFIX} files found under {sd_root}.")
        return 1

    print(f"SD card:  {sd_root}")
    print(f"Books:    {len(books)}")
    print(f"Mode:     {'APPLY' if args.apply else 'dry run (use --apply to copy)'}")
    print()

    # Self-check before writing anything.
    #
    # std_hash_string() is a transcription of libstdc++'s 32-bit _Hash_bytes,
    # and there is no way to confirm it off-device -- so confirm it against the
    # card instead. If CrossEyed has opened even one book, a directory named
    # with this hash must already exist. No matches means either the hash is
    # wrong (do not write) or CrossEyed never ran (nothing to recover into).
    existing = {p.name for p in cache_root.iterdir() if p.is_dir() and p.name.startswith("epub_")}
    verified = [b for b in books if crosseyed_dir_name(device_path(b, sd_root)) in existing]
    if not verified:
        print("error: none of the computed CrossEyed cache directory names exist on this card.", file=sys.stderr)
        if existing:
            print(
                f"       The card has {len(existing)} epub_* directories, so they were named by a\n"
                "       different hash than this script computes. Refusing to write.",
                file=sys.stderr,
            )
        else:
            print("       The card has no epub_* cache directories at all.", file=sys.stderr)
        print("       Boot CrossEyed and open one book, then run this again.", file=sys.stderr)
        return 1
    print(f"Hash check: OK -- {len(verified)}/{len(books)} books match an existing CrossEyed cache directory.")
    print()

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    recovered = skipped = missing = 0

    for book in books:
        rel = device_path(book, sd_root)
        src_dir = cache_root / crossink_dir_name(rel)
        dst_dir = cache_root / crosseyed_dir_name(rel)

        src = find_stats_file(src_dir) if src_dir.is_dir() else None
        if src is None:
            missing += 1
            continue

        if not dst_dir.is_dir():
            # CrossEyed has not opened this book yet. Creating the directory
            # alone is fine: the firmware rebuilds the rest of the cache on
            # first open and reads the stats file it finds there.
            if args.apply:
                dst_dir.mkdir(parents=True, exist_ok=True)

        dst = dst_dir / src.name
        if dst.exists() and not args.force:
            print(f"  skip   {rel}\n         {dst.name} already exists in {dst_dir.name} (use --force to replace)")
            skipped += 1
            continue

        print(f"  copy   {rel}\n         {src_dir.name}/{src.name}  ->  {dst_dir.name}/{src.name}")
        if args.apply:
            if dst.exists():
                backup = dst.with_suffix(dst.suffix + f".{stamp}.bak")
                shutil.copy2(dst, backup)
                print(f"         backed up existing file to {backup.name}")
            shutil.copy2(src, dst)
        recovered += 1

    print()
    print(f"Recoverable: {recovered}   already present: {skipped}   no CrossInk stats: {missing}")
    if recovered and not args.apply:
        print("\nNothing was written. Re-run with --apply to copy.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
