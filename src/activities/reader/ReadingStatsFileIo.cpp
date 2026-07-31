#include "ReadingStatsFileIo.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace {
bool verifyFileSize(const char* module, const char* path, const size_t expectedSize) {
  HalFile file;
  if (!Storage.openFileForRead(module, path, file)) return false;
  const size_t actualSize = file.fileSize();
  file.close();
  return actualSize == expectedSize;
}
}  // namespace

bool writeStatsFileAtomically(const char* module, const char* path, const char* backupPath,
                              const StatsWriteBodyFn writeBody, const void* ctx, const size_t expectedSize) {
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(module, "Could not remove stale stats temp file: %s", tmpPath.c_str());
    return false;
  }

  HalFile f;
  if (!Storage.openFileForWrite(module, tmpPath.c_str(), f)) {
    LOG_ERR(module, "Could not write stats temp file: %s", tmpPath.c_str());
    return false;
  }

  if (!writeBody(f, ctx)) {
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
    LOG_ERR(module, "Failed to close stats temp file after save: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!verifyFileSize(module, tmpPath.c_str(), expectedSize)) {
    LOG_ERR(module, "Stats temp file has unexpected size: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (backupPath != nullptr) {
    if (Storage.exists(backupPath) && !Storage.remove(backupPath)) {
      LOG_ERR(module, "Could not remove old stats backup: %s", backupPath);
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (Storage.exists(path) && !Storage.rename(path, backupPath)) {
      LOG_ERR(module, "Could not rotate stats backup: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
  } else if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR(module, "Could not replace stats file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR(module, "Could not replace stats file: %s", path);
    if (backupPath != nullptr && Storage.exists(backupPath) && !Storage.exists(path)) {
      Storage.rename(backupPath, path);
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
