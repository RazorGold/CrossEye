#include <gtest/gtest.h>

#include "WpmHistogram.h"

// A page on the X3 is ~70 words at ~17 s of dwell, so the numbers below are sized
// to that rather than to a 250-word page: the gates behave very differently on a
// small screen, which is the whole reason the dwell floor exists.
namespace {
constexpr uint16_t X3_PAGE_WORDS = 70;

uint32_t totalCount(const WpmSessionBins& bins) {
  uint32_t total = 0;
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) total += bins.count[i];
  return total;
}

// Dwell that makes a page of `words` read at exactly `wpm`.
uint32_t dwellMsFor(const uint16_t words, const uint32_t wpm) {
  return static_cast<uint32_t>(words) * 60000UL / wpm;
}
}  // namespace

TEST(WpmBins, BinEdgesMatchTheDocumentedRanges) {
  EXPECT_EQ(wpmBinIndex(0), 0u);
  EXPECT_EQ(wpmBinIndex(49), 0u);
  EXPECT_EQ(wpmBinIndex(50), 1u);
  EXPECT_EQ(wpmBinIndex(99), 1u);
  EXPECT_EQ(wpmBinIndex(100), 2u);
  EXPECT_EQ(wpmBinIndex(150), 3u);
  EXPECT_EQ(wpmBinIndex(199), 3u);
  // The typical 17 s dwell on a 70-word page lands mid-distribution, in bin 4.
  EXPECT_EQ(wpmBinIndex(200), 4u);
  EXPECT_EQ(wpmBinIndex(247), 4u);
  EXPECT_EQ(wpmBinIndex(299), 4u);
  EXPECT_EQ(wpmBinIndex(300), 5u);
  EXPECT_EQ(wpmBinIndex(400), 6u);
  EXPECT_EQ(wpmBinIndex(549), 6u);
  // Bin 7 is open-ended: fast pages are kept, not discarded.
  EXPECT_EQ(wpmBinIndex(550), 7u);
  EXPECT_EQ(wpmBinIndex(5000), 7u);
  EXPECT_EQ(wpmBinIndex(UINT32_MAX), 7u);
}

TEST(WpmSample, TypicalPageIsBinnedWithExactSums) {
  WpmSessionBins bins;
  ASSERT_TRUE(bins.addSample(17000, X3_PAGE_WORDS));  // 70 words in 17 s = 247 wpm

  EXPECT_EQ(bins.count[4], 1);
  EXPECT_EQ(bins.words[4], 70u);
  EXPECT_EQ(bins.seconds[4], 17u);
}

TEST(WpmSample, PageFlipBurstIsRejectedByTheDwellFloor) {
  WpmSessionBins bins;
  // 3 s/page on a 70-word page implies 1400 wpm, which is under MAX_PLAUSIBLE_WPM:
  // the rate ceiling alone would admit a whole flip-through.
  EXPECT_LT(static_cast<uint32_t>(X3_PAGE_WORDS) * 60000UL / 2999UL, MAX_PLAUSIBLE_WPM);
  EXPECT_FALSE(bins.addSample(2999, X3_PAGE_WORDS));
  EXPECT_FALSE(bins.addSample(500, X3_PAGE_WORDS));
  EXPECT_EQ(totalCount(bins), 0u);

  // Exactly at the floor is admitted.
  EXPECT_TRUE(bins.addSample(MIN_SAMPLE_DWELL_MS, X3_PAGE_WORDS));
}

TEST(WpmSample, RateCeilingOnlyAppliesToPagesWithEnoughWords) {
  WpmSessionBins bins;
  // A dense page read absurdly fast: rejected.
  EXPECT_FALSE(bins.addSample(3000, 200));  // 4000 wpm
  EXPECT_EQ(totalCount(bins), 0u);

  // A chapter-end partial with a handful of words has no meaningful rate, so the
  // ceiling does not apply and the sample is kept.
  ASSERT_LT(20, MIN_WORDS_FOR_RATE_GATE);
  EXPECT_TRUE(bins.addSample(3000, 20));  // 400 wpm, under the ceiling anyway
  bins.clear();
  EXPECT_TRUE(bins.addSample(3000, 39));  // 780 wpm, above nothing that gates it
  EXPECT_EQ(bins.count[7], 1);
}

