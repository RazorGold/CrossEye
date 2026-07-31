#include <gtest/gtest.h>

#include "WpmHistogram.h"

// A page on the X3 is ~70 words at ~17 s of dwell, so the numbers below are sized
// to that rather than to a 250-word page: the gates behave very differently on a
// small screen, which is the whole reason the dwell floor exists.
namespace {
constexpr uint16_t X3_PAGE_WORDS = 70;

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
  EXPECT_FALSE(bins.empty());
}

TEST(WpmSample, PageFlipBurstIsRejectedByTheDwellFloor) {
  WpmSessionBins bins;
  // 3 s/page on a 70-word page implies 1400 wpm, which is under MAX_PLAUSIBLE_WPM:
  // the rate ceiling alone would admit a whole flip-through.
  EXPECT_LT(static_cast<uint32_t>(X3_PAGE_WORDS) * 60000UL / 2999UL, MAX_PLAUSIBLE_WPM);
  EXPECT_FALSE(bins.addSample(2999, X3_PAGE_WORDS));
  EXPECT_FALSE(bins.addSample(500, X3_PAGE_WORDS));
  EXPECT_TRUE(bins.empty());

  // Exactly at the floor is admitted.
  EXPECT_TRUE(bins.addSample(MIN_SAMPLE_DWELL_MS, X3_PAGE_WORDS));
}

TEST(WpmSample, RateCeilingOnlyAppliesToPagesWithEnoughWords) {
  WpmSessionBins bins;
  // A dense page read absurdly fast: rejected.
  EXPECT_FALSE(bins.addSample(3000, 200));  // 4000 wpm
  EXPECT_TRUE(bins.empty());

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
  EXPECT_TRUE(bins.empty());
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
  EXPECT_TRUE(bins.empty());
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
