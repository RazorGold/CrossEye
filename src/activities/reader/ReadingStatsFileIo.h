#pragma once
#include <cstddef>

class HalFile;

// Writes the body of a stats file into an already-open temp file. Returns false
// to abort the save, which removes the temp file and leaves the live file
// untouched. A plain function pointer plus context rather than std::function:
// std::function heap-allocates its closure and costs ~2-4 KB of binary per
// signature (CLAUDE.md, "Template and std::function Bloat").
using StatsWriteBodyFn = bool (*)(HalFile& file, const void* ctx);

// Durable replace for both reading stats files: write a temp file, flush it,
// close it with the result checked, verify its size on disk, rotate the previous
// file to backupPath (or delete it when backupPath is null), then rename the temp
// over the live path. An interrupted save therefore leaves either the old file or
// the new one, never a truncated file — which matters because a truncated
// stats_v6.bin makes the reader fall back to the stale v5 file and silently
// resurrect pre-migration stats.
bool writeStatsFileAtomically(const char* module, const char* path, const char* backupPath, StatsWriteBodyFn writeBody,
                              const void* ctx, size_t expectedSize);
