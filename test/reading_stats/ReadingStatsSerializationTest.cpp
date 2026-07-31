#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ReadingStatsSerialization.h"

// The byte offsets below are written out literally rather than reused from the
// parser: these tests exist to pin the on-disk format, so they have to state it
// independently of the code under test.
namespace {

void putLe16(std::vector<uint8_t>& buf, const size_t offset, const uint16_t value) {
  buf[offset] = static_cast<uint8_t>(value & 0xFF);
  buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void putLe32(std::vector<uint8_t>& buf, const size_t offset, const uint32_t value) {
  buf[offset] = static_cast<uint8_t>(value & 0xFF);
  buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buf[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buf[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// A v1 per-book file: version, sessionCount, totalReadingSeconds, totalPagesTurned.
std::vector<uint8_t> bookV1() {
  std::vector<uint8_t> buf(11, 0);
  buf[0] = 1;
  putLe16(buf, 1, 7);
  putLe32(buf, 3, 4321);
  putLe32(buf, 7, 555);
  return buf;
}

std::vector<uint8_t> bookV2() {
  std::vector<uint8_t> buf = bookV1();
  buf.resize(12, 0);
  buf[0] = 2;
  buf[11] = 1;  // isCompleted
  return buf;
}

std::vector<uint8_t> bookV3() {
  std::vector<uint8_t> buf = bookV2();
  buf.resize(16, 0);
  buf[0] = 3;
  putLe16(buf, 12, 42);  // avgSecondsPerForwardPage
  putLe16(buf, 14, 19);  // paceSampleCount
  return buf;
}

std::vector<uint8_t> bookV4() {
  std::vector<uint8_t> buf = bookV3();
  buf.resize(69, 0);
  buf[0] = 4;
  buf[16] = 0x03;  // startDateManual | finishedDateManual
  putLe16(buf, 17, 2025);
  buf[19] = 3;
  buf[20] = 14;
  putLe16(buf, 21, 2026);
  buf[23] = 1;
  buf[24] = 2;
  for (int i = 0; i < 4; ++i) putLe32(buf, 25 + i * 4, 100u + static_cast<uint32_t>(i));
  for (int i = 0; i < 7; ++i) putLe32(buf, 41 + i * 4, 200u + static_cast<uint32_t>(i));
  return buf;
}

std::vector<uint8_t> bookV5() {
  std::vector<uint8_t> buf = bookV4();
  buf.resize(73, 0);
  buf[0] = 5;
  putLe32(buf, 69, 9876);  // estimatedTimeLeftSeconds
  return buf;
}

std::vector<uint8_t> bookV6() {
  std::vector<uint8_t> buf = bookV5();
  buf.resize(165, 0);
  buf[0] = 6;
  putLe16(buf, 12, 0);  // avgSecondsPerForwardPage: reserved from v6, written 0
  putLe16(buf, 14, 0);  // paceSampleCount: reserved from v6, written 0
  buf[16] = 0x07;       // startDateManual | finishedDateManual | wordsBackfilled
  for (int i = 0; i < 8; ++i) {
    putLe16(buf, 73 + static_cast<size_t>(i) * 2, static_cast<uint16_t>(300 + i));
    putLe32(buf, 89 + static_cast<size_t>(i) * 4, 4000u + static_cast<uint32_t>(i));
    putLe32(buf, 121 + static_cast<size_t>(i) * 4, 5000u + static_cast<uint32_t>(i));
  }
  putLe32(buf, 153, 77);  // statsRevision
  return buf;
}

void expectV4Tail(const BookReadingStats& stats) {
  EXPECT_TRUE(stats.startDateManual);
  EXPECT_TRUE(stats.finishedDateManual);
  EXPECT_EQ(stats.startDate.year, 2025);
  EXPECT_EQ(stats.startDate.month, 3);
  EXPECT_EQ(stats.startDate.day, 14);
  EXPECT_EQ(stats.finishedDate.year, 2026);
  EXPECT_EQ(stats.finishedDate.month, 1);
  EXPECT_EQ(stats.finishedDate.day, 2);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    EXPECT_EQ(stats.timeOfDaySeconds[i], 100u + i);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    EXPECT_EQ(stats.dayOfWeekSeconds[i], 200u + i);
  }
}

// A v1 global file: version, totalSessions, totalReadingSeconds, totalPagesTurned.
std::vector<uint8_t> globalV1() {
  std::vector<uint8_t> buf(13, 0);
  buf[0] = 1;
  putLe32(buf, 1, 12);
  putLe32(buf, 5, 34567);
  putLe32(buf, 9, 8910);
  return buf;
}

std::vector<uint8_t> globalV2() {
  std::vector<uint8_t> buf = globalV1();
  buf.resize(17, 0);
  buf[0] = 2;
  putLe32(buf, 13, 4);  // completedBooks
  return buf;
}

std::vector<uint8_t> globalV3() {
  std::vector<uint8_t> buf = globalV2();
  buf.resize(159, 0);
  buf[0] = 3;
  for (int i = 0; i < 4; ++i) putLe32(buf, 17 + i * 4, 1000u + static_cast<uint32_t>(i));
  for (int i = 0; i < 7; ++i) putLe32(buf, 33 + i * 4, 2000u + static_cast<uint32_t>(i));
  putLe32(buf, 61, 20000);  // readingHistoryAnchorDay
  for (int i = 0; i < 92; ++i) buf[65 + static_cast<size_t>(i)] = static_cast<uint8_t>(i);
  putLe16(buf, 157, 37);  // longestReadingStreak
  return buf;
}

std::vector<uint8_t> globalV4() {
  std::vector<uint8_t> buf = globalV3();
  buf.resize(270, 0);
  buf[0] = 4;
  for (int i = 0; i < 8; ++i) {
    putLe32(buf, 159 + static_cast<size_t>(i) * 4, 600u + static_cast<uint32_t>(i));
    putLe32(buf, 191 + static_cast<size_t>(i) * 4, 700u + static_cast<uint32_t>(i));
    putLe32(buf, 223 + static_cast<size_t>(i) * 4, 800u + static_cast<uint32_t>(i));
  }
  putLe32(buf, 255, 4096);  // statsRevision
  buf[259] = 1;             // tokenRuleVersion
  putLe16(buf, 260, 120);   // idleThresholdSeconds
  return buf;
}

// Mirrors the firmware's two-pass read: the header block first, then the tail
// block only when the file carries one.
StatsLoadOutcome parseGlobal(const std::vector<uint8_t>& buf, GlobalReadingStats& out) {
  const int headerBytes = static_cast<int>(std::min<size_t>(buf.size(), GlobalReadingStats::HEADER_BLOCK_SIZE));
  const StatsLoadOutcome outcome = parseGlobalStatsHeader(buf.data(), headerBytes, buf.size(), out);
  if (globalStatsHasTail(outcome)) {
    parseGlobalStatsTail(buf.data() + GlobalReadingStats::HEADER_BLOCK_SIZE, out);
  }
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------- per-book ---

TEST(BookStatsParse, V1RoundTripsCommonFields) {
  const std::vector<uint8_t> buf = bookV1();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  EXPECT_EQ(stats.totalReadingSeconds, 4321u);
  EXPECT_EQ(stats.totalPagesTurned, 555u);
  EXPECT_FALSE(stats.isCompleted);
  EXPECT_EQ(stats.estimatedTimeLeftSeconds, 0u);
}

TEST(BookStatsParse, V2AddsIsCompleted) {
  const std::vector<uint8_t> buf = bookV2();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  EXPECT_EQ(stats.totalReadingSeconds, 4321u);
  EXPECT_TRUE(stats.isCompleted);
}

// v3 added the pace fields at [12-15]. They are reserved from v6 on and no longer
// parsed into the struct at all, so this only pins that a v3 file is still
// accepted and its live fields still land.
TEST(BookStatsParse, V3IsStillAccepted) {
  const std::vector<uint8_t> buf = bookV3();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  EXPECT_TRUE(stats.isCompleted);
  EXPECT_EQ(stats.startDate.year, 0);
}

TEST(BookStatsParse, V4AddsDatesAndBuckets) {
  const std::vector<uint8_t> buf = bookV4();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  expectV4Tail(stats);
  EXPECT_EQ(stats.estimatedTimeLeftSeconds, 0u);
}

TEST(BookStatsParse, V5AddsEstimatedTimeLeft) {
  const std::vector<uint8_t> buf = bookV5();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  EXPECT_EQ(stats.totalReadingSeconds, 4321u);
  EXPECT_EQ(stats.totalPagesTurned, 555u);
  EXPECT_TRUE(stats.isCompleted);
  expectV4Tail(stats);
  EXPECT_EQ(stats.estimatedTimeLeftSeconds, 9876u);
}

TEST(BookStatsParse, InvalidDatesAreCleared) {
  std::vector<uint8_t> buf = bookV5();
  putLe16(buf, 17, 2025);
  buf[19] = 2;
  buf[20] = 30;  // Feb 30 does not exist
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.startDate.year, 0);
  EXPECT_EQ(stats.startDate.month, 0);
  EXPECT_EQ(stats.startDate.day, 0);
}

TEST(BookStatsParse, RejectsVersionSizeMismatch) {
  BookReadingStats stats;

  std::vector<uint8_t> wrongVersion = bookV5();
  wrongVersion[0] = 4;  // v4 version byte in a v5-sized file
  EXPECT_FALSE(parseStatsBuffer(wrongVersion.data(), static_cast<int>(wrongVersion.size()), stats));

  std::vector<uint8_t> v4WrongVersion = bookV4();
  v4WrongVersion[0] = 5;
  EXPECT_FALSE(parseStatsBuffer(v4WrongVersion.data(), static_cast<int>(v4WrongVersion.size()), stats));

  std::vector<uint8_t> v1WrongVersion = bookV1();
  v1WrongVersion[0] = 2;
  EXPECT_FALSE(parseStatsBuffer(v1WrongVersion.data(), static_cast<int>(v1WrongVersion.size()), stats));

  std::vector<uint8_t> v6WrongVersion = bookV6();
  v6WrongVersion[0] = 5;  // v5 version byte in a v6-sized file
  EXPECT_FALSE(parseStatsBuffer(v6WrongVersion.data(), static_cast<int>(v6WrongVersion.size()), stats));
}

TEST(BookStatsParse, RejectsTruncatedAndUnknownSizes) {
  const std::vector<uint8_t> buf = bookV6();
  BookReadingStats stats;

  EXPECT_FALSE(parseStatsBuffer(buf.data(), 0, stats));
  EXPECT_FALSE(parseStatsBuffer(buf.data(), 40, stats));
  EXPECT_FALSE(parseStatsBuffer(buf.data(), 72, stats));
  EXPECT_FALSE(parseStatsBuffer(buf.data(), 73, stats));   // v5 size, v6 version byte
  EXPECT_FALSE(parseStatsBuffer(buf.data(), 164, stats));  // one byte short of v6
}

TEST(BookStatsParse, SerializeRoundTrips) {
  BookReadingStats stats;
  stats.sessionCount = 321;
  stats.totalReadingSeconds = 987654;
  stats.totalPagesTurned = 4096;
  stats.isCompleted = true;
  stats.estimatedTimeLeftSeconds = 3600;
  stats.statsRevision = 1234;
  stats.startDateManual = true;
  stats.finishedDateManual = false;
  stats.wordsBackfilled = true;
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    stats.wpmBinCount[i] = static_cast<uint16_t>(3u * (i + 1));
    stats.wpmBinWords[i] = 5000u * (i + 1);
    stats.wpmBinSeconds[i] = 1200u * (i + 1);
  }
  stats.startDate = {2024, 12, 31};
  stats.finishedDate = {2025, 6, 1};
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) stats.timeOfDaySeconds[i] = 11u * (i + 1);
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) stats.dayOfWeekSeconds[i] = 13u * (i + 1);

  uint8_t data[BookReadingStats::CURRENT_FILE_SIZE];
  memset(data, 0xAA, sizeof(data));
  serializeStatsBuffer(stats, data);
  EXPECT_EQ(data[0], BookReadingStats::CURRENT_FILE_VERSION);

  BookReadingStats parsed;
  ASSERT_TRUE(parseStatsBuffer(data, BookReadingStats::CURRENT_FILE_SIZE, parsed));

  EXPECT_EQ(parsed.sessionCount, stats.sessionCount);
  EXPECT_EQ(parsed.totalReadingSeconds, stats.totalReadingSeconds);
  EXPECT_EQ(parsed.totalPagesTurned, stats.totalPagesTurned);
  EXPECT_EQ(parsed.isCompleted, stats.isCompleted);
  // [12-15] are reserved in v6 and must be written zero.
  EXPECT_EQ(data[12], 0);
  EXPECT_EQ(data[13], 0);
  EXPECT_EQ(data[14], 0);
  EXPECT_EQ(data[15], 0);
  EXPECT_EQ(parsed.estimatedTimeLeftSeconds, stats.estimatedTimeLeftSeconds);
  EXPECT_EQ(parsed.statsRevision, stats.statsRevision);
  EXPECT_EQ(parsed.startDateManual, stats.startDateManual);
  EXPECT_EQ(parsed.finishedDateManual, stats.finishedDateManual);
  EXPECT_TRUE(parsed.wordsBackfilled);
  EXPECT_EQ(parsed.wpmBinCount, stats.wpmBinCount);
  EXPECT_EQ(parsed.wpmBinWords, stats.wpmBinWords);
  EXPECT_EQ(parsed.wpmBinSeconds, stats.wpmBinSeconds);
  EXPECT_EQ(parsed.startDate.year, stats.startDate.year);
  EXPECT_EQ(parsed.startDate.month, stats.startDate.month);
  EXPECT_EQ(parsed.startDate.day, stats.startDate.day);
  EXPECT_EQ(parsed.finishedDate.year, stats.finishedDate.year);
  EXPECT_EQ(parsed.finishedDate.month, stats.finishedDate.month);
  EXPECT_EQ(parsed.finishedDate.day, stats.finishedDate.day);
  EXPECT_EQ(parsed.timeOfDaySeconds, stats.timeOfDaySeconds);
  EXPECT_EQ(parsed.dayOfWeekSeconds, stats.dayOfWeekSeconds);
}

TEST(BookStatsParse, SerializeClearsInvalidDates) {
  BookReadingStats stats;
  stats.startDate = {2025, 2, 30};  // invalid
  uint8_t data[BookReadingStats::CURRENT_FILE_SIZE];
  serializeStatsBuffer(stats, data);

  BookReadingStats parsed;
  ASSERT_TRUE(parseStatsBuffer(data, BookReadingStats::CURRENT_FILE_SIZE, parsed));
  EXPECT_EQ(parsed.startDate.year, 0);
  EXPECT_EQ(parsed.startDate.month, 0);
  EXPECT_EQ(parsed.startDate.day, 0);
}

// ------------------------------------------------------------------ global ---

TEST(GlobalStatsParse, V1RoundTripsCommonFields) {
  GlobalReadingStats stats;
  const StatsLoadOutcome outcome = parseGlobal(globalV1(), stats);

  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  EXPECT_EQ(outcome.version, 1);
  EXPECT_EQ(outcome.fileSize, 13u);
  EXPECT_EQ(stats.totalSessions, 12u);
  EXPECT_EQ(stats.totalReadingSeconds, 34567u);
  EXPECT_EQ(stats.totalPagesTurned, 8910u);
  EXPECT_EQ(stats.completedBooks, 0u);
}

TEST(GlobalStatsParse, V2AddsCompletedBooks) {
  GlobalReadingStats stats;
  const StatsLoadOutcome outcome = parseGlobal(globalV2(), stats);

  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  EXPECT_EQ(stats.totalSessions, 12u);
  EXPECT_EQ(stats.completedBooks, 4u);
  EXPECT_EQ(stats.longestReadingStreak, 0);
}

TEST(GlobalStatsParse, V3AddsBucketsHistoryAndStreak) {
  GlobalReadingStats stats;
  const StatsLoadOutcome outcome = parseGlobal(globalV3(), stats);

  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  EXPECT_EQ(outcome.version, 3);
  EXPECT_EQ(stats.totalSessions, 12u);
  EXPECT_EQ(stats.totalReadingSeconds, 34567u);
  EXPECT_EQ(stats.totalPagesTurned, 8910u);
  EXPECT_EQ(stats.completedBooks, 4u);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    EXPECT_EQ(stats.timeOfDaySeconds[i], 1000u + i);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    EXPECT_EQ(stats.dayOfWeekSeconds[i], 2000u + i);
  }
  EXPECT_EQ(stats.readingHistoryAnchorDay, 20000u);
  for (size_t i = 0; i < stats.readingHistoryBits.size(); ++i) {
    EXPECT_EQ(stats.readingHistoryBits[i], static_cast<uint8_t>(i));
  }
  EXPECT_EQ(stats.longestReadingStreak, 37);
}

