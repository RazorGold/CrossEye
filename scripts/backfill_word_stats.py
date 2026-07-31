#!/usr/bin/env python3
"""Backfill Words/Min word counts for books that were read before the stat existed.

Why this is needed
------------------
CrossEye replaced Pages/Min with Words/Min. The new figure is computed from an
8-bin histogram of implied reading rate that the firmware fills in as you read,
so a card full of books read under the old firmware has an empty histogram and
every book shows "-" until it is read again.

Nothing on the card is missing, though: each book still has its total reading
time and its reading position, and the EPUB itself still has its words. This
script estimates how many words each book was read for, and seeds the histogram
with that estimate so the history is not a reset.

    words read  x 60
    ----------------  =  the seeded Words/Min figure
    reading seconds

Estimated values are not the same statistic as measured ones. A measured figure
comes from the display-time trim, which approximates *uninterrupted* pace. An
estimate is derived from totalReadingSeconds, which includes idle time up to the
device's idle threshold, so estimates read 20-30% lower. Every book this script
touches is therefore marked with the wordsBackfilled flag, which the device shows
as a leading "~" and a sync server must use to segregate estimates before ranking.

**Per-book only.** The global histogram is never seeded. It has no flags field to
mark an estimate with, the figure is time-weighted so a seed carrying a whole
history's worth of seconds would dominate it for months, and it is the number
most likely to end up on a leaderboard. Global fills in from real reading, which
takes about one session.

Usage
-----
    # See what would happen (default -- makes no changes):
    python3 backfill_word_stats.py E:\\

    # Inspect the histograms already on the card, change nothing:
    python3 backfill_word_stats.py E:\\ --dump

    # Actually write:
    python3 backfill_word_stats.py E:\\ --apply

    # Verify this script's own arithmetic (no card needed):
    python3 backfill_word_stats.py --selftest

Requires the v6 stats format, i.e. flash the firmware first. Running against v5
files is refused rather than guessed at: a v6 file written here beside a v5 file
the old firmware is still updating would leave two live copies of a book's
history.

Every file is backed up to a timestamped .bak before it is written. A book whose
histogram already holds data is skipped; --force redoes a book that was
previously backfilled and has not been read since, and never overwrites measured
samples.

Calibrating first
-----------------
The shared token rule is the easy half. The risk is the *input*: the firmware
counts tokens on laid-out pages -- after parsing, with images excluded, soft
hyphens stripped and hyphenation applied -- while this script strips tags from
raw XHTML, where titles, alt text, footnote documents and hidden content all
shift the number. Calibrate before trusting a write:

    # On the device, at LOG_LEVEL=2, read one chapter and collect the
    # "Page words: N" lines; sum them.
    python3 backfill_word_stats.py --spine-words /path/to/book.epub

Compare that chapter's spine total against the device's sum. Expect ~60-90 words
per page on an X3, which is the sanity anchor for the whole plan. Do this for two
or three books: the script writes once, and the 100-400 wpm band is the only
other check standing between a bad count and a bad number on screen.

stdlib only, so it runs on any python3 without installing anything -- the card is
only visible from the Windows host.
"""

from __future__ import annotations

import argparse
import html
import re
import shutil
import sys
import unittest
import zipfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from xml.etree import ElementTree

# --- Constants mirrored from the firmware -----------------------------------
# Keep in step with docs/file-formats.md. Each one names its source.

CACHE_DIR_NAME = ".crosspoint"
EPUB_SUFFIX = ".epub"

# BookReadingStats.h
STATS_FILE_NAME = "stats_v6.bin"
STATS_FILE_VERSION = 6
STATS_FILE_SIZE = 165
STATS_FILE_NAME_V5 = "stats_v5.bin"
BOOK_FLAGS_OFFSET = 16
FLAG_WORDS_BACKFILLED = 1 << 2
BOOK_BIN_COUNT_OFFSET = 73
BOOK_BIN_WORDS_OFFSET = 89
BOOK_BIN_SECONDS_OFFSET = 121
BOOK_STATS_REVISION_OFFSET = 153

# GlobalReadingStats.h
GLOBAL_STATS_NAME = "global_stats.bin"
GLOBAL_STATS_VERSION = 4
GLOBAL_STATS_SIZE = 270
GLOBAL_BIN_COUNT_OFFSET = 159
GLOBAL_BIN_WORDS_OFFSET = 191
GLOBAL_BIN_SECONDS_OFFSET = 223
GLOBAL_STATS_REVISION_OFFSET = 255
GLOBAL_TOKEN_RULE_OFFSET = 259

# ReadingStatsUtils.h
WPM_BIN_COUNT = 8
WPM_BIN_UPPER = (50, 100, 150, 200, 300, 400, 550, 0xFFFF)
TOKEN_RULE_VERSION = 1

# WpmHistogram.h
WPM_TRIM_PERCENT = 10
MIN_WPM_SAMPLES = 25

# WordTokenRule.h
CJK_CHARS_PER_WORD = 2

# EpubReaderUtils.h: progress.bin is 6 bytes, three uint16 LE.
PROGRESS_FILE_NAME = "progress.bin"
PROGRESS_FILE_SIZE = 6