TEST(WpmSample, ImagePageIsRealTimeButNotASample) {
  WpmSessionBins bins;
  EXPECT_FALSE(bins.addSample(30000, 0));
  EXPECT_EQ(totalCount(bins), 0u);
}

TEST(WpmSample, SlowPagesLandInTheLowBinsWhereTheTrimWillFindThem) {
  WpmSessionBins bins;
  // A four-minute distracted page under a 300 s idle threshold: 70 words, 17 wpm.
  ASSERT_TRUE(bins.addSample(240000, X3_PAGE_WORDS));
  EXPECT_EQ(bins.count[0], 1);
  EXPECT_EQ(bins.seconds[0], 240u);

  // A hard passage at 120 wpm is legitimate slow reading, and lands clear of bin 0.
  ASSERT_TRUE(bins.addSample(dwellMsFor(X3_PAGE_WORDS, 120), X3_PAGE_WORDS));
  EXPECT_EQ(bins.count[2], 1);
}

TEST(WpmSample, SecondsAreRoundedNotFloored) {
  WpmSessionBins bins;
  ASSERT_TRUE(bins.addSample(17400, X3_PAGE_WORDS));
  EXPECT_EQ(bins.seconds[4], 17u);

  bins.clear();
  ASSERT_TRUE(bins.addSample(17600, X3_PAGE_WORDS));
  EXPECT_EQ(bins.seconds[4], 18u);
}

TEST(WpmSample, BinPlacementUsesMillisecondsNotWholeSeconds) {
  // On a 70-word page, 14.0 s is 300 wpm and 14.2 s is 295 — different bins that
  // whole-second dwell could not tell apart.
  WpmSessionBins fast;
  ASSERT_TRUE(fast.addSample(14000, X3_PAGE_WORDS));
  EXPECT_EQ(wpmBinIndex(300), 5u);
  EXPECT_EQ(fast.count[5], 1);

  WpmSessionBins slower;
  ASSERT_TRUE(slower.addSample(14200, X3_PAGE_WORDS));
  EXPECT_EQ(slower.count[4], 1);
}

TEST(WpmSession, MergeAddsIntoBothWidths) {
  WpmSessionBins bins;
  bins.count[4] = 30;
  bins.words[4] = 2100;
  bins.seconds[4] = 510;

  BookReadingStats book;
  book.wpmBinCount[4] = 5;
  book.wpmBinWords[4] = 350;
  book.wpmBinSeconds[4] = 85;
  bins.mergeInto(book);
  EXPECT_EQ(book.wpmBinCount[4], 35);
  EXPECT_EQ(book.wpmBinWords[4], 2450u);
  EXPECT_EQ(book.wpmBinSeconds[4], 595u);

  GlobalReadingStats global;
  global.wpmBinCount[4] = 70000;  // beyond a uint16_t, which is why global is wider
  bins.mergeInto(global);
  EXPECT_EQ(global.wpmBinCount[4], 70030u);
  EXPECT_EQ(global.wpmBinWords[4], 2100u);
}

TEST(WpmSession, MergeSaturatesRatherThanWrapping) {
  WpmSessionBins bins;
  bins.count[0] = 100;
  bins.words[0] = 1000;

  BookReadingStats book;
  book.wpmBinCount[0] = UINT16_MAX - 10;
  book.wpmBinWords[0] = UINT32_MAX - 10;
  bins.mergeInto(book);
  EXPECT_EQ(book.wpmBinCount[0], UINT16_MAX);
  EXPECT_EQ(book.wpmBinWords[0], UINT32_MAX);
}

