#pragma once
#include <stdint.h>

struct HistoryReading {
  uint32_t sampledAt;
  float humidity;
  float temperature;
};

class RecentHistory {
 public:
  static constexpr uint32_t INTERVAL_MS = 5UL * 60 * 1000;
  static constexpr uint32_t WINDOW_MS = 24UL * 60 * 60 * 1000;
  static constexpr uint16_t CAPACITY = WINDOW_MS / INTERVAL_MS + 1;

  void addIfDue(uint32_t now, float humidity, float temperature) {
    if (count_ && uint32_t(now - lastAt_) < INTERVAL_MS) return;
    readings_[next_] = {now, humidity, temperature};
    next_ = (next_ + 1) % CAPACITY;
    if (count_ < CAPACITY) ++count_;
    lastAt_ = now;
  }

  uint16_t size() const { return count_; }
  const HistoryReading& at(uint16_t chronologicalIndex) const {
    return readings_[(next_ + CAPACITY - count_ + chronologicalIndex) % CAPACITY];
  }

 private:
  HistoryReading readings_[CAPACITY] = {};
  uint16_t next_ = 0, count_ = 0;
  uint32_t lastAt_ = 0;
};
