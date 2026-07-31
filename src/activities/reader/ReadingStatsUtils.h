#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

constexpr size_t READING_TIME_BUCKET_COUNT = 4;
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;
constexpr size_t READING_HISTORY_DAYS = 730;
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;

// Words/Min histogram. Both stats files carry the same 8 bins of implied reading
// rate, each holding an exact word and second sum, so the reported figure is a
// ratio of real accumulated totals and has no quantization error. Bin geometry
// decides only where the display-time trim boundary falls, which is why the
// bins below 200 wpm are narrow: that is where interruptions land.
//
// Bin i covers [WPM_BIN_UPPER[i - 1], WPM_BIN_UPPER[i]), bin 0 starts at 0, and
// the top bin is open-ended so genuinely fast pages are kept rather than
// discarded.
constexpr size_t WPM_BIN_COUNT = 8;
constexpr uint16_t WPM_BIN_UPPER[WPM_BIN_COUNT] = {50, 100, 150, 200, 300, 400, 550, UINT16_MAX};

// Version of the shared word-token rule that produced the histogram. Bumped
// whenever tokenization changes, so synced stats collected under different rules
// are never ranked against each other.
constexpr uint8_t TOKEN_RULE_VERSION = 1;

// Saturating add for the cumulative counters and bins. Templated because per-book
// bin counts are uint16_t and global ones are uint32_t; everything else is
// uint32_t.
template <typename T>
T addSaturated(const T a, const T b) {
  const T max = std::numeric_limits<T>::max();
  return max - a < b ? max : static_cast<T>(a + b);
}

enum class ReadingTimeBucket : uint8_t { Morning = 0, Afternoon, Evening, Night };

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool isValid() const;
  void clear();
};

struct ReadingStatsDateTime {
  ReadingStatsDate date;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  bool isValid() const;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValidReadingStatsDate(const ReadingStatsDate& date);
int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs);
void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta);
void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds);
uint32_t readingStatsDayIndex(const ReadingStatsDate& date);
bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate);
uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date);  // Monday = 0
ReadingTimeBucket readingTimeBucketForHour(uint8_t hour);
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime);
uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end);
uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end);
void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, size_t len);
void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, size_t len);

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits, uint32_t dayIndex);
void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits);
uint16_t computeReadingHistoryLongestStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits);
uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today);
