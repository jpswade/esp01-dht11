#include "../HumiditySensor/RestartRecord.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main() {
  RestartRecord record = {};
  assert(!validRestartRecord(record));
  assert(beginBootRecord(record) == RestartIntent::None);
  assert(validRestartRecord(record) && record.boots == 1);
  const RestartIntent intents[] = {RestartIntent::WifiTimeout, RestartIntent::SetupTimeout, RestartIntent::OtaUpdate};
  for (RestartIntent intent : intents) {
    record = makeRestartRecord(record.boots, intent);
    const auto boots = record.boots;
    assert(beginBootRecord(record) == intent);
    assert(record.boots == boots + 1);
    assert(record.intent == uint32_t(RestartIntent::None));
    assert(beginBootRecord(record) == RestartIntent::None); // A past intent is not reused.
  }
  record.intent ^= 1;
  assert(!validRestartRecord(record));
  assert(beginBootRecord(record) == RestartIntent::None && record.boots == 1);
  record = makeRestartRecord(4, RestartIntent(99));
  assert(!validRestartRecord(record));
  record = makeRestartRecord(UINT32_MAX, RestartIntent::WifiTimeout);
  assert(beginBootRecord(record) == RestartIntent::WifiTimeout && record.boots == 1);
  assert(strcmp(restartIntentName(RestartIntent::WifiTimeout), "wifi_timeout") == 0);
  puts("PASS: reset intents, corruption rejection, boot count and stale-intent clearing");
}
