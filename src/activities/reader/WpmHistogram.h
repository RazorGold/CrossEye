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
  bool empty() const;
};
