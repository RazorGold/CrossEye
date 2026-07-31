#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "ReadingStatsFileIo.h"
#include "ReadingStatsSerialization.h"

namespace {
// The binary layouts for every historical version live in
// ReadingStatsSerialization.cpp alongside the parser that reads them.
static constexpr uint8_t STATS_FILE_VERSION = BookReadingStats::CURRENT_FILE_VERSION;
static constexpr int STATS_FILE_SIZE = BookReadingStats::CURRENT_FILE_SIZE;
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t PREVIOUS_VERSIONED_STATS_FILE_VERSION = STATS_FILE_VERSION - 1;
static constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

bool openStatsFileForRead(const std::string& cachePath, HalFile& f) {
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  if (Storage.openFileForRead("STATS", cachePath + "/" + currentName, f)) {
    return true;
  }

  // When bumping STATS_FILE_VERSION, this automatically tries the previous
  // versioned filename (e.g. v6 falls back to stats_v5.bin) before the original
  // unversioned stats.bin migration source.
  const std::string previousName = statsFileNameForVersion(PREVIOUS_VERSIONED_STATS_FILE_VERSION);
  if (Storage.openFileForRead("STATS", cachePath + "/" + previousName, f)) {
    LOG_DBG("STATS", "Migrating %s to %s", previousName.c_str(), currentName.c_str());
    return true;
  }

  if (Storage.openFileForRead("STATS", cachePath + "/" + LEGACY_STATS_FILE_NAME, f)) {
    LOG_DBG("STATS", "Migrating legacy %s to %s", LEGACY_STATS_FILE_NAME, currentName.c_str());
    return true;
  }

  return false;
}

}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  HalFile f;
  if (!openStatsFileForRead(cachePath, f)) {
    return stats;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const int n = f.read(data, STATS_FILE_SIZE);
  f.close();

  if (!parseStatsBuffer(data, n, stats)) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return BookReadingStats{};
  }
  return stats;
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

void BookReadingStats::save(const std::string& cachePath) {
  // Bump before writing so the file always carries the new value. A failed save
  // leaves the in-memory revision one ahead of the disk, which is harmless: a gap
  // in the sequence means nothing to the sync server, whereas two different files
  // sharing a revision would make a stale upload undetectable.
  statsRevision++;

  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string statsPath = cachePath + "/" + statsFileName;
  const bool ok = writeStatsFileAtomically(
      "STATS", statsPath.c_str(), nullptr,
      [](HalFile& f, const void* ctx) {
        uint8_t data[STATS_FILE_SIZE];
        serializeStatsBuffer(*static_cast<const BookReadingStats*>(ctx), data);
        return f.write(data, STATS_FILE_SIZE) == static_cast<size_t>(STATS_FILE_SIZE);
      },
      this, STATS_FILE_SIZE);
  if (!ok) {
    LOG_ERR("STATS", "Could not write %s", statsFileName.c_str());
  }
}

bool BookReadingStats::remove(const std::string& cachePath) {
  bool ok = true;
  // Every versioned filename, not just the current and previous one: after a
  // version bump the older files are still on the card, still readable by
  // openStatsFileForRead's fallback, and otherwise undeletable through the UI.
  for (uint8_t version = STATS_FILE_VERSION; version >= 1; --version) {
    const std::string statsFileName = statsFileNameForVersion(version);
    const std::string statsPath = cachePath + "/" + statsFileName;
    if (Storage.exists(statsPath.c_str()) && !Storage.remove(statsPath.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", statsFileName.c_str());
      ok = false;
    }
  }

  const std::string legacyStatsPath = cachePath + "/" + LEGACY_STATS_FILE_NAME;
  if (Storage.exists(legacyStatsPath.c_str()) && !Storage.remove(legacyStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", LEGACY_STATS_FILE_NAME);
    ok = false;
  }
  return ok;
}