TEST(GlobalStatsParse, RejectsVersionSizeMismatch) {
  GlobalReadingStats stats;

  std::vector<uint8_t> wrongVersion = globalV3();
  wrongVersion[0] = 2;
  EXPECT_EQ(parseGlobal(wrongVersion, stats).result, StatsLoadResult::Invalid);

  std::vector<uint8_t> v1WrongVersion = globalV1();
  v1WrongVersion[0] = 3;
  EXPECT_EQ(parseGlobal(v1WrongVersion, stats).result, StatsLoadResult::Invalid);
}

TEST(GlobalStatsParse, RejectsUnknownSize) {
  std::vector<uint8_t> buf = globalV3();
  buf.resize(100);
  buf[0] = 3;
  GlobalReadingStats stats;
  EXPECT_EQ(parseGlobal(buf, stats).result, StatsLoadResult::Invalid);
}

TEST(GlobalStatsParse, ShortReadIsInvalid) {
  const std::vector<uint8_t> buf = globalV3();
  GlobalReadingStats stats;

  // fileSize says 159 bytes but only 100 were read.
  const StatsLoadOutcome outcome = parseGlobalStatsHeader(buf.data(), 100, buf.size(), stats);
  EXPECT_EQ(outcome.result, StatsLoadResult::Invalid);
  EXPECT_EQ(outcome.version, 0);
  EXPECT_EQ(outcome.fileSize, 159u);
}

