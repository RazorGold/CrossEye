# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`,
plus the reading stats files under `/.crosspoint/`. All POD fields are written in
the ESP32 little-endian representation used by `Serialization.h`; strings are
length-prefixed UTF-8.

## `book.bin`

### Version 7

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 7
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 30

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 30 is binary-identical to version 29. The version was bumped because
Arabic contextual shaping changed text measurement (`getTextAdvanceX` now
measures the shaped visual text), so word positions cached by v29 no longer
match what `drawText` renders.

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 30
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `stats_v6.bin`

### Version 6

Per-book reading statistics, at `/.crosspoint/epub_<hash>/stats_v6.bin`. Written
by `BookReadingStats` (`src/activities/reader/`), parsed by
`parseStatsBuffer()` in `ReadingStatsSerialization.cpp`.

Unlike the cache files above, a version bump here must **not** discard the file:
these are user statistics, not a rebuildable cache. The loader dispatches on the
`(file size, version byte)` pair and every historical pair still parses. Older
files are left in place rather than deleted, so `stats_v5.bin` remains as a
fallback after the upgrade.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `version` | 6 |
| 1 | 2 | `sessionCount` | uint16 LE |
| 3 | 4 | `totalReadingSeconds` | uint32 LE, elapsed reading time, keeps interruptions up to the idle threshold |
| 7 | 4 | `totalPagesTurned` | uint32 LE, no longer displayed but still maintained |
| 11 | 1 | `isCompleted` | |
| 12 | 2 | *reserved* | was `avgSecondsPerForwardPage`; written 0 from v6 |
| 14 | 2 | *reserved* | was `paceSampleCount`; written 0 from v6 |
| 16 | 1 | `flags` | bit0 `startDateManual`, bit1 `finishedDateManual`, bit2 `wordsBackfilled` |
| 17 | 4 | `startDate` | year uint16 LE, month uint8, day uint8 |
| 21 | 4 | `finishedDate` | same encoding |
| 25 | 16 | `timeOfDaySeconds[4]` | uint32 LE each |
| 41 | 28 | `dayOfWeekSeconds[7]` | uint32 LE each |
| 69 | 4 | `estimatedTimeLeftSeconds` | uint32 LE, 0 means unavailable |
| 73 | 16 | `wpmBinCount[8]` | uint16 LE each, saturating |
| 89 | 32 | `wpmBinWords[8]` | uint32 LE each |
| 121 | 32 | `wpmBinSeconds[8]` | uint32 LE each |
| 153 | 4 | `statsRevision` | uint32 LE, monotonic, incremented on every save |
| 157 | 8 | *reserved* | written 0 |

Total: 165 bytes.

Earlier versions, all still parsed: v1 (11 bytes, through `totalPagesTurned`),
v2 (12, adds `isCompleted`), v3 (16, adds the two pace fields), v4 (69, adds the
flags, dates and buckets), v5 (73, adds `estimatedTimeLeftSeconds`). Bytes
`[0..72]` keep their v5 meaning and offsets in v6.

### The Words/Min histogram

Bytes `[73..152]` are three parallel arrays over the same eight bins of implied
reading rate, defined in `ReadingStatsUtils.h`:

```c++
constexpr uint16_t WPM_BIN_UPPER[8] = {50, 100, 150, 200, 300, 400, 550, UINT16_MAX};
```

Bin `i` covers `[WPM_BIN_UPPER[i - 1], WPM_BIN_UPPER[i])`, bin 0 starts at 0, and
bin 7 is open-ended. One forward page turn adds one vote to `wpmBinCount` and adds
that page's words and seconds to `wpmBinWords` / `wpmBinSeconds`.

The two kinds of value do different jobs. **Counts locate the display-time trim
boundary** — one page, one vote, so a single four-minute distracted page cannot
swallow the whole trim budget. **Sums produce the number**, as an exact ratio of
accumulated words to accumulated seconds, so the reported figure carries no
quantization error regardless of bin width. Bin geometry decides only where the
trim boundary falls, which is why the bins below 200 wpm are narrow: that is where
interruptions land.

