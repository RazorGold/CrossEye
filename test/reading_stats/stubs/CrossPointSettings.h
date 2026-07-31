#pragma once
#include <cstdint>

// Host-test stub for src/CrossPointSettings.h, which pulls in ArduinoJson and
// PersistableStore and cannot compile on the host. ReadingStatsUtils.cpp reads a
// single field from it (the UTC offset used by
// getCurrentLocalReadingStatsDateTime), so the stub carries only that.
class CrossPointSettings {
 public:
  uint8_t clockUtcOffsetQ = 48;  // 48 = UTC+0, in quarter hours

  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
