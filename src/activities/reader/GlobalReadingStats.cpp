#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_mac.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "ReadingStatsFileIo.h"
#include "ReadingStatsSerialization.h"

namespace {
// The binary layouts for every historical version live in
// ReadingStatsSerialization.cpp alongside the parser that reads them.
static constexpr int GLOBAL_STATS_FILE_SIZE = static_cast<int>(GlobalReadingStats::CURRENT_FILE_SIZE);
static constexpr int GLOBAL_STATS_HEADER_SIZE = static_cast<int>(GlobalReadingStats::HEADER_BLOCK_SIZE);
static constexpr int GLOBAL_STATS_TAIL_SIZE = static_cast<int>(GlobalReadingStats::TAIL_BLOCK_SIZE);
static constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
static constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";
static constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";
static bool s_blockDestructiveSave = false;

// Set when load() migrated a pre-v4 file, cleared once the one-shot backup below
// has been attempted. saveToFile rotates global_stats.bin -> .bak on every save,
// and a save happens on every reader exit, so at ~9-minute sessions the rotating
// backup is under 20 minutes of reading away from holding post-migration data
// too. The one-shot copy is the only thing that survives that window.
static uint8_t s_pendingLegacyBackupVersion = 0;

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
  for (size_t i = 0; i < target.wpmBinCount.size(); ++i) {
    target.wpmBinCount[i] = addSaturated(target.wpmBinCount[i], source.wpmBinCount[i]);
    target.wpmBinWords[i] = addSaturated(target.wpmBinWords[i], source.wpmBinWords[i]);
    target.wpmBinSeconds[i] = addSaturated(target.wpmBinSeconds[i], source.wpmBinSeconds[i]);
  }
  mergeReadingHistory(target.readingHistoryAnchorDay, target.readingHistoryBits, source.readingHistoryAnchorDay,
                      source.readingHistoryBits);
  target.longestReadingStreak = std::max(target.longestReadingStreak, source.longestReadingStreak);
  // statsRevision, tokenRuleVersion and idleThresholdSeconds describe the device
  // that produced a file, so they are deliberately not summed: the aggregate keeps
  // the local device's values.
}

StatsLoadOutcome loadFromOpenFile(HalFile& f, GlobalReadingStats& out) {
  // Two sequential passes over one 159-byte buffer: the whole 270-byte file in a
  // single frame would break the 256-byte stack-local limit, and this chain runs
  // through HalStorage into SdFat, which has substantial frames of its own.
  const size_t fileSize = f.fileSize();
  uint8_t data[GLOBAL_STATS_HEADER_SIZE] = {};
  const size_t bytesToRead = std::min(fileSize, static_cast<size_t>(GLOBAL_STATS_HEADER_SIZE));
  const int n = f.read(data, bytesToRead);
  const StatsLoadOutcome outcome = parseGlobalStatsHeader(data, n, fileSize, out);
  if (!globalStatsHasTail(outcome)) return outcome;

  uint8_t tail[GLOBAL_STATS_TAIL_SIZE] = {};
  if (f.read(tail, GLOBAL_STATS_TAIL_SIZE) != GLOBAL_STATS_TAIL_SIZE) {
    // The header parsed and the size said v4, so a short tail read means the file
    // is damaged. Reject the whole thing rather than reporting zeroed bins as data.
    return StatsLoadOutcome{StatsLoadResult::Invalid, outcome.version, fileSize};
  }
  parseGlobalStatsTail(tail, out);
  return outcome;
}

std::string localSyncedStatsFileName() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != 0) return {};

  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

// Copies the pre-migration file aside once, before the first v4 save overwrites
// it. Chunked rather than read-whole-file: the source is 159 bytes today but the
// guarantee should not depend on that.
bool copyFile(const char* fromPath, const char* toPath) {
  HalFile src;
  if (!Storage.openFileForRead("GSTATS", fromPath, src)) return false;

  HalFile dst;
  if (!Storage.openFileForWrite("GSTATS", toPath, dst)) {
    src.close();
    return false;
  }

  uint8_t buf[64];
  bool ok = true;
  while (true) {
    const int n = src.read(buf, sizeof(buf));
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    if (dst.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      ok = false;
      break;
    }
  }

  src.close();
  dst.flush();
  if (!dst.close()) ok = false;
  if (!ok) Storage.remove(toPath);
  return ok;
}

