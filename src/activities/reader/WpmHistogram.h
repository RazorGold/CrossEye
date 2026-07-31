#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Sampling side of the Words/Min histogram: which page reads become samples, which
// bin they land in, and how a session's samples reach the two stats files. Pure —
// no HAL, no Arduino — so test/reading_stats/ covers the admission rules directly.

// A page-flip burst is not reading. This floor is page-size independent, which
// matters because MAX_PLAUSIBLE_WPM alone barely fires on a small screen: on a
// 70-word X3 page it only catches dwells under 2.1 s, so a 3 s/page flip implies
// 1420 wpm and would sail through.
constexpr uint32_t MIN_SAMPLE_DWELL_MS = 3000;

// Cheap backstop for absurd word counts, applied only to pages with enough words
// for the implied rate to mean anything.
constexpr uint32_t MAX_PLAUSIBLE_WPM = 2000;
constexpr uint16_t MIN_WORDS_FOR_RATE_GATE = 40;

// Index of the bin holding this implied rate. Bin i covers
// [WPM_BIN_UPPER[i - 1], WPM_BIN_UPPER[i]), bin 0 starts at 0, bin 7 is open-ended.
size_t wpmBinIndex(uint32_t wpm);

// Share of the slowest pages the display-time reducer drops. A display-time
// constant over retained data, so changing it costs a firmware change and nothing
// else: no format bump, no migration, no lost history, and it re-reads all
// existing history correctly. That property is why the histogram is stored at all.
constexpr uint32_t WPM_TRIM_PERCENT = 10;

// Roughly one 9-minute session at 3.5 pages/min. Below it a per-book figure would
// be one interruption away from meaningless, so the card shows nothing instead.
constexpr uint32_t MIN_WPM_SAMPLES = 25;

struct WpmReading {
  bool available = false;
  uint16_t wordsPerMinute = 0;
  // The figure is partly or wholly the backfill script's estimate rather than
  // measurement. Backfilled figures are derived from totalReadingSeconds, which
  // includes idle time up to the threshold, so they read 20-30% lower than a
  // measured pace and must not be presented as the same statistic. Stays true for
  // the life of the book: the seeded words and seconds are never removed from the
  // histogram, so the figure never becomes purely measured.
  bool estimated = false;
};

// The one reducer, used for both cards and by the sync server: trim the slowest
// WPM_TRIM_PERCENT of pages by count, then report the exact ratio of the words and
// seconds that remain.
//
// Asymmetric by construction, because the contamination is. The figure is
// time-weighted — every page is weighted by the seconds it consumed — so a
// distracted page sitting for four minutes drags the ratio down hard while a page
// flipped through in three seconds contributes three seconds and cannot move it.
// Counts locate the boundary so one page is one vote: trimming by seconds instead
// would let a single distracted page spend the entire trim budget.
//
// untrimmedFallback decides what happens below MIN_WPM_SAMPLES: global reports the
// untrimmed ratio, which is a reasonable aggregate and is populated from first boot
// after a backfill, while per-book reports nothing.
WpmReading trimmedWordsPerMinute(const uint32_t count[WPM_BIN_COUNT], const uint32_t words[WPM_BIN_COUNT],
                                 const uint32_t seconds[WPM_BIN_COUNT], bool untrimmedFallback);

// Card-level entry points, carrying each file's sentinel policy.
WpmReading bookWordsPerMinute(const BookReadingStats& stats);
WpmReading globalWordsPerMinute(const GlobalReadingStats& stats);

// One session's samples, merged into the per-book and global files together with
// the session's time and pages. Buffered rather than applied per sample because
// the session commit is gated: a session too short to contribute reading time must
// not contribute votes either.
//
// uint16_t counts, matching the per-book file. 80 bytes.
struct WpmSessionBins {
  std::array<uint16_t, WPM_BIN_COUNT> count{};
  std::array<uint32_t, WPM_BIN_COUNT> words{};
  std::array<uint32_t, WPM_BIN_COUNT> seconds{};

  // Applies gate A — the dwell floor and the rate backstop — and bins the sample
  // if it passes. Returns true when a sample was added. This decides bin admission
  // only: the page's seconds and the page itself are committed by the caller under
  // the idle threshold alone, so no rate heuristic can ever discard reading time.
  bool addSample(uint32_t dwellMs, uint16_t pageWords);

  // Field-wise saturating merge into either file. Counts saturate at the target's
  // width, which is uint16_t per book and uint32_t in global.
  void mergeInto(BookReadingStats& stats) const;
  void mergeInto(GlobalReadingStats& stats) const;

  void clear();
};
