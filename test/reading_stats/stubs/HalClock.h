#pragma once
#include <cstdint>

// Host-test stub for lib/hal/HalClock.h, which depends on Arduino and the SDK
// Rtc driver. ReadingStatsUtils.cpp calls getDateTime() from
// getCurrentLocalReadingStatsDateTime only; the stub reports no RTC, so that one
// function returns false on the host and every date helper under test stays
// pure. HalClockStub.cpp defines the singleton.
class HalClock {
 public:
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
};

extern HalClock halClock;
