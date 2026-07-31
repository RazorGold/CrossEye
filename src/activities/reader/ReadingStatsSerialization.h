#pragma once
#include <cstddef>
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Pure buffer <-> struct conversion for both reading stats files. No HalStorage,
// no logging, no Arduino: file I/O stays in BookReadingStats.cpp /
// GlobalReadingStats.cpp so this translation unit can be covered by the host
// tests in test/reading_stats/.

// Parses any historical per-book layout (v1..v6) out of a buffer holding n bytes.
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

// The global file is 270 bytes, which exceeds the 256-byte stack-local limit, so
// it is read and written in two sequential passes over one HEADER_BLOCK_SIZE
// buffer. The header block is the v1..v3 layout unchanged; the tail block holds
// the histogram and the sync scalars added in v4.
//
// Parses the header block out of a buffer holding n bytes read from a file of
// fileSize bytes. fileSize is what distinguishes a newer-format file (which must
// never be overwritten) from a corrupt one, so it is passed in rather than
// inferred from n. On Ok, call globalStatsHasTail() to find out whether a second
// pass is needed.
StatsLoadOutcome parseGlobalStatsHeader(const uint8_t* data, int n, size_t fileSize, GlobalReadingStats& out);

// True when the parsed file carries a v4 tail block that still has to be read.
bool globalStatsHasTail(const StatsLoadOutcome& outcome);

// Parses the GlobalReadingStats::TAIL_BLOCK_SIZE tail block. Only valid after
// parseGlobalStatsHeader returned Ok with globalStatsHasTail() true.
void parseGlobalStatsTail(const uint8_t* data, GlobalReadingStats& out);

// Write halves of the two passes above.
void serializeGlobalStatsHeader(const GlobalReadingStats& stats, uint8_t* data);
void serializeGlobalStatsTail(const GlobalReadingStats& stats, uint8_t* data);