TEST(WpmSession, ClearResetsEveryArray) {
  WpmSessionBins bins;
  ASSERT_TRUE(bins.addSample(17000, X3_PAGE_WORDS));
  bins.clear();
  EXPECT_EQ(totalCount(bins), 0u);
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    EXPECT_EQ(bins.count[i], 0);
    EXPECT_EQ(bins.words[i], 0u);
    EXPECT_EQ(bins.seconds[i], 0u);
  }
}

TEST(WpmSession, ASessionOfTypicalReadingLandsWhereTheCalibrationSaysItShould) {
  // ~31 pages at 3.5 pages/min, one of them interrupted for two minutes under the
  // 120 s threshold. The interrupted page is 3% of the pages and 18% of the time,
  // which is the effect the display-time trim exists to remove.
  WpmSessionBins bins;
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(bins.addSample(17000, X3_PAGE_WORDS));
  }
  ASSERT_TRUE(bins.addSample(120000, X3_PAGE_WORDS));

  EXPECT_EQ(bins.count[4], 30);
  EXPECT_EQ(bins.count[0], 1);

  uint32_t totalSeconds = 0;
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) totalSeconds += bins.seconds[i];
  EXPECT_EQ(totalSeconds, 30u * 17u + 120u);
  EXPECT_GT(bins.seconds[0] * 100 / totalSeconds, 18u);
}

// ---------------------------------------------------------------- reducer ---

namespace {

// Builds a histogram directly, so each test states the distribution it means to
// reduce rather than reaching it through the sampling gates.
struct Bins {
  uint32_t count[WPM_BIN_COUNT] = {};
  uint32_t words[WPM_BIN_COUNT] = {};
  uint32_t seconds[WPM_BIN_COUNT] = {};

  // Adds n pages of `pageWords` words each, read at `wpm`, to their natural bin.
  // The seconds are computed over all n pages at once: dividing per page first
  // would truncate each one and make the fixture, not the reducer, lossy.
  void add(const uint32_t n, const uint16_t pageWords, const uint32_t wpm) {
    const size_t bin = wpmBinIndex(wpm);
    count[bin] += n;
    words[bin] += n * pageWords;
    seconds[bin] += n * static_cast<uint32_t>(pageWords) * 60UL / wpm;
  }

  WpmReading reduce(const bool untrimmedFallback = false) const {
    return trimmedWordsPerMinute(count, words, seconds, untrimmedFallback);
  }
};

}  // namespace

TEST(WpmReducer, EmptyBinsAreNotMeasured) {
  const Bins bins;
  EXPECT_FALSE(bins.reduce().available);
  EXPECT_FALSE(bins.reduce(true).available);
}

TEST(WpmReducer, ExactRatioOverACleanDistribution) {
  Bins bins;
  bins.add(30, 70, 250);  // 30 pages, 70 words each, 16.8 s each

  const WpmReading reading = bins.reduce();
  ASSERT_TRUE(reading.available);
  // 30 pages trims 3, all from the same bin, so the ratio is preserved — to within
  // a wpm. The pro-rata slice is integer arithmetic, so it is only bit-exact when
  // the count divides the bin's words and seconds evenly.
  EXPECT_NEAR(reading.wordsPerMinute, 250, 1);
}

TEST(WpmReducer, ASinglePopulatedBinIsUnchangedByTheTrim) {
  // This is what lets a backfilled book reproduce its estimate exactly: removing a
  // pro-rata slice of one bin leaves its words:seconds ratio identical.
  Bins bins;
  bins.count[4] = 100;
  bins.words[4] = 7000;
  bins.seconds[4] = 1700;

  const WpmReading reading = bins.reduce();
  ASSERT_TRUE(reading.available);
  EXPECT_EQ(reading.wordsPerMinute, 7000u * 60u / 1700u);
}

