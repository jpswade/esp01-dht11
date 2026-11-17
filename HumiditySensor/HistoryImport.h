#pragma once
#include "RecentHistory.h"
#include <math.h>
#include <string.h>

// Fixed-size streaming format: HST1, count, then count little-endian Iff records.
// Each record is age_ms, humidity, temperature. NaN represents a failed sample.
class HistoryImport {
 public:
  void begin(uint32_t now) {
    staged_ = RecentHistory(); now_ = now; used_ = 0; received_ = 0;
    expected_ = 0; header_ = false; failed_ = false; previousAge_ = UINT32_MAX;
  }
  void feed(const uint8_t* data, size_t size) {
    while (size-- && !failed_) {
      buffer_[used_++] = *data++;
      const size_t needed = header_ ? 12 : 8;
      if (used_ != needed) continue;
      if (!header_) {
        memcpy(&expected_, buffer_ + 4, 4);
        failed_ = memcmp(buffer_, "HST1", 4) != 0 || expected_ > RecentHistory::CAPACITY;
        header_ = true;
      } else {
        uint32_t age; float humidity, temperature;
        memcpy(&age, buffer_, 4); memcpy(&humidity, buffer_ + 4, 4); memcpy(&temperature, buffer_ + 8, 4);
        const bool humidityOK = isnan(humidity) || (isfinite(humidity) && humidity >= 0 && humidity <= 100);
        const bool temperatureOK = isnan(temperature) || (isfinite(temperature) && temperature >= -50 && temperature <= 100);
        if (received_ >= expected_ || age >= previousAge_ || !humidityOK || !temperatureOK ||
            !staged_.appendRestored(now_, age, humidity, temperature)) failed_ = true;
        previousAge_ = age; ++received_;
      }
      used_ = 0;
    }
  }
  bool complete() const { return header_ && !failed_ && used_ == 0 && received_ == expected_; }
  const RecentHistory& staged() const { return staged_; }
 private:
  RecentHistory staged_;
  uint8_t buffer_[12] = {};
  uint32_t now_ = 0, previousAge_ = UINT32_MAX, expected_ = 0, received_ = 0;
  size_t used_ = 0;
  bool header_ = false, failed_ = true;
};