// One-shot, never rotated. Runs before the first v4 write, so the pre-migration
// file survives even if every later save is bad.
void backupLegacyStatsOnce() {
  if (s_pendingLegacyBackupVersion == 0) return;
  const uint8_t version = s_pendingLegacyBackupVersion;
  s_pendingLegacyBackupVersion = 0;

  // Named for the version being preserved, so a v1 or v2 file is not filed under
  // a name claiming it is v3.
  char backupPath[48];
  snprintf(backupPath, sizeof(backupPath), "/.crosspoint/global_stats.v%u.bak", version);
  if (Storage.exists(backupPath)) return;

  // The flag was armed by a load that may have happened several saves ago. Only
  // copy a file that is still pre-v4 in size, so a post-migration file can never
  // be filed away as though it were the original.
  HalFile f;
  if (!Storage.openFileForRead("GSTATS", GLOBAL_STATS_PATH, f)) return;
  const size_t fileSize = f.fileSize();
  f.close();
  if (fileSize == 0 || fileSize >= GlobalReadingStats::CURRENT_FILE_SIZE) return;

  if (copyFile(GLOBAL_STATS_PATH, backupPath)) {
    LOG_INF("GSTATS", "Kept a one-shot pre-migration backup of the v%u global stats", version);
  } else {
    LOG_ERR("GSTATS", "Could not write the pre-migration global stats backup");
  }
}

struct GlobalStatsWriteContext {
  const GlobalReadingStats* stats;
};

bool writeGlobalStatsBody(HalFile& f, const void* ctx) {
  // Two sequential passes over one 159-byte buffer; see loadFromOpenFile.
  const GlobalReadingStats& stats = *static_cast<const GlobalStatsWriteContext*>(ctx)->stats;
  uint8_t data[GLOBAL_STATS_HEADER_SIZE];

  serializeGlobalStatsHeader(stats, data);
  if (f.write(data, GLOBAL_STATS_HEADER_SIZE) != static_cast<size_t>(GLOBAL_STATS_HEADER_SIZE)) {
    LOG_ERR("GSTATS", "Short write for the global stats header block");
    return false;
  }

  serializeGlobalStatsTail(stats, data);
  if (f.write(data, GLOBAL_STATS_TAIL_SIZE) != static_cast<size_t>(GLOBAL_STATS_TAIL_SIZE)) {
    LOG_ERR("GSTATS", "Short write for the global stats tail block");
    return false;
  }
  return true;
}

bool saveToFile(const GlobalReadingStats& stats, const char* path, const char* backupPath) {
  const GlobalStatsWriteContext ctx{&stats};
  return writeStatsFileAtomically("GSTATS", path, backupPath, &writeGlobalStatsBody, &ctx, GLOBAL_STATS_FILE_SIZE);
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
  if (primary.result == StatsLoadResult::Ok) {
    if (primary.version < CURRENT_FILE_VERSION) {
      // The next save upgrades this file in place. Arm the one-shot backup so the
      // pre-migration bytes are kept before that happens. Only the primary file
      // arms it: if we are here having recovered from the rotating .bak instead,
      // the primary is already damaged and not worth preserving.
      s_pendingLegacyBackupVersion = primary.version;
    }
    return stats;
  }
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

void GlobalReadingStats::save() {
  if (s_blockDestructiveSave) {
    LOG_ERR("GSTATS", "Refusing to overwrite on-disk stats after newer-format file was detected");
    return;
  }

  backupLegacyStatsOnce();

  // Bump before writing so the file always carries the new value; see
  // BookReadingStats::save() for why a failed save may leave a gap.
  statsRevision++;
  // Both scalars describe the conditions the data was collected under rather than
  // the data itself, so they are snapshotted at save time instead of being
  // maintained incrementally.
  tokenRuleVersion = TOKEN_RULE_VERSION;
  idleThresholdSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();

  saveToFile(*this, GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH);
}

bool GlobalReadingStats::resetLocal() {
  // A deliberate reset writes no backup at all (backupPath is null below), so the
  // pending one-shot copy is dropped rather than left to fire on a later save and
  // file the post-reset file away as though it were the pre-migration original.
  s_pendingLegacyBackupVersion = 0;

  GlobalReadingStats fresh;
  // A reset must not rewind statsRevision: a sync server reads a counter going
  // backwards as a restored backup or a rollback, and would then treat everything
  // uploaded afterwards as stale.
  GlobalReadingStats existing;
  if (loadFromFile(GLOBAL_STATS_PATH, existing).result == StatsLoadResult::Ok) {
    fresh.statsRevision = existing.statsRevision;
  }
  fresh.statsRevision++;
  fresh.tokenRuleVersion = TOKEN_RULE_VERSION;
  fresh.idleThresholdSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  return saveToFile(fresh, GLOBAL_STATS_PATH, nullptr);
}

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