TEST(WpmReducer, InterruptedPagesAreTrimmedOffTheSlowSide) {
  // The measured example from the plan: a handful of interrupted pages are a small
  // share of the pages but a large share of the time, so the untrimmed figure is
  // dragged well below the real pace.
  Bins bins;
  bins.add(200, 70, 360);  // real reading
  bins.add(12, 70, 25);    // interrupted: 5.6% of pages, ~27% of the time

  // Computed by hand: above MIN_WPM_SAMPLES the reducer always trims, so there is
  // no way to ask it for the untrimmed figure. That is deliberate — the untrimmed
  // average is what the whole feature exists to stop showing.
  uint64_t rawWords = 0;
  uint64_t rawSeconds = 0;
  for (size_t i = 0; i < WPM_BIN_COUNT; ++i) {
    rawWords += bins.words[i];
    rawSeconds += bins.seconds[i];
  }
  const uint64_t untrimmed = rawWords * 60 / rawSeconds;

  const WpmReading trimmed = bins.reduce();
  ASSERT_TRUE(trimmed.available);

  // 12 interrupted pages are 5.7% of the pages but consume ~46% of the time, which
  // drags the untrimmed figure far below the pace actually being read at.
  EXPECT_LT(untrimmed, 250u);
  EXPECT_GT(trimmed.wordsPerMinute, 350);
  EXPECT_GT(bins.seconds[0] * 100 / rawSeconds, 40u);
}

TEST(WpmReducer, BoundaryBinIsProRatedNotDroppedWhole) {
  // 100 pages trims 10. Bin 0 holds 12, so it must give up 10/12 of its words and
  // seconds — dropping it whole would remove 12% of the pages for a 10% trim.
  Bins bins;
  bins.count[0] = 12;
  bins.words[0] = 1200;
  bins.seconds[0] = 2400;
  bins.count[4] = 88;
  bins.words[4] = 6160;
  bins.seconds[4] = 1496;

  const WpmReading reading = bins.reduce();
  ASSERT_TRUE(reading.available);

  // Whole-bin trimming would leave exactly bin 4's ratio; pro-rata leaves the
  // remaining sixth of bin 0 in, which is slower.
  const uint16_t wholeBinRate = static_cast<uint16_t>(6160u * 60u / 1496u);
  EXPECT_LT(reading.wordsPerMinute, wholeBinRate);

  const uint64_t expectedWords = 6160 + 1200 - (1200ULL * 10 / 12);
  const uint64_t expectedSeconds = 1496 + 2400 - (2400ULL * 10 / 12);
  EXPECT_EQ(reading.wordsPerMinute, expectedWords * 60 / expectedSeconds);
}

TEST(WpmReducer, DropLandingExactlyOnABinEdgeRemovesItWhole) {
  Bins bins;
  bins.count[0] = 10;
  bins.words[0] = 1000;
  bins.seconds[0] = 2000;
  bins.count[4] = 90;
  bins.words[4] = 6300;
  bins.seconds[4] = 1530;

  const WpmReading reading = bins.reduce();
  ASSERT_TRUE(reading.available);
  EXPECT_EQ(reading.wordsPerMinute, 6300u * 60u / 1530u);
}

TEST(WpmReducer, AllMassInBinZeroStillReports) {
  Bins bins;
  bins.count[0] = 40;
  bins.words[0] = 2800;
  bins.seconds[0] = 11200;

  const WpmReading reading = bins.reduce();
  ASSERT_TRUE(reading.available);
  EXPECT_EQ(reading.wordsPerMinute, 15);
}

TEST(WpmReducer, ZeroSecondsCannotDivideByZero) {
  Bins bins;
  bins.count[4] = 50;
  bins.words[4] = 3500;
  bins.seconds[4] = 0;
  EXPECT_FALSE(bins.reduce().available);

  // And after a trim that removes every second.
  Bins allTrimmed;
  allTrimmed.count[0] = 30;
  allTrimmed.words[0] = 100;
  allTrimmed.seconds[0] = 1;
  const WpmReading reading = allTrimmed.reduce();
  if (reading.available) EXPECT_GT(reading.wordsPerMinute, 0);
}

