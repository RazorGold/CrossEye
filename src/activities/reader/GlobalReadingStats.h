#pragma once
#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total forward page turns after the dwell threshold
  uint32_t completedBooks = 0;       // Books manually marked as finished
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;

  // Words/Min histogram, same bins as BookReadingStats but with uint32_t counts:
  // 65535 pages in one bin is ~312 hours of reading, which a heavy reader reaches
  // in a few years across all books. A saturated count under-weights itself in the
  // trim walk and quietly shifts the boundary, so global gets the wider type.
  std::array<uint32_t, WPM_BIN_COUNT> wpmBinCount{};
  std::array<uint32_t, WPM_BIN_COUNT> wpmBinWords{};
  std::array<uint32_t, WPM_BIN_COUNT> wpmBinSeconds{};

  // Sync scalars. Reserved now rather than in a later format bump, because the
  // load path dispatches on file size and a second bump means a second migration.
  uint32_t statsRevision = 0;         // Monotonic save counter; never goes backwards
  uint8_t tokenRuleVersion = 0;       // Word-token rule that produced the bins
  uint16_t idleThresholdSeconds = 0;  // Idle threshold the data was collected under

  static constexpr uint8_t CURRENT_FILE_VERSION = 4;
  static constexpr size_t CURRENT_FILE_SIZE = 270;
  // The v3 block still leads the file, and 270 bytes exceeds the 256-byte
  // stack-local limit, so load and save walk it in two sequential passes over one
  // buffer of this size.
  static constexpr size_t HEADER_BLOCK_SIZE = 159;
  static constexpr size_t TAIL_BLOCK_SIZE = CURRENT_FILE_SIZE - HEADER_BLOCK_SIZE;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;

  // Loads stats from /.crosspoint/global_stats.bin. Returns default-constructed
  // stats if the file is missing or the version byte does not match.
  static GlobalReadingStats load();

  // Returns true when the optional synced stats directory exists.
  static bool hasSyncedStats();

  // Loads this device's local stats plus one synced stats file per other device
  // from /.crosspoint/synced_stats/. A stale file matching this device's MAC is
  // skipped to avoid double counting.
  static GlobalReadingStats loadAggregated();

  // Adds synced device stats to an already-loaded local stats snapshot. Use this
  // when the local stats may include in-memory changes that are not saved yet.
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);

  // Saves stats to /.crosspoint/global_stats.bin. Increments statsRevision, which
  // is why this is not const.
  void save();

  // Replaces /.crosspoint/global_stats.bin with a fresh empty file without
  // rotating or deleting any backup files.
  static bool resetLocal();

  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};