# Books whose implied pace lands outside this band are reported and skipped, never
# guessed at. Out-of-band means an assumption broke: a re-read (pages counted
# twice against one recorded position), an abandoned book marked finished, or
# stats belonging to a different edition. Expect a downward skew on pre-migration
# data, whose reading time carries up to 300 s of idle per interruption.
SANITY_WPM_MIN = 100
SANITY_WPM_MAX = 400


# --- Cache directory naming --------------------------------------------------


def std_hash_string(s: str) -> int:
    """libstdc++ std::hash<std::string> on a 32-bit target (ESP32-C3).

    Mirrors _Hash_bytes() in libsupc++/hash_bytes.cc: MurmurHash2, 32-bit
    variant, seeded with 0xc70f6907. Used by CrossEye in lib/Epub/Epub.h to build
    the cache key.

    Duplicated from recover_crossink_stats.py rather than imported: each script
    has to be independently copy-pasteable to the Windows host, which is the only
    machine that can see the card.
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


def cache_dir_name(device_path: str) -> str:
    return f"epub_{std_hash_string(device_path)}"


def device_path_for(book: Path, sd_root: Path) -> str:
    """The path string the firmware hashes: SD-root-relative, leading slash."""
    return "/" + book.relative_to(sd_root).as_posix()


# --- The shared word token rule ---------------------------------------------
# Mirrors src/activities/reader/WordTokenRule.cpp. Versioned as
# TOKEN_RULE_VERSION so a sync server can refuse to rank histograms built under
# different rules rather than doing it silently.

_GENERAL_PUNCTUATION = ((0x2000, 0x206F),)

_CJK_RANGES = (
    (0x1100, 0x11FF),  # Hangul Jamo
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x3130, 0x318F),  # Hangul Compatibility Jamo
    (0x3400, 0x4DBF),  # CJK Extension A
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs
    (0xAC00, 0xD7AF),  # Hangul Syllables
    (0xD7B0, 0xD7FF),  # Hangul Jamo Extended-B
    (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
    (0xFE30, 0xFE4F),  # CJK Compatibility Forms
    (0xFF01, 0xFF60),  # Fullwidth Latin / Punctuation
    (0xFF65, 0xFFEF),  # Halfwidth Katakana / Hangul
    (0x20000, 0x2A6DF),  # CJK Extension B
    (0x2A700, 0x2B73F),  # CJK Extension C
)

_CJK_PUNCTUATION_RANGES = (
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0xFE30, 0xFE4F),  # CJK Compatibility Forms
    (0xFF01, 0xFF0F),  # fullwidth ! through /
    (0xFF1A, 0xFF20),  # fullwidth : through @
    (0xFF3B, 0xFF40),  # fullwidth [ through `
    (0xFF5B, 0xFF60),  # fullwidth { through
)


def _in_ranges(cp: int, ranges: tuple) -> bool:
    return any(lo <= cp <= hi for lo, hi in ranges)


def is_cjk_word_character(ch: str) -> bool:
    cp = ord(ch)
    return _in_ranges(cp, _CJK_RANGES) and not _in_ranges(cp, _CJK_PUNCTUATION_RANGES)


def is_word_token(token: str) -> bool:
    """True when a token carries word content, matching isWordToken() in C++."""
    for ch in token:
        cp = ord(ch)
        if cp < 0x80:
            if ch.isalnum():
                return True
            continue
        if cp == 0xFFFD:
            continue
        if _in_ranges(cp, _GENERAL_PUNCTUATION) or _in_ranges(cp, _CJK_PUNCTUATION_RANGES):
            continue
        return True
    return False


def count_words(text: str) -> int:
    """Words in a run of plain text, under the shared token rule.

    One deliberate difference from the firmware: the hyphenation-prefix rule is
    not applied. On the device a trailing '-' marks a word the *layout* split
    across a line break, which raw source text has never been through. Applying it
    here would drop genuine words that happen to end in a hyphen.
    """
    latin_words = 0
    cjk_characters = 0
    for token in text.split():
        if not is_word_token(token):
            continue
        cjk_in_token = sum(1 for ch in token if is_cjk_word_character(ch))
        if cjk_in_token:
            cjk_characters += cjk_in_token
        else:
            latin_words += 1
    return latin_words + (cjk_characters + CJK_CHARS_PER_WORD // 2) // CJK_CHARS_PER_WORD


# --- EPUB text extraction ----------------------------------------------------

_HEAD_RE = re.compile(rb"<head\b.*?</head\s*>", re.IGNORECASE | re.DOTALL)
_DROP_ELEMENT_RE = re.compile(rb"<(script|style)\b.*?</\1\s*>", re.IGNORECASE | re.DOTALL)
_COMMENT_RE = re.compile(rb"<!--.*?-->", re.DOTALL)
_TAG_RE = re.compile(rb"<[^>]*>")


def visible_text(document: bytes) -> str:
    """Strip an XHTML document down to the text a reader would see.

    <head> goes first, which is what keeps every chapter's <title> out of the
    count: the firmware counts words on laid-out pages, where the title was never
    rendered. This is the single biggest source of divergence between the two
    sides, which is why the calibration step below is not optional.
    """
    stripped = _COMMENT_RE.sub(b" ", document)
    stripped = _HEAD_RE.sub(b" ", stripped)
    stripped = _DROP_ELEMENT_RE.sub(b" ", stripped)
    stripped = _TAG_RE.sub(b" ", stripped)
    return html.unescape(stripped.decode("utf-8", errors="replace"))


_XHTML_MEDIA_TYPES = ("application/xhtml+xml", "text/html", "application/x-dtbook+xml")


def _resolve(base: str, href: str) -> str:
    """Resolve an OPF-relative href to a zip entry name."""
    href = href.split("#", 1)[0]
    parts = [p for p in base.split("/")[:-1] if p]
    for segment in href.split("/"):
        if segment in ("", "."):
            continue
        if segment == "..":
            if parts:
                parts.pop()
            continue
        parts.append(segment)
    return "/".join(parts)


def _localname(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def spine_word_counts(epub_path: Path) -> list[int]:
    """Words per spine document, in reading order.

    Navigation documents are skipped: EPUB 3 puts the table of contents in the
    spine, and its entries are chapter titles rather than prose.
    """
    with zipfile.ZipFile(epub_path) as zf:
        container = ElementTree.fromstring(zf.read("META-INF/container.xml"))
        rootfile = None
        for element in container.iter():
            if _localname(element.tag) == "rootfile":
                rootfile = element.get("full-path")
                break
        if not rootfile:
            raise ValueError("container.xml names no rootfile")

        opf = ElementTree.fromstring(zf.read(rootfile))
        manifest: dict[str, tuple[str, str, str]] = {}
        spine_ids: list[str] = []
        for element in opf.iter():
            name = _localname(element.tag)
            if name == "item":
                item_id = element.get("id")
                if item_id:
                    manifest[item_id] = (
                        element.get("href", ""),
                        element.get("media-type", ""),
                        element.get("properties", ""),
                    )
            elif name == "itemref":
                idref = element.get("idref")
                if idref:
                    spine_ids.append(idref)

        counts: list[int] = []
        names = set(zf.namelist())
        for item_id in spine_ids:
            href, media_type, properties = manifest.get(item_id, ("", "", ""))
            if not href or (media_type and media_type not in _XHTML_MEDIA_TYPES):
                counts.append(0)
                continue
            if "nav" in properties.split():
                counts.append(0)
                continue
            entry = _resolve(rootfile, href)
            if entry not in names:
                counts.append(0)
                continue
            counts.append(count_words(visible_text(zf.read(entry))))
        return counts


# --- Binary helpers ----------------------------------------------------------


def read_le16(buf: bytes, offset: int) -> int:
    return int.from_bytes(buf[offset : offset + 2], "little")


def read_le32(buf: bytes, offset: int) -> int:
    return int.from_bytes(buf[offset : offset + 4], "little")


def write_le16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = min(value, 0xFFFF).to_bytes(2, "little")


def write_le32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = min(value, 0xFFFFFFFF).to_bytes(4, "little")


def wpm_bin_index(wpm: int) -> int:
    for i in range(WPM_BIN_COUNT - 1):
        if wpm < WPM_BIN_UPPER[i]:
            return i
    return WPM_BIN_COUNT - 1


@dataclass
class Histogram:
    count: list[int] = field(default_factory=lambda: [0] * WPM_BIN_COUNT)
    words: list[int] = field(default_factory=lambda: [0] * WPM_BIN_COUNT)
    seconds: list[int] = field(default_factory=lambda: [0] * WPM_BIN_COUNT)

    def empty(self) -> bool:
        return not any(self.count)

    def total_count(self) -> int:
        return sum(self.count)

    def untrimmed_wpm(self) -> int | None:
        total_seconds = sum(self.seconds)
        if not total_seconds:
            return None
        return sum(self.words) * 60 // total_seconds

    def trimmed_wpm(self) -> int | None:
        """Mirrors trimmedWordsPerMinute() in WpmHistogram.cpp.

        Integer arithmetic throughout, including the pro-rata slice, so this
        reports exactly what the device shows.
        """
        total = self.total_count()
        words = sum(self.words)
        seconds = sum(self.seconds)
        if not total or not seconds:
            return None
        drop = total * WPM_TRIM_PERCENT // 100 if total >= MIN_WPM_SAMPLES else 0
        for i in range(WPM_BIN_COUNT):
            if drop <= 0:
                break
            if not self.count[i]:
                continue
            if self.count[i] <= drop:
                words -= self.words[i]
                seconds -= self.seconds[i]
                drop -= self.count[i]
                continue
            words -= self.words[i] * drop // self.count[i]
            seconds -= self.seconds[i] * drop // self.count[i]
            drop = 0
        if seconds <= 0:
            return None
        return words * 60 // seconds


def read_book_histogram(buf: bytes) -> Histogram:
    hist = Histogram()
    for i in range(WPM_BIN_COUNT):
        hist.count[i] = read_le16(buf, BOOK_BIN_COUNT_OFFSET + i * 2)
        hist.words[i] = read_le32(buf, BOOK_BIN_WORDS_OFFSET + i * 4)
        hist.seconds[i] = read_le32(buf, BOOK_BIN_SECONDS_OFFSET + i * 4)
    return hist


def read_global_histogram(buf: bytes) -> Histogram:
    hist = Histogram()
    for i in range(WPM_BIN_COUNT):
        hist.count[i] = read_le32(buf, GLOBAL_BIN_COUNT_OFFSET + i * 4)
        hist.words[i] = read_le32(buf, GLOBAL_BIN_WORDS_OFFSET + i * 4)
        hist.seconds[i] = read_le32(buf, GLOBAL_BIN_SECONDS_OFFSET + i * 4)
    return hist


# --- Stats files -------------------------------------------------------------


@dataclass
class BookStats:
    total_reading_seconds: int
    total_pages_turned: int
    is_completed: bool
    finished_date_valid: bool
    words_backfilled: bool
    stats_revision: int
    histogram: Histogram


def parse_book_stats(buf: bytes) -> BookStats:
    if len(buf) != STATS_FILE_SIZE or buf[0] != STATS_FILE_VERSION:
        raise ValueError(f"not a v{STATS_FILE_VERSION} stats file ({len(buf)} bytes, version {buf[0] if buf else '?'})")
    flags = buf[BOOK_FLAGS_OFFSET]
    return BookStats(
        total_reading_seconds=read_le32(buf, 3),
        total_pages_turned=read_le32(buf, 7),
        is_completed=buf[11] != 0,
        finished_date_valid=read_le16(buf, 21) != 0,
        words_backfilled=bool(flags & FLAG_WORDS_BACKFILLED),
        stats_revision=read_le32(buf, BOOK_STATS_REVISION_OFFSET),
        histogram=read_book_histogram(buf),
    )


def seed_book_stats(buf: bytes, words_read: int, seconds: int, wpm: int) -> bytes:
    """Write one seeded bin, the backfill flag and a bumped revision.

    The count is MIN_WPM_SAMPLES rather than the book's page count, for two
    reasons. The displayed value is right either way, because trimming a pro-rata
    slice of a single bin leaves its words:seconds ratio alone. But seeding a
    whole reading history's worth of votes into one bin would spend the 10% trim
    budget inside the synthetic bin for years, so real slow pages would never be
    trimmed. MIN_WPM_SAMPLES is also exactly the threshold the per-book card needs
    to show a figure at all, and the device uses it to tell a pure seed from a
    seed that has since been read against.
    """
    out = bytearray(buf)
    out[BOOK_FLAGS_OFFSET] |= FLAG_WORDS_BACKFILLED
    for i in range(WPM_BIN_COUNT):
        write_le16(out, BOOK_BIN_COUNT_OFFSET + i * 2, 0)
        write_le32(out, BOOK_BIN_WORDS_OFFSET + i * 4, 0)
        write_le32(out, BOOK_BIN_SECONDS_OFFSET + i * 4, 0)
    index = wpm_bin_index(wpm)
    write_le16(out, BOOK_BIN_COUNT_OFFSET + index * 2, MIN_WPM_SAMPLES)
    write_le32(out, BOOK_BIN_WORDS_OFFSET + index * 4, words_read)
    write_le32(out, BOOK_BIN_SECONDS_OFFSET + index * 4, seconds)
    write_le32(out, BOOK_STATS_REVISION_OFFSET, read_le32(buf, BOOK_STATS_REVISION_OFFSET) + 1)
    return bytes(out)


def read_progress(path: Path) -> tuple[int, int, int] | None:
    """spineIndex, pageNumber, pageCount -- three uint16 LE."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) < PROGRESS_FILE_SIZE:
        return None
    return read_le16(data, 0), read_le16(data, 2), read_le16(data, 4)


