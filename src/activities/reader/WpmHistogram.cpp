#include "WpmHistogram.h"

size_t wpmBinIndex(const uint32_t wpm) {
  for (size_t i = 0; i + 1 < WPM_BIN_COUNT; ++i) {
    if (wpm < WPM_BIN_UPPER[i]) return i;
  }
  return WPM_BIN_COUNT - 1;
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