TEST(WpmReducer, BelowTheMinimumPerBookReportsNothingAndGlobalFallsBack) {
  Bins bins;
  bins.add(24, 70, 250);  // one short of MIN_WPM_SAMPLES

  EXPECT_FALSE(bins.reduce(false).available);

  const WpmReading global = bins.reduce(true);
  ASSERT_TRUE(global.available);
  EXPECT_EQ(global.wordsPerMinute, 250);
}

TEST(WpmReducer, AtTheMinimumTheTrimEngages) {
  Bins bins;
  bins.add(24, 70, 360);
  bins.add(1, 70, 25);  // one interrupted page in a 25-page session

  const WpmReading reading = bins.reduce(false);
  ASSERT_TRUE(reading.available);
  // The single slow page is 4% of the pages but dominates a 10% trim budget, so
  // the reported figure sits close to the real pace rather than the average.
  EXPECT_GT(reading.wordsPerMinute, 300);
}

// --------------------------------------------------------- card sentinels ---

TEST(WpmCard, PerBookNeedsBothAMinuteOfReadingAndEnoughSamples) {
  BookReadingStats stats;
  stats.totalReadingSeconds = 30;
  stats.wpmBinCount[4] = 100;
  stats.wpmBinWords[4] = 7000;
  stats.wpmBinSeconds[4] = 1700;
  EXPECT_FALSE(bookWordsPerMinute(stats).available);

  stats.totalReadingSeconds = 1700;
  EXPECT_TRUE(bookWordsPerMinute(stats).available);

  // Enough time, too few samples: still nothing, never an untrimmed average.
  BookReadingStats sparse;
  sparse.totalReadingSeconds = 600;
  sparse.wpmBinCount[4] = 10;
  sparse.wpmBinWords[4] = 700;
  sparse.wpmBinSeconds[4] = 170;
  EXPECT_FALSE(bookWordsPerMinute(sparse).available);
}

TEST(WpmCard, GlobalFallsBackToUntrimmedBelowTheMinimum) {
  GlobalReadingStats stats;
  stats.totalReadingSeconds = 600;
  stats.wpmBinCount[4] = 10;
  stats.wpmBinWords[4] = 700;
  stats.wpmBinSeconds[4] = 170;

  const WpmReading reading = globalWordsPerMinute(stats);
  ASSERT_TRUE(reading.available);
  EXPECT_EQ(reading.wordsPerMinute, 700u * 60u / 170u);
  EXPECT_FALSE(reading.estimated);
}

TEST(WpmCard, BackfilledSeedIsMarkedEstimatedUntilRealSamplesArrive) {
  BookReadingStats stats;
  stats.totalReadingSeconds = 36000;
  stats.wordsBackfilled = true;
  // The script seeds exactly MIN_WPM_SAMPLES votes into one bin.
  stats.wpmBinCount[4] = MIN_WPM_SAMPLES;
  stats.wpmBinWords[4] = 90000;
  stats.wpmBinSeconds[4] = 21600;

  const WpmReading seeded = bookWordsPerMinute(stats);
  ASSERT_TRUE(seeded.available);
  EXPECT_TRUE(seeded.estimated);
  // A single populated bin is unchanged by the trim, so the estimate is reproduced
  // exactly rather than approximately.
  EXPECT_EQ(seeded.wordsPerMinute, 90000u * 60u / 21600u);

  // One measured session later, the marker goes away.
  stats.wpmBinCount[5] = 30;
  stats.wpmBinWords[5] = 2100;
  stats.wpmBinSeconds[5] = 380;
  EXPECT_FALSE(bookWordsPerMinute(stats).estimated);
}

TEST(WpmCard, MeasuredBooksAreNeverMarkedEstimated) {
  BookReadingStats stats;
  stats.totalReadingSeconds = 1700;
  stats.wpmBinCount[4] = 100;
  stats.wpmBinWords[4] = 7000;
  stats.wpmBinSeconds[4] = 1700;

  const WpmReading reading = bookWordsPerMinute(stats);
  ASSERT_TRUE(reading.available);
  EXPECT_FALSE(reading.estimated);
}