# --- The estimate ------------------------------------------------------------


def estimate_words_read(
    spine_words: list[int],
    completed: bool,
    progress: tuple[int, int, int] | None,
) -> int:
    """Words this book was read for.

    A finished book is its whole word count. An unfinished one is every spine
    document before the current position plus a page-proportional slice of the
    current one -- using per-spine *word* counts rather than the firmware's
    byte-size progress proxy, so uneven word density across front matter, notes
    and prose does not skew it.
    """
    total = sum(spine_words)
    if completed or not progress:
        return total

    spine_index, page_number, page_count = progress
    if spine_index >= len(spine_words):
        return total

    read = sum(spine_words[:spine_index])
    if page_count > 0:
        fraction = min(page_number, page_count)
        read += spine_words[spine_index] * fraction // page_count
    return read


# --- Reporting ---------------------------------------------------------------


@dataclass
class BookResult:
    title: str
    status: str
    words_read: int = 0
    seconds: int = 0
    wpm: int = 0
    pages: int = 0
    detail: str = ""


def _wpm(value: int | None) -> str:
    """A reading of None means not measured, which the device shows as "-"."""
    return "-" if value is None else str(value)


def format_histogram(hist: Histogram) -> str:
    parts = []
    for i in range(WPM_BIN_COUNT):
        if not hist.count[i]:
            continue
        low = 0 if i == 0 else WPM_BIN_UPPER[i - 1]
        high = "inf" if i == WPM_BIN_COUNT - 1 else str(WPM_BIN_UPPER[i])
        parts.append(f"[{low}-{high}) n={hist.count[i]} w={hist.words[i]} s={hist.seconds[i]}")
    return "; ".join(parts) if parts else "empty"


