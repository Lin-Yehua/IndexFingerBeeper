#include "DeviceUuid.h"

#include <Preferences.h>

namespace {

constexpr char kDeviceUuidPrefsNs[] = "deviceid";
constexpr char kDeviceUuidPrefsKey[] = "uuid";
constexpr char kDeviceUuidPrefix[] = "UUID";
constexpr size_t kDeviceUuidLen = 18;  // UUID + ssmmhhddmmyyyy

String normalizeUuid(const String &raw) {
  String out = raw;
  out.trim();
  out.toUpperCase();
  return out;
}

bool isUuidDigit(char c) {
  return c >= '0' && c <= '9';
}

bool isDeviceUuidValid(const String &uuidRaw) {
  const String uuid = normalizeUuid(uuidRaw);
  if (uuid.length() != kDeviceUuidLen) return false;
  if (!uuid.startsWith(kDeviceUuidPrefix)) return false;
  for (size_t i = strlen(kDeviceUuidPrefix); i < uuid.length(); ++i) {
    if (!isUuidDigit(uuid[i])) return false;
  }
  return true;
}

String buildDeviceUuidFromDateTime(const Ds1302DateTime &dt) {
  char buf[24] = {0};
  snprintf(buf,
           sizeof(buf),
           "UUID%02u%02u%02u%02u%02u%04u",
           static_cast<unsigned int>(dt.second),
           static_cast<unsigned int>(dt.minute),
           static_cast<unsigned int>(dt.hour),
           static_cast<unsigned int>(dt.day),
           static_cast<unsigned int>(dt.month),
           static_cast<unsigned int>(dt.year));
  return String(buf);
}

}  // namespace

bool deviceUuidRead(String &outUuid) {
  outUuid = "";
  Preferences prefs;
  // Open RW so first boot can create namespace automatically.
  // RO begin would fail when namespace does not exist yet.
  if (!prefs.begin(kDeviceUuidPrefsNs, false)) {
    return false;
  }

  String uuid = prefs.getString(kDeviceUuidPrefsKey, "");
  prefs.end();
  uuid = normalizeUuid(uuid);
  if (isDeviceUuidValid(uuid)) {
    outUuid = uuid;
  }
  return true;
}

bool deviceUuidEnsureFromDateTime(const Ds1302DateTime &dt, String &outUuid, String &errorOut) {
  outUuid = "";
  errorOut = "";

  if (!ds1302IsValidDateTime(dt)) {
    errorOut = "invalid datetime";
    return false;
  }

  String existing;
  if (!deviceUuidRead(existing)) {
    errorOut = "preferences read failed";
    return false;
  }
  if (existing.length()) {
    outUuid = existing;
    return true;
  }

  const String uuid = buildDeviceUuidFromDateTime(dt);
  if (!isDeviceUuidValid(uuid)) {
    errorOut = "uuid format invalid";
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kDeviceUuidPrefsNs, false)) {
    errorOut = "preferences write open failed";
    return false;
  }
  const size_t stored = prefs.putString(kDeviceUuidPrefsKey, uuid);
  prefs.end();
  if (stored != uuid.length()) {
    errorOut = "preferences write failed";
    return false;
  }

  outUuid = uuid;
  return true;
}

bool deviceUuidEnsureFromRtc(String &outUuid, String &errorOut) {
  outUuid = "";
  errorOut = "";

  Ds1302DateTime dt;
  if (!rtc.readDateTime(dt) || !ds1302IsValidDateTime(dt)) {
    errorOut = "rtc invalid";
    return false;
  }

  return deviceUuidEnsureFromDateTime(dt, outUuid, errorOut);
}
