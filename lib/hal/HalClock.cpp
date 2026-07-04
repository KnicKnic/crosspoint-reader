#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  uint8_t second = 0;
  return getTime(hour, minute, second);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute, uint8_t& second) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    second = _cachedSecond;
    return true;
  }

  if (!readDateTimeFromRTC()) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
  }

  hour = _cachedHour;
  minute = _cachedMinute;
  second = _cachedSecond;
  return true;
}

bool HalClock::getDate(uint16_t& year, uint8_t& month, uint8_t& day) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    if (!readDateTimeFromRTC() && !_hasCachedTime) return false;
  }

  year = _cachedYear;
  month = _cachedMonth;
  day = _cachedDay;
  return true;
}

bool HalClock::readDateTimeFromRTC() const {
  const unsigned long now = millis();

  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)7);
  if (Wire.available() < 7) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();
  Wire.read();  // weekday
  const uint8_t rawDay = Wire.read();
  const uint8_t rawMonth = Wire.read();
  const uint8_t rawYear = Wire.read();

  _cachedSecond = bcdToDec(rawSec & 0x7F);
  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _cachedDay = bcdToDec(rawDay & 0x3F);
  _cachedMonth = bcdToDec(rawMonth & 0x1F);
  _cachedYear = 2000 + bcdToDec(rawYear);
  if (rawMonth & 0x80) {
    _cachedYear += 100;
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour,
                          bool includeSeconds) const {
  if (bufSize < (use12Hour ? (includeSeconds ? 12u : 9u) : (includeSeconds ? 9u : 6u))) return false;
  uint8_t h, m;
  uint8_t s;
  if (!getTime(h, m, s)) return false;

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
    if (includeSeconds) {
      snprintf(buf, bufSize, "%d:%02d:%02u %s", hour12, min, static_cast<unsigned>(s), pm ? "PM" : "AM");
    } else {
      snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
    }
  } else {
    if (includeSeconds) {
      snprintf(buf, bufSize, "%02d:%02d:%02u", hour24, min, static_cast<unsigned>(s));
    } else {
      snprintf(buf, bufSize, "%02d:%02d", hour24, min);
    }
  }
  return true;
}

bool HalClock::formatDate(char* buf, size_t bufSize) const {
  if (bufSize < 11u) return false;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  if (!getDate(year, month, day)) return false;

  snprintf(buf, bufSize, "%04u-%02u-%02u", year, month, day);
  return true;
}

bool HalClock::writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour,
                                  uint8_t minute, uint8_t second) {
  assert(year >= 2000 && year <= 2199);
  assert(month >= 1 && month <= 12);
  assert(day >= 1 && day <= 31);
  assert(weekday >= 1 && weekday <= 7);
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  const bool century = year >= 2100;
  const uint8_t dsYear = static_cast<uint8_t>(year % 100);

  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  Wire.write(decToBcd(weekday));
  Wire.write(decToBcd(day));
  Wire.write(decToBcd(month) | (century ? 0x80 : 0x00));
  Wire.write(decToBcd(dsYear));
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write date/time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _cachedSecond = second;
  _cachedYear = year;
  _cachedMonth = month;
  _cachedDay = day;
  _hasCachedTime = true;
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

      const uint16_t year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      const uint8_t month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      const uint8_t day = static_cast<uint8_t>(timeinfo.tm_mday);
      const uint8_t weekday = static_cast<uint8_t>(timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday);
      if (writeDateTimeToRTC(year, month, day, weekday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02d:%02d:%02d UTC", year, month, day, timeinfo.tm_hour,
                timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