Nothing is ever removed from the distribution, so the trim percentage stays a
display-time choice that can be changed later with no format bump, no migration
and no lost history.

**Reducing the bins to a number** (`trimmedWordsPerMinute()` in
`WpmHistogram.cpp`) is one rule, used by both stats cards and intended for the
sync server too, so every device and every friend is ranked by identical
arithmetic:

1. Sum the counts. All zero means not measured.
2. Below `MIN_WPM_SAMPLES` (25, about one 9-minute session), the per-book card
   reports nothing and the global card reports the untrimmed ratio.
3. Otherwise drop the slowest `WPM_TRIM_PERCENT` (10%) of pages **by count**,
   walking the bins from bin 0 upwards, and report `words * 60 / seconds` over
   what remains.

Counts locate the boundary so that one page is one vote: trimming by *seconds*
instead would let a single four-minute distracted page count as 240 units of
"slowest 10%" and spend the entire budget. The boundary bin is pro-rated rather
than dropped whole, so a bin holding 12% of pages gives up 10/12 of its words
and seconds. That arithmetic is integer, so a single-bin histogram — what the
backfill script produces — is reproduced to within a wpm rather than bit-exactly.

The trim is asymmetric because the contamination is. The figure is time-weighted:
a distracted page sitting for four minutes drags the ratio down hard, while a page
flipped through in three seconds contributes three seconds and cannot move a ratio
built from hours.

`Σ wpmBinWords[i]` is a **rate numerator, not a count of words read**: a page whose
dwell was fragmented (by opening the stats screen, or by closing the book) can
contribute its words more than once, along with its seconds. Never display or
transmit it as a total.

`scripts/backfill_word_stats.py` seeds the **per-book** histogram for books that
were read before the stat existed, and its `--dump` mode prints the histograms
already on a card alongside both readings, trimmed and untrimmed. It is
stdlib-only and runs wherever the card is visible.

The **global** histogram is never seeded, by design. It has no flags field to
mark an estimate with, so a seeded bin would merge with later measured samples
and become permanently indistinguishable from them; and because the figure is
time-weighted, a seed carrying a whole reading history's worth of seconds would
dominate it for months of real reading. Global therefore holds measured samples
only, and reads `-` until about one session has been read on this firmware.

## `global_stats.bin`

### Version 4

Cumulative statistics across all books, at `/.crosspoint/global_stats.bin`.
Written by `GlobalReadingStats`. The same file layout is used for the per-device
files under `/.crosspoint/synced_stats/`, which are summed on read.

At 270 bytes the file exceeds the 256-byte stack-local limit, so both the load and
the save walk it in **two sequential passes** over one 159-byte buffer: the header
block (the v3 layout, unchanged) and then the tail block.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `version` | 4 |
| 1 | 4 | `totalSessions` | uint32 LE |
| 5 | 4 | `totalReadingSeconds` | uint32 LE |
| 9 | 4 | `totalPagesTurned` | uint32 LE |
| 13 | 4 | `completedBooks` | uint32 LE |
| 17 | 16 | `timeOfDaySeconds[4]` | uint32 LE each |
| 33 | 28 | `dayOfWeekSeconds[7]` | uint32 LE each |
| 61 | 4 | `readingHistoryAnchorDay` | uint32 LE |
| 65 | 92 | `readingHistoryBits[92]` | one bit per day, 730 days |
| 157 | 2 | `longestReadingStreak` | uint16 LE |
| 159 | 32 | `wpmBinCount[8]` | uint32 LE each — wider than per-book, see below |
| 191 | 32 | `wpmBinWords[8]` | uint32 LE each |
| 223 | 32 | `wpmBinSeconds[8]` | uint32 LE each |
| 255 | 4 | `statsRevision` | uint32 LE, monotonic, incremented on every save |
| 259 | 1 | `tokenRuleVersion` | word-token rule that produced the bins |
| 260 | 2 | `idleThresholdSeconds` | uint16 LE, snapshot of the setting at save time |
| 262 | 8 | *reserved* | written 0 |

Total: 270 bytes. Bytes `[0..158]` are the header block; `[159..269]` are the tail.

