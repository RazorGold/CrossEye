#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_mac.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "ReadingStatsSerialization.h"

namespace {
// The binary layouts for every historical version live in
// ReadingStatsSerialization.cpp alongside the parser that reads them.
static constexpr int GLOBAL_STATS_FILE_SIZE = static_cast<int>(GlobalReadingStats::CURRENT_FILE_SIZE);
static constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
static constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";
static constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";
static bool s_blockDestructiveSave = false;

uint32_t addSaturated(const uint32_t a, const uint32_t b) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  return max - a < b ? max : a + b;
}

void addStats(GlobalReadingStats& target, const GlobalReadingStats& source) {
  target.totalSessions = addSaturated(target.totalSessions, source.totalSessions);
  target.totalReadingSeconds = addSaturated(target.totalReadingSeconds, source.totalReadingSeconds);
  target.totalPagesTurned = addSaturated(target.totalPagesTurned, source.totalPagesTurned);
  target.completedBooks = addSaturated(target.completedBooks, source.completedBooks);
  for (size_t i = 0; i < target.timeOfDaySeconds.size(); ++i) {
    target.timeOfDaySeconds[i] = addSaturated(target.timeOfDaySeconds[i], source.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < target.dayOfWeekSeconds.size(); ++i) {
    target.dayOfWeekSeconds[i] = addSaturated(target.dayOfWeekSeconds[i], source.dayOfWeekSeconds[i]);
  }
  mergeReadingHistory(target.readingHistoryAnchorDay, target.readingHistoryBits, source.readingHistoryAnchorDay,
                      source.readingHistoryBits);
  target.longestReadingStreak = std::max(target.longestReadingStreak, source.longestReadingStreak);
}

StatsLoadOutcome loadFromOpenFile(HalFile& f, GlobalReadingStats& out) {
  const size_t fileSize = f.fileSize();
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const size_t bytesToRead = std::min(fileSize, static_cast<size_t>(GLOBAL_STATS_FILE_SIZE));
  const int n = f.read(data, bytesToRead);
  return parseGlobalStatsBuffer(data, n, fileSize, out);
}

std::string localSyncedStatsFileName() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != 0) return {};

  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

bool verifyFileSize(const char* path, const size_t expectedSize) {
  HalFile file;
  if (!Storage.openFileForRead("GSTATS", path, file)) return false;
  const size_t actualSize = file.fileSize();
  file.close();
  return actualSize == expectedSize;
}