TEST(GlobalStatsParse, LargerFileIsNewerFormatNotCorrupt) {
  const std::vector<uint8_t> buf = globalV3();
  GlobalReadingStats stats;

  // A hypothetical v5 file: larger than 270 bytes, of which only the first 159
  // are read. Size alone is enough to refuse it.
  const StatsLoadOutcome outcome = parseGlobalStatsHeader(buf.data(), static_cast<int>(buf.size()), 400, stats);
  EXPECT_EQ(outcome.result, StatsLoadResult::NewerFormat);
  EXPECT_EQ(outcome.fileSize, 400u);
}

TEST(GlobalStatsParse, HigherVersionByteIsNewerFormat) {
  std::vector<uint8_t> buf = globalV3();
  buf[0] = 5;
  GlobalReadingStats stats;

  const StatsLoadOutcome outcome = parseGlobal(buf, stats);
  EXPECT_EQ(outcome.result, StatsLoadResult::NewerFormat);
  EXPECT_EQ(outcome.version, 5);
}

TEST(GlobalStatsParse, SerializeRoundTrips) {
  GlobalReadingStats stats;
  stats.totalSessions = 4242;
  stats.totalReadingSeconds = 1234567;
  stats.totalPagesTurned = 98765;
  stats.completedBooks = 11;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) stats.timeOfDaySeconds[i] = 7u * (i + 1);
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) stats.dayOfWeekSeconds[i] = 9u * (i + 1);
  stats.readingHistoryAnchorDay = 19000;
  for (size_t i = 0; i < stats.readingHistoryBits.size(); ++i) {
    stats.readingHistoryBits[i] = static_cast<uint8_t>(0xF0u ^ i);
  }
  stats.longestReadingStreak = 128;
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    stats.wpmBinCount[i] = 40u * (i + 1);
    stats.wpmBinWords[i] = 60000u * (i + 1);
    stats.wpmBinSeconds[i] = 15000u * (i + 1);
  }
  stats.statsRevision = 90210;
  stats.tokenRuleVersion = 1;
  stats.idleThresholdSeconds = 120;

  uint8_t data[GlobalReadingStats::CURRENT_FILE_SIZE];
  memset(data, 0xAA, sizeof(data));
  serializeGlobalStatsHeader(stats, data);
  serializeGlobalStatsTail(stats, data + GlobalReadingStats::HEADER_BLOCK_SIZE);
  EXPECT_EQ(data[0], GlobalReadingStats::CURRENT_FILE_VERSION);

  GlobalReadingStats parsed;
  const StatsLoadOutcome outcome = parseGlobalStatsHeader(data, static_cast<int>(GlobalReadingStats::HEADER_BLOCK_SIZE),
                                                          GlobalReadingStats::CURRENT_FILE_SIZE, parsed);
  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  ASSERT_TRUE(globalStatsHasTail(outcome));
  parseGlobalStatsTail(data + GlobalReadingStats::HEADER_BLOCK_SIZE, parsed);
  EXPECT_EQ(parsed.totalSessions, stats.totalSessions);
  EXPECT_EQ(parsed.totalReadingSeconds, stats.totalReadingSeconds);
  EXPECT_EQ(parsed.totalPagesTurned, stats.totalPagesTurned);
  EXPECT_EQ(parsed.completedBooks, stats.completedBooks);
  EXPECT_EQ(parsed.timeOfDaySeconds, stats.timeOfDaySeconds);
  EXPECT_EQ(parsed.dayOfWeekSeconds, stats.dayOfWeekSeconds);
  EXPECT_EQ(parsed.readingHistoryAnchorDay, stats.readingHistoryAnchorDay);
  EXPECT_EQ(parsed.readingHistoryBits, stats.readingHistoryBits);
  EXPECT_EQ(parsed.longestReadingStreak, stats.longestReadingStreak);
  EXPECT_EQ(parsed.wpmBinCount, stats.wpmBinCount);
  EXPECT_EQ(parsed.wpmBinWords, stats.wpmBinWords);
  EXPECT_EQ(parsed.wpmBinSeconds, stats.wpmBinSeconds);
  EXPECT_EQ(parsed.statsRevision, stats.statsRevision);
  EXPECT_EQ(parsed.tokenRuleVersion, stats.tokenRuleVersion);
  EXPECT_EQ(parsed.idleThresholdSeconds, stats.idleThresholdSeconds);
}