def backup_then_write(path: Path, data: bytes, apply: bool) -> str:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_suffix(path.suffix + f".{stamp}.bak")
    if not apply:
        return f"would write {path.name} ({len(data)} bytes), backup {backup.name}"
    if path.exists():
        shutil.copy2(path, backup)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)
    return f"wrote {path.name}, backup {backup.name}"


# --- Main --------------------------------------------------------------------


def process_book(book: Path, sd_root: Path, cache_root: Path, args) -> BookResult:
    title = book.name
    cache_dir = cache_root / cache_dir_name(device_path_for(book, sd_root))
    if not cache_dir.is_dir():
        return BookResult(title, "skip", detail="no cache directory (never opened on this firmware)")

    stats_path = cache_dir / STATS_FILE_NAME
    if not stats_path.is_file():
        if (cache_dir / STATS_FILE_NAME_V5).is_file():
            return BookResult(title, "skip", detail="still v5: flash the firmware and open the book once first")
        return BookResult(title, "skip", detail="no stats file")

    try:
        raw = stats_path.read_bytes()
        stats = parse_book_stats(raw)
    except (OSError, ValueError) as exc:
        return BookResult(title, "skip", detail=str(exc))

    if args.dump:
        hist = stats.histogram
        return BookResult(
            title,
            "dump",
            seconds=stats.total_reading_seconds,
            pages=stats.total_pages_turned,
            detail=(
                f"{format_histogram(hist)} | untrimmed={_wpm(hist.untrimmed_wpm())} "
                f"trimmed={_wpm(hist.trimmed_wpm())} backfilled={stats.words_backfilled}"
            ),
        )

    if not stats.histogram.empty():
        pure_seed = stats.words_backfilled and stats.histogram.total_count() <= MIN_WPM_SAMPLES
        if not (args.force and pure_seed):
            reason = "already has measured samples" if not pure_seed else "already backfilled (use --force to redo)"
            return BookResult(title, "skip", detail=reason)

    if stats.total_reading_seconds < 60:
        return BookResult(title, "skip", detail="under a minute of reading time")

    try:
        spine_words = spine_word_counts(book)
    except (OSError, ValueError, zipfile.BadZipFile, ElementTree.ParseError) as exc:
        return BookResult(title, "skip", detail=f"could not read EPUB: {exc}")

    total_words = sum(spine_words)
    if total_words == 0:
        return BookResult(title, "skip", detail="no text found in the EPUB")

    progress = read_progress(cache_dir / PROGRESS_FILE_NAME)
    completed = stats.is_completed or stats.finished_date_valid
    words_read = estimate_words_read(spine_words, completed, progress)
    if words_read == 0:
        return BookResult(title, "skip", detail="estimated zero words read")

    wpm = words_read * 60 // stats.total_reading_seconds
    result = BookResult(
        title,
        "ok",
        words_read=words_read,
        seconds=stats.total_reading_seconds,
        wpm=wpm,
        pages=stats.total_pages_turned,
    )
    if not SANITY_WPM_MIN <= wpm <= SANITY_WPM_MAX:
        result.status = "skip"
        result.detail = (
            f"implied {wpm} wpm is outside {SANITY_WPM_MIN}-{SANITY_WPM_MAX}; "
            f"{words_read} words over {stats.total_reading_seconds}s looks like a re-read, "
            "a wrong edition or a mis-marked finish"
        )
        return result

    seeded = seed_book_stats(raw, words_read, stats.total_reading_seconds, wpm)
    result.detail = backup_then_write(stats_path, seeded, args.apply)
    return result


