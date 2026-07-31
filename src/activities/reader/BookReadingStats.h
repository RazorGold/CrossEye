#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Per-book reading statistics, persisted to cachePath/stats_v6.bin.
struct BookReadingStats {
  uint16_t sessionCount = 0;              // Total times this book was opened
  uint32_t totalReadingSeconds = 0;       // Accumulated reading time in seconds
  uint32_t totalPagesTurned = 0;          // Total forward page turns after the dwell threshold
  bool isCompleted = false;               // Whether the user manually marked this book as finished
  uint16_t avgSecondsPerForwardPage = 0;  // Reserved: no longer maintained, written 0
  uint16_t paceSampleCount = 0;           // Reserved: no longer maintained, written 0
  uint32_t estimatedTimeLeftSeconds = 0;  // Last live reader book time-left estimate; 0 means unavailable
  bool startDateManual = false;           // Permanent user override for the reading start date
  bool finishedDateManual = false;        // Permanent user override for the finished date
  bool wordsBackfilled = false;           // Word counts came from the host backfill script, not measurement
  ReadingStatsDate startDate;             // First qualifying reading date (or manual override)
  ReadingStatsDate finishedDate;          // Manual or auto-finished date on X3
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};

  // Words/Min histogram: one page turn is one vote in wpmBinCount, and the page's
  // words and seconds are added to the matching bin. Counts locate the display-time
  // trim boundary; the sums produce the number. See ReadingStatsUtils.h for the
  // bin edges. uint16_t counts here because 65535 pages in one bin is ~312 hours
  // on a single book; global uses uint32_t.
  std::array<uint16_t, WPM_BIN_COUNT> wpmBinCount{};
  std::array<uint32_t, WPM_BIN_COUNT> wpmBinWords{};
  std::array<uint32_t, WPM_BIN_COUNT> wpmBinSeconds{};

  // Monotonic save counter. A sync server uses it to tell a stale upload or a
  // restored card backup from new data, so it must never go backwards.
  uint32_t statsRevision = 0;

  static constexpr uint8_t CURRENT_FILE_VERSION = 6;
  static constexpr int CURRENT_FILE_SIZE = 165;

  // Loads stats from cachePath/stats_v6.bin, with fallback reads from the
  // previous versioned filename and legacy cachePath/stats.bin. Returns
  // default-constructed stats if no compatible file exists.
  static BookReadingStats load(const std::string& cachePath);

  // Saves stats to cachePath/stats_v6.bin via a temp file and a rename, so an
  // interrupted write cannot leave a truncated file behind. Increments
  // statsRevision, which is why this is not const.
  void save(const std::string& cachePath);

  // Deletes cachePath/stats_v6.bin, every older versioned filename, and legacy
  // cachePath/stats.bin. Missing files are treated as success.
  static bool remove(const std::string& cachePath);

  // Attributes reading time to the X3 local date/time buckets when RTC data exists.
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // Formats a duration in seconds into a human-readable string.
  // Output examples: "< 1 min", "45 min", "2h 30 min"
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
