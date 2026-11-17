#pragma once
#include <stdint.h>

enum class RestartIntent : uint32_t { None, WifiTimeout, SetupTimeout, OtaUpdate };

struct alignas(4) RestartRecord {
  uint32_t magic, version, boots, intent, checksum;
};

inline RestartRecord makeRestartRecord(uint32_t boots, RestartIntent intent) {
  RestartRecord record = {0x48554D44, 1, boots, uint32_t(intent), 0};
  record.checksum = record.magic ^ record.version ^ record.boots ^ record.intent ^ 0xB17D1A65;
  return record;
}

inline bool validRestartRecord(const RestartRecord& record) {
  return record.magic == 0x48554D44 && record.version == 1 && record.boots > 0 &&
    record.intent <= uint32_t(RestartIntent::OtaUpdate) &&
    record.checksum == (record.magic ^ record.version ^ record.boots ^ record.intent ^ 0xB17D1A65);
}

inline RestartIntent beginBootRecord(RestartRecord& record) {
  const bool valid = validRestartRecord(record);
  const RestartIntent previous = valid ? RestartIntent(record.intent) : RestartIntent::None;
  const uint32_t boots = valid && record.boots != UINT32_MAX ? record.boots + 1 : 1;
  record = makeRestartRecord(boots, RestartIntent::None);
  return previous;
}

inline const char* restartIntentName(RestartIntent intent) {
  switch (intent) {
    case RestartIntent::WifiTimeout: return "wifi_timeout";
    case RestartIntent::SetupTimeout: return "setup_timeout";
    case RestartIntent::OtaUpdate: return "ota_update";
    default: return "none_recorded";
  }
}