// --------------------------------------------------------------- migration ---

// The trap this whole phase exists to avoid: at v6 the loader reads 73 bytes from
// an old file, and if the (size, version) dispatch does not recognise the pair it
// starts fresh — silently resetting the book.
TEST(StatsMigration, V5FilePreservesEveryFieldIntoV6) {
  const std::vector<uint8_t> buf = bookV5();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_EQ(stats.sessionCount, 7);
  EXPECT_EQ(stats.totalReadingSeconds, 4321u);
  EXPECT_EQ(stats.totalPagesTurned, 555u);
  EXPECT_TRUE(stats.isCompleted);
  expectV4Tail(stats);
  EXPECT_EQ(stats.estimatedTimeLeftSeconds, 9876u);

  // Nothing measured yet, and no v5 file can claim to have been backfilled.
  EXPECT_FALSE(stats.wordsBackfilled);
  EXPECT_EQ(stats.statsRevision, 0u);
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    EXPECT_EQ(stats.wpmBinCount[i], 0);
    EXPECT_EQ(stats.wpmBinWords[i], 0u);
    EXPECT_EQ(stats.wpmBinSeconds[i], 0u);
  }
}

TEST(StatsMigration, V5MigratesThroughASaveWithoutLoss) {
  const std::vector<uint8_t> v5 = bookV5();
  BookReadingStats loaded;
  ASSERT_TRUE(parseStatsBuffer(v5.data(), static_cast<int>(v5.size()), loaded));

  uint8_t data[BookReadingStats::CURRENT_FILE_SIZE];
  serializeStatsBuffer(loaded, data);
  ASSERT_EQ(data[0], 6);

  BookReadingStats reloaded;
  ASSERT_TRUE(parseStatsBuffer(data, BookReadingStats::CURRENT_FILE_SIZE, reloaded));
  EXPECT_EQ(reloaded.sessionCount, 7);
  EXPECT_EQ(reloaded.totalReadingSeconds, 4321u);
  EXPECT_EQ(reloaded.totalPagesTurned, 555u);
  EXPECT_TRUE(reloaded.isCompleted);
  expectV4Tail(reloaded);
  EXPECT_EQ(reloaded.estimatedTimeLeftSeconds, 9876u);
}