def process_global(cache_root: Path, args) -> str:
    """Reports the global histogram. Never writes to it.

    Global is measurement-only by design. Seeding it would put an estimate into
    the one figure that is meant to be comparable between people, and because the
    reported rate is time-weighted, a seed carrying a whole reading history's
    worth of seconds would dominate that figure for months of real reading.

    Worse, it could not be labelled: the per-book file has a wordsBackfilled flag,
    the global file has no flags field at all, so a seeded global bin merges with
    later measured samples and becomes permanently indistinguishable from them.
    Per-book estimates are flagged, marked on screen and segregable by a server;
    a global estimate would be none of those things.

    So global fills up from real reading only. It reads "-" until about 25 pages
    have been read on this firmware, which is one session.
    """
    path = cache_root / GLOBAL_STATS_NAME
    if not path.is_file():
        return "global: no global_stats.bin"
    try:
        raw = path.read_bytes()
    except OSError as exc:
        return f"global: {exc}"
    if len(raw) != GLOBAL_STATS_SIZE or raw[0] != GLOBAL_STATS_VERSION:
        return f"global: not a v{GLOBAL_STATS_VERSION} file ({len(raw)} bytes); flash the firmware first"

    hist = read_global_histogram(raw)
    total_seconds = read_le32(raw, 5)
    total_pages = read_le32(raw, 9)

    if args.dump:
        return (
            f"global: {format_histogram(hist)} | untrimmed={_wpm(hist.untrimmed_wpm())} "
            f"trimmed={_wpm(hist.trimmed_wpm())} seconds={total_seconds} pages={total_pages}"
        )

    if hist.empty():
        return "global: not seeded (measurement-only by design); fills in after ~25 pages of reading"
    return f"global: left alone, {hist.total_count()} measured sample(s), {_wpm(hist.trimmed_wpm())} wpm"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("sd_root", nargs="?", type=Path, help="Mount point of the SD card (holding .crosspoint)")
    parser.add_argument("--apply", action="store_true", help="Actually write files (default is a dry run)")
    parser.add_argument("--force", action="store_true", help="Redo a book already backfilled and not read since")
    parser.add_argument(
        "--spine-words",
        type=Path,
        metavar="BOOK.EPUB",
        help="Print this EPUB's per-spine word counts and exit, for calibrating against device logs",
    )
    parser.add_argument("--dump", action="store_true", help="Print each histogram and both readings, change nothing")
    parser.add_argument("--selftest", action="store_true", help="Run this script's own tests and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        result = unittest.main(argv=[sys.argv[0], "-v"], module=__name__, exit=False).result
        return 0 if result.wasSuccessful() else 1

    if args.spine_words is not None:
        try:
            counts = spine_word_counts(args.spine_words)
        except (OSError, ValueError, zipfile.BadZipFile, ElementTree.ParseError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        for i, words in enumerate(counts):
            print(f"spine {i:3d}  {words:7d} words")
        print(f"total     {sum(counts):7d} words")
        return 0

    if args.sd_root is None:
        parser.error("sd_root is required unless --selftest or --spine-words is given")

    sd_root: Path = args.sd_root.resolve()
    cache_root = sd_root / CACHE_DIR_NAME
    if not cache_root.is_dir():
        print(f"error: no {CACHE_DIR_NAME}/ directory under {sd_root}", file=sys.stderr)
        return 1

    books = sorted(p for p in sd_root.rglob(f"*{EPUB_SUFFIX}") if p.is_file() and CACHE_DIR_NAME not in p.parts)
    if not books:
        print(f"error: no {EPUB_SUFFIX} files under {sd_root}", file=sys.stderr)
        return 1

    if not args.apply and not args.dump:
        print("DRY RUN -- no files will be written. Re-run with --apply.\n")

    results = [process_book(book, sd_root, cache_root, args) for book in books]
    width = max(len(r.title) for r in results)
    for r in results:
        if r.status == "ok":
            summary = f"{r.words_read} words / {r.seconds}s = {r.wpm} wpm"
        elif r.status == "dump":
            summary = ""
        else:
            summary = "SKIP"
        line = f"{r.title.ljust(width)}  {summary}  {r.detail}" if summary else f"{r.title.ljust(width)}  {r.detail}"
        print(line.rstrip())

    accepted = [r for r in results if r.status == "ok"]
    print()
    print(process_global(cache_root, args))
    if not args.dump:
        print(f"\n{len(accepted)} book(s) backfilled, {len(results) - len(accepted)} skipped.")
        if not args.apply:
            print("Nothing was written. Re-run with --apply.")
    return 0


# --- Self-test ---------------------------------------------------------------
# Runs with --selftest. The card is only visible from the Windows host, so this
# is how the arithmetic gets verified anywhere else.


def _blank_book_stats(seconds: int = 3600, pages: int = 500, completed: bool = False) -> bytes:
    buf = bytearray(STATS_FILE_SIZE)
    buf[0] = STATS_FILE_VERSION
    write_le32(buf, 3, seconds)
    write_le32(buf, 7, pages)
    buf[11] = 1 if completed else 0
    return bytes(buf)


def _blank_global_stats(seconds: int = 36000, pages: int = 5000) -> bytes:
    buf = bytearray(GLOBAL_STATS_SIZE)
    buf[0] = GLOBAL_STATS_VERSION
    write_le32(buf, 5, seconds)
    write_le32(buf, 9, pages)
    return bytes(buf)


def _make_epub(path: Path, chapters: list[str]) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip")
        zf.writestr(
            "META-INF/container.xml",
            '<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
            "</rootfiles></container>",
        )
        items = []
        refs = []
        for i, body in enumerate(chapters):
            name = f"chap{i}.xhtml"
            zf.writestr(
                f"OEBPS/{name}",
                f"<html><head><title>Chapter Title Words Here</title></head><body>{body}</body></html>",
            )
            items.append(f'<item id="c{i}" href="{name}" media-type="application/xhtml+xml"/>')
            refs.append(f'<itemref idref="c{i}"/>')
        zf.writestr(
            "OEBPS/content.opf",
            '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
            f'<manifest>{"".join(items)}</manifest><spine>{"".join(refs)}</spine></package>',
        )


class WordRuleTests(unittest.TestCase):
    def test_latin_words(self):
        self.assertEqual(count_words("the quick brown fox"), 4)

    def test_punctuation_is_not_a_word(self):
        self.assertEqual(count_words("hello . \u2014 \u2022 world"), 2)

    def test_cjk_counts_characters(self):
        # Four characters, reported as two words by the shared convention.
        self.assertEqual(count_words("\u65e5 \u672c \u8a9e \u6587"), 2)

    def test_cjk_run_in_one_token(self):
        self.assertEqual(count_words("\u65e5\u672c\u8a9e\u6587"), 2)

    def test_cjk_punctuation_is_neither(self):
        self.assertEqual(count_words("\u65e5\u3002\u300c\u672c"), 1)

    def test_cjk_rounds_to_nearest(self):
        self.assertEqual(count_words("\u65e5"), 1)
        self.assertEqual(count_words("\u65e5\u672c\u8a9e"), 2)

    def test_mixed_script(self):
        self.assertEqual(count_words("EPUB \u65e5 \u672c reader"), 3)


class ExtractionTests(unittest.TestCase):
    def test_head_is_stripped(self):
        text = visible_text(b"<html><head><title>Not Body Words</title></head><body>one two</body></html>")
        self.assertEqual(count_words(text), 2)

    def test_scripts_and_comments_are_stripped(self):
        text = visible_text(b"<body><script>var a = 1;</script><!-- hidden words --><p>one two</p></body>")
        self.assertEqual(count_words(text), 2)

    def test_entities_are_decoded(self):
        self.assertEqual(count_words(visible_text(b"<p>caf&#233; na&#239;ve</p>")), 2)

    def test_tags_do_not_glue_words_together(self):
        self.assertEqual(count_words(visible_text(b"<p>one</p><p>two</p>")), 2)

    def test_spine_order_and_counts(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "book.epub"
            _make_epub(path, ["<p>one two three</p>", "<p>four five</p>"])
            self.assertEqual(spine_word_counts(path), [3, 2])


class EstimateTests(unittest.TestCase):
    def test_completed_book_is_its_whole_word_count(self):
        self.assertEqual(estimate_words_read([100, 200, 300], True, (0, 1, 10)), 600)

    def test_in_progress_is_prior_spines_plus_a_page_slice(self):
        # Halfway through the second of three spines.
        self.assertEqual(estimate_words_read([100, 200, 300], False, (1, 5, 10)), 200)

    def test_missing_progress_assumes_the_whole_book(self):
        self.assertEqual(estimate_words_read([100, 200], False, None), 300)

    def test_out_of_range_spine_is_clamped(self):
        self.assertEqual(estimate_words_read([100, 200], False, (9, 1, 2)), 300)

    def test_page_number_beyond_page_count_is_clamped(self):
        self.assertEqual(estimate_words_read([100, 200], False, (1, 99, 10)), 300)


class HistogramTests(unittest.TestCase):
    def test_bin_edges_match_the_firmware(self):
        self.assertEqual([wpm_bin_index(w) for w in (0, 49, 50, 199, 200, 299, 300, 550, 5000)],
                         [0, 0, 1, 3, 4, 4, 5, 7, 7])

    def test_single_bin_is_reproduced_by_the_trim(self):
        # Exact when the seed count divides the bin evenly...
        hist = Histogram()
        hist.count[4] = MIN_WPM_SAMPLES
        hist.words[4] = 90000
        hist.seconds[4] = 21600
        self.assertEqual(hist.trimmed_wpm(), 250)

        # ...and within a wpm when it does not. The plan claimed this case was
        # exact; the pro-rata slice is integer division, so it is not quite.
        rough = Histogram()
        rough.count[4] = MIN_WPM_SAMPLES
        rough.words[4] = 3000
        rough.seconds[4] = 720
        self.assertIn(rough.trimmed_wpm(), (249, 250))

    def test_trim_removes_the_slow_side(self):
        hist = Histogram()
        hist.count[5] = 200
        hist.words[5] = 14000
        hist.seconds[5] = 2333
        hist.count[0] = 12
        hist.words[0] = 840
        hist.seconds[0] = 2016
        self.assertLess(hist.untrimmed_wpm(), 250)
        self.assertGreater(hist.trimmed_wpm(), 350)

    def test_below_the_minimum_nothing_is_trimmed(self):
        hist = Histogram()
        hist.count[4] = 10
        hist.words[4] = 700
        hist.seconds[4] = 170
        self.assertEqual(hist.trimmed_wpm(), hist.untrimmed_wpm())


class SeedingTests(unittest.TestCase):
    def test_seed_round_trips_and_reproduces_the_estimate(self):
        seeded = seed_book_stats(_blank_book_stats(seconds=3600), 15000, 3600, 250)
        stats = parse_book_stats(seeded)
        self.assertTrue(stats.words_backfilled)
        self.assertEqual(stats.stats_revision, 1)
        self.assertEqual(stats.histogram.total_count(), MIN_WPM_SAMPLES)
        self.assertEqual(stats.histogram.trimmed_wpm(), 250)
        # Seeded into the bin its own rate belongs in.
        self.assertEqual(stats.histogram.count[wpm_bin_index(250)], MIN_WPM_SAMPLES)

    def test_seed_does_not_disturb_the_rest_of_the_file(self):
        original = _blank_book_stats(seconds=3600, pages=500)
        seeded = seed_book_stats(original, 15000, 3600, 250)
        self.assertEqual(len(seeded), STATS_FILE_SIZE)
        self.assertEqual(seeded[:BOOK_FLAGS_OFFSET], original[:BOOK_FLAGS_OFFSET])
        self.assertEqual(seeded[BOOK_FLAGS_OFFSET + 1 : BOOK_BIN_COUNT_OFFSET],
                         original[BOOK_FLAGS_OFFSET + 1 : BOOK_BIN_COUNT_OFFSET])
        # The reserved tail stays zero.
        self.assertEqual(seeded[157:165], bytes(8))

    def test_v5_file_is_refused(self):
        with self.assertRaises(ValueError):
            parse_book_stats(bytes([5]) + bytes(72))

    def test_seed_count_is_the_display_minimum_not_the_page_count(self):
        # A whole history of votes in one bin would spend the trim budget inside
        # the synthetic bin for years.
        seeded = seed_book_stats(_blank_book_stats(pages=9999), 15000, 3600, 250)
        self.assertEqual(parse_book_stats(seeded).histogram.total_count(), MIN_WPM_SAMPLES)


class EndToEndTests(unittest.TestCase):
    def _card(self, tmp: Path, body_words: int = 3000, seconds: int = 720) -> tuple[Path, Path]:
        sd_root = tmp / "card"
        (sd_root / "books").mkdir(parents=True)
        book = sd_root / "books" / "novel.epub"
        _make_epub(book, ["<p>" + " ".join(["word"] * body_words) + "</p>"])

        cache = sd_root / CACHE_DIR_NAME / cache_dir_name(device_path_for(book, sd_root))
        cache.mkdir(parents=True)
        (cache / STATS_FILE_NAME).write_bytes(_blank_book_stats(seconds=seconds, pages=100, completed=True))
        (sd_root / CACHE_DIR_NAME / GLOBAL_STATS_NAME).write_bytes(_blank_global_stats(seconds=seconds, pages=100))
        return sd_root, cache

    def test_dry_run_writes_nothing(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            sd_root, cache = self._card(Path(tmp))
            before = (cache / STATS_FILE_NAME).read_bytes()
            self.assertEqual(main([str(sd_root)]), 0)
            self.assertEqual((cache / STATS_FILE_NAME).read_bytes(), before)

    def test_apply_seeds_the_book_but_never_global(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            sd_root, cache = self._card(Path(tmp))
            self.assertEqual(main([str(sd_root), "--apply"]), 0)

            stats = parse_book_stats((cache / STATS_FILE_NAME).read_bytes())
            self.assertTrue(stats.words_backfilled)
            # 3000 words / 720 s = 250 wpm, reproduced to within a wpm: the trim's
            # pro-rata slice is integer division, so the ratio only survives exactly
            # when the seed count divides the words and seconds evenly.
            self.assertIn(stats.histogram.trimmed_wpm(), (249, 250))
            self.assertTrue(list((cache).glob("*.bak")))

            # Global is never seeded: an estimate there could not be flagged, and
            # would dominate the shared figure for months of real reading.
            global_before = _blank_global_stats(seconds=720, pages=100)
            self.assertEqual((sd_root / CACHE_DIR_NAME / GLOBAL_STATS_NAME).read_bytes(), global_before)

    def test_second_run_skips_and_force_redoes(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            sd_root, cache = self._card(Path(tmp))
            main([str(sd_root), "--apply"])
            after_first = parse_book_stats((cache / STATS_FILE_NAME).read_bytes())

            main([str(sd_root), "--apply"])
            self.assertEqual(parse_book_stats((cache / STATS_FILE_NAME).read_bytes()).stats_revision,
                             after_first.stats_revision)

            main([str(sd_root), "--apply", "--force"])
            self.assertEqual(parse_book_stats((cache / STATS_FILE_NAME).read_bytes()).stats_revision,
                             after_first.stats_revision + 1)

    def test_measured_samples_are_never_overwritten(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            sd_root, cache = self._card(Path(tmp))
            measured = bytearray(_blank_book_stats(seconds=720, pages=100, completed=True))
            write_le16(measured, BOOK_BIN_COUNT_OFFSET + 4 * 2, 300)
            write_le32(measured, BOOK_BIN_WORDS_OFFSET + 4 * 4, 21000)
            write_le32(measured, BOOK_BIN_SECONDS_OFFSET + 4 * 4, 5040)
            (cache / STATS_FILE_NAME).write_bytes(bytes(measured))

            main([str(sd_root), "--apply", "--force"])
            self.assertEqual((cache / STATS_FILE_NAME).read_bytes(), bytes(measured))

    def test_out_of_band_book_is_skipped_not_guessed(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            # 3000 words in 60 s is 3000 wpm: a re-read or a wrong edition.
            sd_root, cache = self._card(Path(tmp), seconds=60)
            before = (cache / STATS_FILE_NAME).read_bytes()
            main([str(sd_root), "--apply"])
            self.assertEqual((cache / STATS_FILE_NAME).read_bytes(), before)


if __name__ == "__main__":
    sys.exit(main())
