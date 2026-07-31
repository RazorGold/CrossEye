#include "WpmHistogram.h"

size_t wpmBinIndex(const uint32_t wpm) {
  for (size_t i = 0; i + 1 < WPM_BIN_COUNT; ++i) {
    if (wpm < WPM_BIN_UPPER[i]) return i;
  }
  return WPM_BIN_COUNT - 1;
}

WpmReading trimmedWordsPerMinute(const uint32_t count[WPM_BIN_COUNT], const uint32_t words[WPM_BIN_COUNT],
                                 const uint32_t seconds[WPM_BIN_COUNT], const bool untrimmedFallback) {
  WpmReading reading;

  // 64-bit sums: a lifetime of global bins can carry more words than a uint32_t
  // holds, and this runs once, at display time, off the reading path.
  uint64_t totalCount = 0;
  uint64_t totalWords = 0;
  uint64_t totalSeconds = 0;
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    totalCount += count[i];
    totalWords += words[i];
    totalSeconds += seconds[i];
  }

  // All bins zero means not measured, which is also the sole condition the backfill
  // script keys on.
  if (totalCount == 0 || totalSeconds == 0) return reading;

  uint64_t drop = 0;
  if (totalCount < MIN_WPM_SAMPLES) {
    if (!untrimmedFallback) return reading;
  } else {
    drop = totalCount * WPM_TRIM_PERCENT / 100;
  }

  // Low bins first: the slow side is the only side worth trimming.
  for (size_t i = 0; i < WPM_BIN_COUNT && drop > 0; ++i) {
    if (count[i] == 0) continue;
    if (count[i] <= drop) {
      totalWords -= words[i];
      totalSeconds -= seconds[i];
      drop -= count[i];
      continue;
    }
    // Pro-rata within the boundary bin, so there is no whole-bin cliff: a bin
    // holding 12% of pages gives up 10/12 of its words and seconds rather than all
    // of them. Whole-bin trimming failed exactly where it mattered most, in bin 0.
    //
    // Integer division, so a single-bin histogram is reproduced to within a wpm
    // rather than bit-exactly — the ratio only survives untouched when the count
    // divides the bin's words and seconds evenly. Irrelevant to a displayed
    // integer, but the sync server must use the same arithmetic to agree.
    totalWords -= static_cast<uint64_t>(words[i]) * drop / count[i];
    totalSeconds -= static_cast<uint64_t>(seconds[i]) * drop / count[i];
    drop = 0;
  }

  if (totalSeconds == 0) return reading;

  const uint64_t wpm = totalWords * 60ULL / totalSeconds;
  reading.available = true;
  reading.wordsPerMinute = wpm > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(wpm);
  return reading;
}

WpmReading bookWordsPerMinute(const BookReadingStats& stats) {
  // Under a minute of reading is not a pace, whatever the bins say.
  if (stats.totalReadingSeconds <= 60) return {};

  uint32_t count[WPM_BIN_COUNT];
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) count[i] = stats.wpmBinCount[i];

  // No untrimmed fallback per book: below the minimum the card shows nothing rather
  // than an interruption-contaminated average, which is the very figure Words/Min
  // exists to replace. Showing it for a new book's first session and then having it
  // jump when the reducer engaged would be worse than showing nothing.
  WpmReading reading = trimmedWordsPerMinute(count, stats.wpmBinWords.data(), stats.wpmBinSeconds.data(), false);

  // The backfill seeds exactly MIN_WPM_SAMPLES votes into one bin, so a total still
  // at or below that means no measured sample has arrived yet.
  if (reading.available && stats.wordsBackfilled) {
    uint32_t totalCount = 0;
    for (size_t i = 0; i < WPM_BIN_COUNT; ++i) totalCount += stats.wpmBinCount[i];
    reading.estimated = totalCount <= MIN_WPM_SAMPLES;
  }
  return reading;
}

WpmReading globalWordsPerMinute(const GlobalReadingStats& stats) {
  if (stats.totalReadingSeconds <= 60) return {};
  return trimmedWordsPerMinute(stats.wpmBinCount.data(), stats.wpmBinWords.data(), stats.wpmBinSeconds.data(), true);
}

bool WpmSessionBins::addSample(const uint32_t dwellMs, const uint16_t pageWords) {
  // An image page, or a page whose blocks yielded no word tokens, is real reading
  // time but carries no rate.
  if (pageWords == 0) return false;
  if (dwellMs < MIN_SAMPLE_DWELL_MS) return false;

  // Rate from milliseconds, not from the truncated seconds below: on a 70-word
  // page consecutive whole seconds sit 15-20 wpm apart near typical pace but
  // 75-100 wpm apart above 500 wpm, which would coarsen bin placement exactly
  // where the bins are widest.
  const uint32_t wpm = static_cast<uint32_t>(pageWords) * 60000UL / dwellMs;
  if (pageWords >= MIN_WORDS_FOR_RATE_GATE && wpm > MAX_PLAUSIBLE_WPM) return false;

  // Rounded, not floored. The per-page flooring this replaces cost ~3% of an X3's
  // reading time in one direction; rounding leaves a ±0.5 s error with a mean of
  // zero, and the bins are a rate distribution rather than the reading-time total.
  const uint32_t sampleSeconds = (dwellMs + 500UL) / 1000UL;
  if (sampleSeconds == 0) return false;

  const size_t bin = wpmBinIndex(wpm);
  count[bin] = addSaturated<uint16_t>(count[bin], 1);
  words[bin] = addSaturated<uint32_t>(words[bin], pageWords);
  seconds[bin] = addSaturated<uint32_t>(seconds[bin], sampleSeconds);
  return true;
}

void WpmSessionBins::mergeInto(BookReadingStats& stats) const {
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    stats.wpmBinCount[i] = addSaturated<uint16_t>(stats.wpmBinCount[i], count[i]);
    stats.wpmBinWords[i] = addSaturated<uint32_t>(stats.wpmBinWords[i], words[i]);
    stats.wpmBinSeconds[i] = addSaturated<uint32_t>(stats.wpmBinSeconds[i], seconds[i]);
  }
}

void WpmSessionBins::mergeInto(GlobalReadingStats& stats) const {
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    stats.wpmBinCount[i] = addSaturated<uint32_t>(stats.wpmBinCount[i], count[i]);
    stats.wpmBinWords[i] = addSaturated<uint32_t>(stats.wpmBinWords[i], words[i]);
    stats.wpmBinSeconds[i] = addSaturated<uint32_t>(stats.wpmBinSeconds[i], seconds[i]);
  }
}

void WpmSessionBins::clear() {
  count.fill(0);
  words.fill(0);
  seconds.fill(0);
}

bool WpmSessionBins::empty() const {
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    if (count[i] != 0) return false;
  }
  return true;
}