TEST(StatsMigration, V6ReadsBinsRevisionAndBackfillFlag) {
  const std::vector<uint8_t> buf = bookV6();
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  expectV4Tail(stats);
  EXPECT_TRUE(stats.wordsBackfilled);
  EXPECT_EQ(stats.statsRevision, 77u);
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    EXPECT_EQ(stats.wpmBinCount[i], 300 + i);
    EXPECT_EQ(stats.wpmBinWords[i], 4000u + i);
    EXPECT_EQ(stats.wpmBinSeconds[i], 5000u + i);
  }
}

TEST(StatsMigration, BackfillFlagIsIndependentOfTheDateFlags) {
  std::vector<uint8_t> buf = bookV6();
  buf[16] = 0x04;  // wordsBackfilled only
  BookReadingStats stats;
  ASSERT_TRUE(parseStatsBuffer(buf.data(), static_cast<int>(buf.size()), stats));

  EXPECT_TRUE(stats.wordsBackfilled);
  EXPECT_FALSE(stats.startDateManual);
  EXPECT_FALSE(stats.finishedDateManual);

  uint8_t data[BookReadingStats::CURRENT_FILE_SIZE];
  serializeStatsBuffer(stats, data);
  EXPECT_EQ(data[16], 0x04);
}

TEST(StatsMigration, V3GlobalFilePreservesEveryFieldIntoV4) {
  GlobalReadingStats stats;
  const StatsLoadOutcome outcome = parseGlobal(globalV3(), stats);

  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  EXPECT_FALSE(globalStatsHasTail(outcome));
  EXPECT_EQ(stats.totalSessions, 12u);
  EXPECT_EQ(stats.totalReadingSeconds, 34567u);
  EXPECT_EQ(stats.completedBooks, 4u);
  EXPECT_EQ(stats.readingHistoryAnchorDay, 20000u);
  EXPECT_EQ(stats.longestReadingStreak, 37);

  // A v3 file has no tail, so the sync scalars stay at their defaults until the
  // first v4 save fills them in.
  EXPECT_EQ(stats.statsRevision, 0u);
  EXPECT_EQ(stats.tokenRuleVersion, 0);
  EXPECT_EQ(stats.idleThresholdSeconds, 0);
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    EXPECT_EQ(stats.wpmBinCount[i], 0u);
  }
}