bool saveToFile(const GlobalReadingStats& stats, const char* path, const char* backupPath) {
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("GSTATS", "Could not remove stale stats temp file: %s", tmpPath.c_str());
    return false;
  }

  HalFile f;
  if (!Storage.openFileForWrite("GSTATS", tmpPath.c_str(), f)) {
    LOG_ERR("GSTATS", "Could not write stats temp file: %s", tmpPath.c_str());
    return false;
  }

  uint8_t data[GLOBAL_STATS_FILE_SIZE];
  serializeGlobalStatsBuffer(stats, data);
  const size_t bytesWritten = f.write(data, GLOBAL_STATS_FILE_SIZE);
  if (bytesWritten != GLOBAL_STATS_FILE_SIZE) {
    LOG_ERR("GSTATS", "Short write for stats temp file %s: %u/%u bytes", tmpPath.c_str(),
            static_cast<unsigned>(bytesWritten), static_cast<unsigned>(GLOBAL_STATS_FILE_SIZE));
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  // CrossInk called FsFile::sync() here. HalFile does not expose sync(), and it is not
  // needed: SdFat's close() syncs internally, the close() below is checked, and
  // verifyFileSize() re-reads the file afterwards. Durability of the write-temp-then-
  // rename pattern is unchanged.
  f.flush();

  if (!f.close()) {
    LOG_ERR("GSTATS", "Failed to close stats temp file after save: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!verifyFileSize(tmpPath.c_str(), GLOBAL_STATS_FILE_SIZE)) {
    LOG_ERR("GSTATS", "Stats temp file has unexpected size: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (backupPath != nullptr) {
    if (Storage.exists(backupPath) && !Storage.remove(backupPath)) {
      LOG_ERR("GSTATS", "Could not remove old stats backup: %s", backupPath);
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (Storage.exists(path) && !Storage.rename(path, backupPath)) {
      LOG_ERR("GSTATS", "Could not rotate stats backup: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
  } else if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("GSTATS", "Could not replace stats file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR("GSTATS", "Could not replace stats file: %s", path);
    if (backupPath != nullptr && Storage.exists(backupPath) && !Storage.exists(path)) {
      Storage.rename(backupPath, path);
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
}  // namespace

static StatsLoadOutcome loadFromFile(const char* path, GlobalReadingStats& out) {
  StatsLoadOutcome outcome;
  HalFile f;
  if (!Storage.openFileForRead("GSTATS", path, f)) return outcome;
  outcome = loadFromOpenFile(f, out);
  f.close();
  return outcome;
}

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  const StatsLoadOutcome primary = loadFromFile(GLOBAL_STATS_PATH, stats);
  if (primary.result == StatsLoadResult::Ok) return stats;
  if (primary.result == StatsLoadResult::NewerFormat) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "On-disk stats are from a newer build (v%u, %u bytes); refusing to overwrite", primary.version,
            static_cast<unsigned>(primary.fileSize));
    return stats;
  }

  const StatsLoadOutcome backup = loadFromFile(GLOBAL_STATS_BAK_PATH, stats);
  if (backup.result == StatsLoadResult::Ok) {
    LOG_DBG("GSTATS", "Recovered global stats from backup");
    return stats;
  }
  if (backup.result == StatsLoadResult::NewerFormat) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Backup stats are from a newer build (v%u, %u bytes); refusing to overwrite", backup.version,
            static_cast<unsigned>(backup.fileSize));
    return stats;
  }

  LOG_DBG("GSTATS", "Global stats missing or corrupt, starting fresh");
  return stats;
}

GlobalReadingStats GlobalReadingStats::loadAggregated() { return loadAggregated(load()); }

bool GlobalReadingStats::hasSyncedStats() {
  HalFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return false;

  const bool exists = dir.isDirectory();
  dir.close();
  return exists;
}

GlobalReadingStats GlobalReadingStats::loadAggregated(const GlobalReadingStats& localStats) {
  GlobalReadingStats stats = localStats;
  HalFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return stats;

  if (!dir.isDirectory()) {
    dir.close();
    return stats;
  }

  char name[128];
  const std::string localFileName = localSyncedStatsFileName();
  uint16_t loadedCount = 0;
  uint16_t skippedCount = 0;
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));

    // Older firmware or manual copies may leave this device's own file here.
    // Skip it because local stats are already included from global_stats.bin.
    if (!isDirectory && nameLen > 0 && (localFileName.empty() || strcmp(name, localFileName.c_str()) != 0)) {
      GlobalReadingStats syncedStats;
      const StatsLoadOutcome outcome = loadFromOpenFile(file, syncedStats);
      if (outcome.result == StatsLoadResult::Ok) {
        addStats(stats, syncedStats);
        loadedCount++;
      } else if (outcome.result == StatsLoadResult::NewerFormat) {
        skippedCount++;
        LOG_DBG("GSTATS", "Skipping newer-format synced stats file: %s (v%u, %u bytes)", name, outcome.version,
                static_cast<unsigned>(outcome.fileSize));
      } else {
        skippedCount++;
        LOG_DBG("GSTATS", "Skipping invalid synced stats file: %s", name);
      }
    }

    file.close();
  }
  dir.close();

  if (loadedCount > 0 || skippedCount > 0) {
    LOG_DBG("GSTATS", "Aggregated %u synced stats file(s), skipped %u", static_cast<unsigned>(loadedCount),
            static_cast<unsigned>(skippedCount));
  }
  return stats;
}

void GlobalReadingStats::save() const {
  if (s_blockDestructiveSave) {
    LOG_ERR("GSTATS", "Refusing to overwrite on-disk stats after newer-format file was detected");
    return;
  }
  saveToFile(*this, GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH);
}

bool GlobalReadingStats::resetLocal() { return saveToFile(GlobalReadingStats{}, GLOBAL_STATS_PATH, nullptr); }

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);

  const uint16_t historyLongest = computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits);
  if (historyLongest > longestReadingStreak) {
    longestReadingStreak = historyLongest;
  }
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
