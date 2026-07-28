#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

namespace {
constexpr const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool isLeap(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t monthLength(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeap(year)) return 29;
  return kLengths[month - 1];
}

bool isValidDate(const uint16_t year, const uint8_t month, const uint8_t day) {
  return month >= 1 && month <= 12 && day >= 1 && day <= monthLength(year, month);
}

// Shifts the date by a single day in either direction. Only +/-1 is needed: the UTC
// offset range is [-12:00, +14:00], so applying it can cross at most one midnight.
void adjustDateByOneDay(uint16_t& year, uint8_t& month, uint8_t& day, const int delta) {
  if (delta == 0) return;
  if (delta > 0) {
    if (day < monthLength(year, month)) {
      day++;
    } else {
      day = 1;
      if (month == 12) {
        month = 1;
        year++;
      } else {
        month++;
      }
    }
    return;
  }
  if (day > 1) {
    day--;
  } else {
    if (month == 1) {
      month = 12;
      year--;
    } else {
      month--;
    }
    day = monthLength(year, month);
  }
}
}  // namespace

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) return false;

  year = dt.year;
  month = dt.month;
  day = dt.day;
  hour = dt.hour;
  minute = dt.minute;
  return true;
}

bool HalClock::formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased) const {
  if (bufSize < 13u) return false;

  uint16_t year;
  uint8_t month, day, hour, minute;
  if (!getDateTime(year, month, day, hour, minute)) return false;

  // Same clamp as formatTime, so a corrupted persisted offset can't shift the date
  // outside the real UTC range.
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  const int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;
  const int dayDelta = localMinutes < 0 ? -1 : (localMinutes >= 1440 ? 1 : 0);
  adjustDateByOneDay(year, month, day, dayDelta);
  if (!isValidDate(year, month, day)) return false;

  snprintf(buf, bufSize, "%s %u, %u", kMonthNames[month - 1], static_cast<unsigned int>(day),
           static_cast<unsigned int>(year));
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