TEST(StatsMigration, V4GlobalReadsBinsAndSyncScalars) {
  GlobalReadingStats stats;
  const StatsLoadOutcome outcome = parseGlobal(globalV4(), stats);

  ASSERT_EQ(outcome.result, StatsLoadResult::Ok);
  EXPECT_TRUE(globalStatsHasTail(outcome));
  EXPECT_EQ(stats.totalSessions, 12u);
  EXPECT_EQ(stats.longestReadingStreak, 37);
  for (size_t i = 0; i < stats.wpmBinCount.size(); ++i) {
    EXPECT_EQ(stats.wpmBinCount[i], 600u + i);
    EXPECT_EQ(stats.wpmBinWords[i], 700u + i);
    EXPECT_EQ(stats.wpmBinSeconds[i], 800u + i);
  }
  EXPECT_EQ(stats.statsRevision, 4096u);
  EXPECT_EQ(stats.tokenRuleVersion, 1);
  EXPECT_EQ(stats.idleThresholdSeconds, 120);
}

TEST(StatsMigration, GlobalTailOffsetsMatchTheDocumentedLayout) {
  GlobalReadingStats stats;
  stats.wpmBinCount[0] = 0x01020304;
  stats.statsRevision = 0x0A0B0C0D;
  stats.tokenRuleVersion = 0x5A;
  stats.idleThresholdSeconds = 0x1234;

  uint8_t tail[GlobalReadingStats::TAIL_BLOCK_SIZE];
  memset(tail, 0xEE, sizeof(tail));
  serializeGlobalStatsTail(stats, tail);

  // File offsets 159, 255, 259 and 260, relative to the start of the tail block.
  EXPECT_EQ(tail[0], 0x04);
  EXPECT_EQ(tail[96], 0x0D);
  EXPECT_EQ(tail[100], 0x5A);
  EXPECT_EQ(tail[101], 0x34);
  EXPECT_EQ(tail[102], 0x12);
  // The last 8 bytes are reserved and must be written zero, not left as padding.
  for (size_t i = 103; i < GlobalReadingStats::TAIL_BLOCK_SIZE; ++i) {
    EXPECT_EQ(tail[i], 0) << "reserved byte " << i << " was not cleared";
  }
}

