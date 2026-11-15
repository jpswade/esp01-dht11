#include "../HumiditySensor/RecentHistory.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main() {
  RecentHistory history;
  assert(history.size() == 0);
  history.addIfDue(0, 43, 25);
  history.addIfDue(4999, 44, 26);
  assert(history.size() == 1);
  history.addIfDue(RecentHistory::INTERVAL_MS, NAN, NAN);
  assert(history.size() == 2 && isnan(history.at(1).humidity));
  for (unsigned i = 2; i < 1000; ++i)
    history.addIfDue(i * RecentHistory::INTERVAL_MS, float(i), 25);
  assert(history.size() == RecentHistory::CAPACITY);
  assert(history.at(0).humidity == 1000 - RecentHistory::CAPACITY);
  assert(history.at(history.size() - 1).humidity == 999);
  assert(history.at(history.size() - 1).sampledAt - history.at(0).sampledAt
         == RecentHistory::WINDOW_MS);

  RecentHistory rollover;
  const uint32_t beforeWrap = UINT32_MAX - 1000;
  rollover.addIfDue(beforeWrap, 43, 25);
  rollover.addIfDue(1000, 44, 26);
  assert(rollover.size() == 1);
  const uint32_t afterWrap = beforeWrap + RecentHistory::INTERVAL_MS;
  rollover.addIfDue(afterWrap, 45, 27);
  assert(rollover.size() == 2 && rollover.at(1).humidity == 45);
  puts("PASS: sampling interval, missing readings, ring wrap, 24h window, millis rollover");
}
