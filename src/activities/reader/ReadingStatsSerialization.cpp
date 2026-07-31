#include "ReadingStatsSerialization.h"

#include <algorithm>
#include <cstring>

namespace {
// Per-book binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Per-book binary layout v2 (12 bytes):
//   [0-10]  v1 fields
//   [11]    isCompleted         uint8_t
//
// Per-book binary layout v3 (16 bytes):
//   [0-11]   v2 fields
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Per-book binary layout v4 (69 bytes):
//   [0-15]   v3 fields
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Per-book binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
constexpr int STATS_FILE_SIZE_V1 = 11;
constexpr int STATS_FILE_SIZE_V2 = 12;
constexpr int STATS_FILE_SIZE_V3 = 16;
constexpr int STATS_FILE_SIZE_V4 = 69;
constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;

// Global binary layout v1 (13 bytes):
//   [0]     version (= 1)
//   [1-4]   totalSessions       uint32_t LE
//   [5-8]   totalReadingSeconds uint32_t LE
//   [9-12]  totalPagesTurned    uint32_t LE
//
// Global binary layout v2 (17 bytes):
//   [0-12]   v1 fields
//   [13-16]  completedBooks      uint32_t LE
//
// Global binary layout v3 (159 bytes):
//   [0-16]    v2 fields
//   [17-32]   timeOfDaySeconds[4]       uint32_t LE each
//   [33-60]   dayOfWeekSeconds[7]       uint32_t LE each
//   [61-64]   readingHistoryAnchorDay   uint32_t LE
//   [65-156]  readingHistoryBits[92]    uint8_t
//   [157-158] longestReadingStreak      uint16_t LE
constexpr uint8_t GLOBAL_STATS_VERSION_V1 = 1;
constexpr uint8_t GLOBAL_STATS_VERSION_V2 = 2;
constexpr int GLOBAL_STATS_FILE_SIZE_V1 = 13;
constexpr int GLOBAL_STATS_FILE_SIZE_V2 = 17;

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

void loadGlobalCommonFields(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}
}  // namespace

bool parseStatsBuffer(const uint8_t* data, const int n, BookReadingStats& out) {
  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, out);
    return true;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, out);
    out.isCompleted = data[11] != 0;
    return true;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, out);
    out.isCompleted = data[11] != 0;
    out.avgSecondsPerForwardPage = readLe16(data, 12);
    out.paceSampleCount = readLe16(data, 14);
    return true;
  }

  if (n != BookReadingStats::CURRENT_FILE_SIZE && n != STATS_FILE_SIZE_V4) {
    return false;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] != STATS_FILE_VERSION_V4) {
    return false;
  }
  if (n == BookReadingStats::CURRENT_FILE_SIZE && data[0] != BookReadingStats::CURRENT_FILE_VERSION) {
    return false;
  }

  readCommonStats(data, out);
  out.isCompleted = data[11] != 0;
  out.avgSecondsPerForwardPage = readLe16(data, 12);
  out.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  out.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  out.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  out.startDate = readDate(data, 17);
  out.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < out.timeOfDaySeconds.size(); ++i) {
    out.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < out.dayOfWeekSeconds.size(); ++i) {
    out.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (n == BookReadingStats::CURRENT_FILE_SIZE) {
    out.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  return true;
}

void serializeStatsBuffer(const BookReadingStats& stats, uint8_t* data) {
  memset(data, 0, BookReadingStats::CURRENT_FILE_SIZE);
  data[0] = BookReadingStats::CURRENT_FILE_VERSION;
  writeLe16(data, 1, stats.sessionCount);
  writeLe32(data, 3, stats.totalReadingSeconds);
  writeLe32(data, 7, stats.totalPagesTurned);
  data[11] = stats.isCompleted ? 1 : 0;
  writeLe16(data, 12, stats.avgSecondsPerForwardPage);
  writeLe16(data, 14, stats.paceSampleCount);
  data[16] = (stats.startDateManual ? FLAG_START_DATE_MANUAL : 0u) |
             (stats.finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, stats.startDate.isValid() ? stats.startDate.year : 0);
  data[19] = stats.startDate.isValid() ? stats.startDate.month : 0;
  data[20] = stats.startDate.isValid() ? stats.startDate.day : 0;
  writeLe16(data, 21, stats.finishedDate.isValid() ? stats.finishedDate.year : 0);
  data[23] = stats.finishedDate.isValid() ? stats.finishedDate.month : 0;
  data[24] = stats.finishedDate.isValid() ? stats.finishedDate.day : 0;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, stats.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, stats.dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, stats.estimatedTimeLeftSeconds);
}

StatsLoadOutcome parseGlobalStatsBuffer(const uint8_t* data, const int n, const size_t fileSize,
                                        GlobalReadingStats& out) {
  StatsLoadOutcome outcome;
  outcome.fileSize = fileSize;

  const size_t bytesExpected = std::min(fileSize, GlobalReadingStats::CURRENT_FILE_SIZE);
  if (n <= 0 || static_cast<size_t>(n) != bytesExpected) return outcome;
  outcome.version = data[0];

  if (fileSize > GlobalReadingStats::CURRENT_FILE_SIZE || outcome.version > GlobalReadingStats::CURRENT_FILE_VERSION) {
    outcome.result = StatsLoadResult::NewerFormat;
    return outcome;
  }

  if (n == GLOBAL_STATS_FILE_SIZE_V1 && data[0] == GLOBAL_STATS_VERSION_V1) {
    loadGlobalCommonFields(data, out);
    out.completedBooks = 0;
    outcome.result = StatsLoadResult::Ok;
    return outcome;
  }

  if (n == GLOBAL_STATS_FILE_SIZE_V2 && data[0] == GLOBAL_STATS_VERSION_V2) {
    loadGlobalCommonFields(data, out);
    out.completedBooks = readLe32(data, 13);
    outcome.result = StatsLoadResult::Ok;
    return outcome;
  }

  if (static_cast<size_t>(n) != GlobalReadingStats::CURRENT_FILE_SIZE ||
      data[0] != GlobalReadingStats::CURRENT_FILE_VERSION) {
    return outcome;
  }
  loadGlobalCommonFields(data, out);
  out.completedBooks = readLe32(data, 13);
  for (size_t i = 0; i < out.timeOfDaySeconds.size(); ++i) {
    out.timeOfDaySeconds[i] = readLe32(data, 17 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < out.dayOfWeekSeconds.size(); ++i) {
    out.dayOfWeekSeconds[i] = readLe32(data, 33 + static_cast<int>(i) * 4);
  }
  out.readingHistoryAnchorDay = readLe32(data, 61);
  memcpy(out.readingHistoryBits.data(), data + 65, out.readingHistoryBits.size());
  out.longestReadingStreak = readLe16(data, 157);
  outcome.result = StatsLoadResult::Ok;
  return outcome;
}

void serializeGlobalStatsBuffer(const GlobalReadingStats& stats, uint8_t* data) {
  data[0] = GlobalReadingStats::CURRENT_FILE_VERSION;
  writeLe32(data, 1, stats.totalSessions);
  writeLe32(data, 5, stats.totalReadingSeconds);
  writeLe32(data, 9, stats.totalPagesTurned);
  writeLe32(data, 13, stats.completedBooks);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 17 + static_cast<int>(i) * 4, stats.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 33 + static_cast<int>(i) * 4, stats.dayOfWeekSeconds[i]);
  }
  writeLe32(data, 61, stats.readingHistoryAnchorDay);
  memcpy(data + 65, stats.readingHistoryBits.data(), stats.readingHistoryBits.size());
  writeLe16(data, 157, stats.longestReadingStreak);
}