TEST(StatsMigration, BinMergeSaturatesRatherThanWrapping) {
  // Per-book counts are uint16_t and global counts uint32_t; both have to clamp,
  // because a wrapped bin silently moves the trim boundary.
  EXPECT_EQ(addSaturated<uint16_t>(65000, 1000), 65535);
  EXPECT_EQ(addSaturated<uint16_t>(65535, 1), 65535);
  EXPECT_EQ(addSaturated<uint16_t>(100, 200), 300);
  EXPECT_EQ(addSaturated<uint32_t>(4294967000u, 1000u), 4294967295u);
  EXPECT_EQ(addSaturated<uint32_t>(4294967295u, 1u), 4294967295u);
  EXPECT_EQ(addSaturated<uint32_t>(100u, 200u), 300u);
}

TEST(StatsMigration, FileSizesMatchTheSpec) {
  EXPECT_EQ(BookReadingStats::CURRENT_FILE_VERSION, 6);
  EXPECT_EQ(BookReadingStats::CURRENT_FILE_SIZE, 165);
  EXPECT_EQ(GlobalReadingStats::CURRENT_FILE_VERSION, 4);
  EXPECT_EQ(GlobalReadingStats::CURRENT_FILE_SIZE, 270u);
  EXPECT_EQ(GlobalReadingStats::HEADER_BLOCK_SIZE, 159u);
  EXPECT_EQ(GlobalReadingStats::TAIL_BLOCK_SIZE, 111u);
  // Both write buffers must stay under the 256-byte stack-local limit.
  EXPECT_LE(BookReadingStats::CURRENT_FILE_SIZE, 256);
  EXPECT_LE(GlobalReadingStats::HEADER_BLOCK_SIZE, 256u);
}
