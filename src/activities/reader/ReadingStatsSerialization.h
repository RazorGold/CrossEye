#pragma once
#include <cstddef>
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Pure buffer <-> struct conversion for both reading stats files. No HalStorage,
// no logging, no Arduino: file I/O stays in BookReadingStats.cpp /
// GlobalReadingStats.cpp so this translation unit can be covered by the host
// tests in test/reading_stats/.

// Parses any historical per-book layout (v1..v5) out of a buffer holding n bytes.
// Returns false when the (size, version) pair is not recognised, leaving out
// untouched so the caller can start fresh.
bool parseStatsBuffer(const uint8_t* data, int n, BookReadingStats& out);

// Writes BookReadingStats::CURRENT_FILE_SIZE bytes of the current layout.
void serializeStatsBuffer(const BookReadingStats& stats, uint8_t* data);

enum class StatsLoadResult : uint8_t { Ok, Invalid, NewerFormat };

struct StatsLoadOutcome {
  StatsLoadResult result = StatsLoadResult::Invalid;
  uint8_t version = 0;
  size_t fileSize = 0;
};

// Parses any historical global layout (v1..v3) out of a buffer holding n bytes
// read from a file of fileSize bytes. fileSize is what distinguishes a
// newer-format file (which must never be overwritten) from a corrupt one, so it
// is passed in rather than inferred from n.
StatsLoadOutcome parseGlobalStatsBuffer(const uint8_t* data, int n, size_t fileSize, GlobalReadingStats& out);

// Writes GlobalReadingStats::CURRENT_FILE_SIZE bytes of the current layout.
void serializeGlobalStatsBuffer(const GlobalReadingStats& stats, uint8_t* data);