Earlier versions, all still parsed: v1 (13 bytes), v2 (17, adds
`completedBooks`), v3 (159, adds the buckets, the reading history bitmap and the
streak).

Global bin counts are `uint32_t` where the per-book ones are `uint16_t`: 65535
pages in a single bin is around 312 hours of reading, which one book will never
reach but a heavy reader's lifetime total will. A saturated count under-weights
itself when the trim boundary is located, which shifts the reported number
quietly.

The three scalars at `[255..261]` each prevent a specific silent error once these
files are synced to a server:

| Field | Prevents |
|---|---|
| `statsRevision` | a stale upload, a restored card backup or a rollback being merged as though it were new data |
| `tokenRuleVersion` | ranking two devices whose firmware tokenizes words differently |
| `idleThresholdSeconds` | comparing reading time between a 300 s device and a 120 s device as though they measured the same thing |

`tokenRuleVersion` is `TOKEN_RULE_VERSION` in `ReadingStatsUtils.h`, snapshotted
at save time along with `idleThresholdSeconds`. Bump it whenever word
tokenization changes, so incompatible histograms are never ranked against each
other.

### Word token rule, version 1

`WordTokenRule.{h,cpp}` holds the one rule that everything counting words must
agree on: the firmware's page counter, the dictionary's word selection, and the
host backfill script. Counting happens over the **laid-out page**, not the raw
XHTML, so images, chapter-end partials and hyphenation splits are already
resolved.

A token counts as a word when it carries an ASCII alphanumeric, or a non-ASCII
codepoint that is not punctuation. Excluded:

- General Punctuation (`U+2000`–`U+206F`) — dashes, bullets and quotes appear as
  standalone tokens;
- CJK and fullwidth punctuation (`U+3000`–`U+303F`, `U+FE30`–`U+FE4F`, and the
  punctuation subranges of `U+FF01`–`U+FF60`);
- the replacement glyph `U+FFFD`, so a damaged block cannot inflate a count;
- **hyphenation prefixes**, i.e. tokens ending in `-`. `ParsedText` appends a
  literal hyphen to the prefix when it splits a word across a line break, so the
  remainder token carries the word. This also gets words broken at an existing
  hyphen right: they add no character, but the prefix still ends in `-`, so the
  two halves count as the one word they were.

**CJK is counted in characters, not tokens.** `ParsedText` splits a CJK run into
one token per character, so naive token counting reports a Chinese or Japanese
book as several times more words than it has. CJK word characters are tallied
separately and divided by `CJK_CHARS_PER_WORD = 2`, rounded to nearest, once per
page. That divisor is a documented convention rather than a measurement —
Chinese runs closer to 1.5 characters per word and Japanese closer to 2 — but it
is stable, versioned, and reachable by the script without reimplementing the
tokenizer.

The count is independent of Focus Reading: `TextBlock` merges the bold prefix and
its suffix back into one word entry carrying a `focusBoundary`, so `wordCount()`
does not move with the setting. A word count that changed with a render setting
would defeat the point of Words/Min.

### Durability and downgrades

Both files are replaced by writing a temp file, flushing it, closing it with the
result checked, verifying its size on disk and then renaming it over the live
path (`writeStatsFileAtomically()` in `ReadingStatsFileIo.cpp`). An interrupted
save therefore leaves either the old file or the new one, never a truncated file —
which matters most for the per-book file, where a truncated `stats_v6.bin` would
make the loader fall back to the stale `stats_v5.bin` and silently resurrect
pre-migration statistics.

`global_stats.bin` additionally rotates to `global_stats.bin.bak` on every save.
That backup is one book-close away from holding the same data as the primary, so
the first upgrade of a pre-v4 file also writes a one-shot
`global_stats.v<version>.bak` — `global_stats.v3.bak` in practice — that nothing
ever rotates.

**Downgrading firmware after the upgrade** is safe but looks alarming. Per-book,
the v5 file is still there, so old firmware reads stale statistics rather than
corrupt ones. Globally, old firmware sees a 270-byte file, treats it as a newer
format, refuses to overwrite it and displays zeros — the data is intact, the
screen is not.
