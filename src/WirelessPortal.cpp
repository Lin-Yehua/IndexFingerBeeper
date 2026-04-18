#include "WirelessPortal.h"

#include <Arduino.h>
#include <FS.h>
#include <FFat.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

#include "AppGlobals.h"
#include "EspNowMessage.h"
#include "UsbAppMode.h"
#include "Ds1302Rtc.h"

namespace {

constexpr char kDefaultApSsid[] = u8"魔法哔哔机";
constexpr char kDefaultApPassword[] = "12345678";
constexpr uint8_t kDefaultApChannel = 1;
constexpr uint8_t kApChannelMin = 1;
constexpr uint8_t kApChannelMax = 13;
constexpr size_t kApSsidMaxLen = 33;
constexpr size_t kApPasswordMaxLen = 65;
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kDnsPort = 53;
constexpr BaseType_t kWebTaskCore = 0;
constexpr UBaseType_t kWebTaskPriority = 1;
constexpr uint32_t kWebTaskStack = 12288;
constexpr size_t kTextMaxLen = 240;
constexpr size_t kQueueDepth = 128;
constexpr size_t kImageUploadByteLimit = 1024 * 1024;
constexpr uint16_t kImageMaxWidth = 320;
constexpr uint16_t kImageMaxHeight = 240;
constexpr uint16_t kImageDefaultWidth = 320;
constexpr uint16_t kImageDefaultHeight = 140;
constexpr int16_t kImageDefaultCenterX = 160;
constexpr int16_t kImageDefaultCenterY = 155;
constexpr char kPrefsNs[] = "wireless";
constexpr char kPrefsInstantRefreshNoKey[] = "instant_no_key";

struct WebQueuedMessage {
  char text[kTextMaxLen + 1];
};

struct PendingImageFrame {
  uint16_t *pixels = nullptr;
  size_t pixelCapacity = 0;
  size_t pixelCount = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  int16_t centerX = kImageDefaultCenterX;
  int16_t centerY = kImageDefaultCenterY;
  bool ready = false;
};

struct ImageUploadState {
  uint8_t *bytes = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  bool failed = false;
  char error[96] = {0};
};

class PortalWebServer final : public WebServer {
 public:
  explicit PortalWebServer(uint16_t port)
      : WebServer(port) {}

  void handleClient() override {
    if (_currentStatus == HC_NONE) {
      _currentClient = _server.available();
      if (!_currentClient) {
        if (_nullDelay) {
          delay(1);
        }
        return;
      }

      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }

    bool keepCurrentClient = false;
    bool callYield = false;

    if (_currentClient.connected()) {
      switch (_currentStatus) {
        case HC_NONE:
          break;
        case HC_WAIT_READ:
          if (_currentClient.available()) {
            // Avoid long busy-loop parsing on captive-portal half-requests.
            static_cast<Stream &>(_currentClient).setTimeout(120);
            if (_parseRequest(_currentClient)) {
              _currentClient.setTimeout(HTTP_MAX_SEND_WAIT / 1000);
              _contentLength = CONTENT_LENGTH_NOT_SET;
              _handleRequest();
            }
          } else {
            if (millis() - _statusChange <= HTTP_MAX_DATA_WAIT) {
              keepCurrentClient = true;
            }
            callYield = true;
          }
          break;
        case HC_WAIT_CLOSE:
          if (millis() - _statusChange <= HTTP_MAX_CLOSE_WAIT) {
            keepCurrentClient = true;
            callYield = true;
          }
          break;
      }
    }

    if (!keepCurrentClient) {
      _currentClient = WiFiClient();
      _currentStatus = HC_NONE;
      _currentUpload.reset();
      _currentRaw.reset();
    }

    if (callYield) {
      yield();
    }
  }
};

QueueHandle_t gMessageQueue = nullptr;
QueueHandle_t gHostMessageQueue = nullptr;
SemaphoreHandle_t gImageMutex = nullptr;
TaskHandle_t gWebTaskHandle = nullptr;
PortalWebServer *gWebServer = nullptr;
DNSServer *gDnsServer = nullptr;
char gApSsid[kApSsidMaxLen] = {0};
char gApPassword[kApPasswordMaxLen] = {0};
uint8_t gApChannel = kDefaultApChannel;
bool gEnableAp = true;
bool gEnableEspNow = true;
bool gEspNowReady = false;
uint8_t gHostMacFilter[6] = {0};
bool gHostMacFilterEnabled = false;
char gHostMacFilterText[18] = {0};
bool gInstantRefreshNoKey = false;
volatile bool gWebTaskRunning = false;
volatile bool gCsvReloadRequested = false;
volatile bool gScheduleReloadRequested = false;
bool gPortalStarted = false;
PendingImageFrame gPendingImage;
ImageUploadState gImageUpload;
portMUX_TYPE gFlagMux = portMUX_INITIALIZER_UNLOCKED;
struct ImmediateMessageState {
  char text[kTextMaxLen + 1] = {0};
  bool pending = false;
};
ImmediateMessageState gImmediateMessage;
portMUX_TYPE gImmediateMessageMux = portMUX_INITIALIZER_UNLOCKED;

void copyStringToBuf(const String &src, char *dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  if (!src.length()) {
    dst[0] = '\0';
    return;
  }
  src.toCharArray(dst, dstSize);
  dst[dstSize - 1] = '\0';
}

float clampGain(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint8_t clampApChannel(int channel) {
  if (channel < static_cast<int>(kApChannelMin)) return kApChannelMin;
  if (channel > static_cast<int>(kApChannelMax)) return kApChannelMax;
  return static_cast<uint8_t>(channel);
}

bool parseApChannel(const String &raw, uint8_t &outChannel) {
  String s = raw;
  s.trim();
  if (!s.length()) return false;
  const int channel = s.toInt();
  if (channel < static_cast<int>(kApChannelMin) || channel > static_cast<int>(kApChannelMax)) {
    return false;
  }
  outChannel = static_cast<uint8_t>(channel);
  return true;
}

bool parseBoolString(const String &raw, bool &outValue) {
  String v = raw;
  v.trim();
  v.toLowerCase();
  if (!v.length()) return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "enable" || v == "enabled") {
    outValue = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no" || v == "disable" || v == "disabled") {
    outValue = false;
    return true;
  }
  return false;
}

bool parseIntString(const String &raw, int &outValue) {
  String v = raw;
  v.trim();
  if (!v.length()) return false;

  int start = 0;
  if (v[0] == '+' || v[0] == '-') {
    if (v.length() == 1) return false;
    start = 1;
  }

  for (int i = start; i < v.length(); ++i) {
    if (!isDigit(v[i])) return false;
  }

  outValue = v.toInt();
  return true;
}

bool parseFloatString(const String &raw, float &outValue) {
  String v = raw;
  v.trim();
  if (!v.length()) return false;

  char *endPtr = nullptr;
  const float parsed = strtof(v.c_str(), &endPtr);
  if (endPtr == v.c_str()) return false;
  while (*endPtr == ' ' || *endPtr == '\t' || *endPtr == '\r' || *endPtr == '\n') {
    ++endPtr;
  }
  if (*endPtr != '\0') return false;
  if (parsed != parsed) return false;

  outValue = parsed;
  return true;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= (month <= 2U) ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int monthAdj = static_cast<int>(month) + ((month > 2U) ? -3 : 9);
  const unsigned doy = static_cast<unsigned>((153 * monthAdj + 2) / 5 + static_cast<int>(day) - 1);
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

void resetImageUploadStateLocked() {
  if (gImageUpload.bytes) {
    free(gImageUpload.bytes);
  }
  gImageUpload.bytes = nullptr;
  gImageUpload.size = 0;
  gImageUpload.capacity = 0;
  gImageUpload.failed = false;
  gImageUpload.error[0] = '\0';
}

void setImageUploadErrorLocked(const char *msg) {
  gImageUpload.failed = true;
  if (!msg) msg = "upload failed";
  snprintf(gImageUpload.error, sizeof(gImageUpload.error), "%s", msg);
  gImageUpload.error[sizeof(gImageUpload.error) - 1] = '\0';
}

bool ensureImageUploadCapacityLocked(size_t required) {
  if (required <= gImageUpload.capacity) return true;
  size_t nextCap = gImageUpload.capacity ? gImageUpload.capacity : 1024;
  while (nextCap < required && nextCap < kImageUploadByteLimit) {
    const size_t doubled = nextCap * 2;
    if (doubled <= nextCap) break;
    nextCap = doubled;
  }
  if (nextCap < required) nextCap = required;
  if (nextCap > kImageUploadByteLimit) return false;

  uint8_t *next = static_cast<uint8_t *>(realloc(gImageUpload.bytes, nextCap));
  if (!next) return false;
  gImageUpload.bytes = next;
  gImageUpload.capacity = nextCap;
  return true;
}

bool ensurePendingImageCapacityLocked(size_t requiredPixels) {
  if (requiredPixels == 0) return false;
  if (requiredPixels <= gPendingImage.pixelCapacity && gPendingImage.pixels) return true;

  uint16_t *next = static_cast<uint16_t *>(
      realloc(gPendingImage.pixels, requiredPixels * sizeof(uint16_t)));
  if (!next) return false;
  gPendingImage.pixels = next;
  gPendingImage.pixelCapacity = requiredPixels;
  return true;
}

bool parseImageArgs(uint16_t &outWidth,
                    uint16_t &outHeight,
                    int16_t &outCenterX,
                    int16_t &outCenterY,
                    String &errorOut) {
  outWidth = kImageDefaultWidth;
  outHeight = kImageDefaultHeight;
  outCenterX = kImageDefaultCenterX;
  outCenterY = kImageDefaultCenterY;
  errorOut = "";

  String widthRaw = gWebServer->arg("width");
  if (!widthRaw.length() && gWebServer->hasArg("w")) widthRaw = gWebServer->arg("w");
  widthRaw.trim();
  if (widthRaw.length()) {
    int parsed = 0;
    if (!parseIntString(widthRaw, parsed) || parsed <= 0 || parsed > static_cast<int>(kImageMaxWidth)) {
      errorOut = "width must be 1-320";
      return false;
    }
    outWidth = static_cast<uint16_t>(parsed);
  }

  String heightRaw = gWebServer->arg("height");
  if (!heightRaw.length() && gWebServer->hasArg("h")) heightRaw = gWebServer->arg("h");
  heightRaw.trim();
  if (heightRaw.length()) {
    int parsed = 0;
    if (!parseIntString(heightRaw, parsed) || parsed <= 0 || parsed > static_cast<int>(kImageMaxHeight)) {
      errorOut = "height must be 1-240";
      return false;
    }
    outHeight = static_cast<uint16_t>(parsed);
  }

  String centerXRaw = gWebServer->arg("centerX");
  if (!centerXRaw.length() && gWebServer->hasArg("cx")) centerXRaw = gWebServer->arg("cx");
  centerXRaw.trim();
  if (centerXRaw.length()) {
    int parsed = 0;
    if (!parseIntString(centerXRaw, parsed) || parsed < -1024 || parsed > 1024) {
      errorOut = "centerX must be -1024..1024";
      return false;
    }
    outCenterX = static_cast<int16_t>(parsed);
  }

  String centerYRaw = gWebServer->arg("centerY");
  if (!centerYRaw.length() && gWebServer->hasArg("cy")) centerYRaw = gWebServer->arg("cy");
  centerYRaw.trim();
  if (centerYRaw.length()) {
    int parsed = 0;
    if (!parseIntString(centerYRaw, parsed) || parsed < -1024 || parsed > 1024) {
      errorOut = "centerY must be -1024..1024";
      return false;
    }
    outCenterY = static_cast<int16_t>(parsed);
  }

  return true;
}

void handleImageUpload() {
  if (!gImageMutex) return;

  HTTPUpload &upload = gWebServer->upload();
  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    return;
  }

  if (upload.status == UPLOAD_FILE_START) {
    resetImageUploadStateLocked();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!gImageUpload.failed) {
      const size_t nextSize = gImageUpload.size + upload.currentSize;
      if (nextSize > kImageUploadByteLimit) {
        setImageUploadErrorLocked("image exceeds 1MB");
      } else if (!ensureImageUploadCapacityLocked(nextSize)) {
        setImageUploadErrorLocked("no memory for image");
      } else {
        memcpy(gImageUpload.bytes + gImageUpload.size, upload.buf, upload.currentSize);
        gImageUpload.size = nextSize;
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    setImageUploadErrorLocked("upload aborted");
  }

  xSemaphoreGive(gImageMutex);
}

void handleImageUploadFinalize() {
  if (!gImageMutex) {
    gWebServer->send(500, "text/plain", "image mutex not ready");
    return;
  }

  uint16_t width = kImageDefaultWidth;
  uint16_t height = kImageDefaultHeight;
  int16_t centerX = kImageDefaultCenterX;
  int16_t centerY = kImageDefaultCenterY;
  String argError;
  if (!parseImageArgs(width, height, centerX, centerY, argError)) {
    if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
      resetImageUploadStateLocked();
      xSemaphoreGive(gImageMutex);
    }
    gWebServer->send(400, "text/plain", argError);
    return;
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    gWebServer->send(503, "text/plain", "image buffer busy");
    return;
  }

  const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2U;
  if (gImageUpload.failed) {
    const String err = gImageUpload.error[0] ? String(gImageUpload.error) : String("upload failed");
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", err);
    return;
  }

  if (!gImageUpload.bytes || gImageUpload.size == 0) {
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", "image body is empty");
    return;
  }

  if (gImageUpload.size != expectedBytes) {
    String err = "invalid RGB565 payload bytes: got ";
    err += String(static_cast<unsigned int>(gImageUpload.size));
    err += ", expect ";
    err += String(static_cast<unsigned int>(expectedBytes));
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", err);
    return;
  }

  const size_t pixelCount = expectedBytes / 2U;
  if (!ensurePendingImageCapacityLocked(pixelCount)) {
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(500, "text/plain", "no memory for pending frame");
    return;
  }

  for (size_t i = 0; i < pixelCount; ++i) {
    const size_t b = i * 2U;
    gPendingImage.pixels[i] =
        static_cast<uint16_t>(gImageUpload.bytes[b]) |
        static_cast<uint16_t>(static_cast<uint16_t>(gImageUpload.bytes[b + 1]) << 8);
  }
  gPendingImage.pixelCount = pixelCount;
  gPendingImage.width = width;
  gPendingImage.height = height;
  gPendingImage.centerX = centerX;
  gPendingImage.centerY = centerY;
  gPendingImage.ready = true;

  resetImageUploadStateLocked();
  xSemaphoreGive(gImageMutex);

  String out = "{\"ok\":true,\"width\":";
  out += String(static_cast<unsigned int>(width));
  out += ",\"height\":";
  out += String(static_cast<unsigned int>(height));
  out += ",\"centerX\":";
  out += String(centerX);
  out += ",\"centerY\":";
  out += String(centerY);
  out += "}";
  gWebServer->send(200, "application/json", out);
}

bool forceStaChannel(uint8_t channel) {
  if (channel < kApChannelMin || channel > kApChannelMax) {
    Serial.printf("[ESPNOW] invalid channel: %u\n", static_cast<unsigned int>(channel));
    return false;
  }

  const esp_err_t setRet = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (setRet != ESP_OK) {
    Serial.printf("[ESPNOW] esp_wifi_set_channel failed: %d\n", static_cast<int>(setRet));
    return false;
  }

  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
    Serial.printf("[ESPNOW] STA channel locked to %u\n", static_cast<unsigned int>(primary));
  }
  return true;
}

int gainToPercent(float gain) {
  const float clamped = clampGain(gain);
  return static_cast<int>(clamped * 100.0f + 0.5f);
}

int insertVolumePercent() {
  return gainToPercent(gInsertGain);
}

int bgVolumePercent() {
  return gainToPercent(gBgGain);
}

int masterVolumePercent() {
  return gainToPercent((gInsertGain + gBgGain) * 0.5f);
}

float percentToGain(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return static_cast<float>(percent) / 100.0f;
}

void applyVolumePercents(int insertPercent, int bgPercent) {
  gInsertGain = percentToGain(insertPercent);
  gBgGain = percentToGain(bgPercent);
  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
}

String stripIniValue(String value) {
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0) value = value.substring(0, semicolon);
  const int hash = value.indexOf('#');
  if (hash >= 0) value = value.substring(0, hash);
  value.trim();
  if (value.length() >= 2) {
    const char first = value[0];
    const char last = value[value.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      value = value.substring(1, value.length() - 1);
      value.trim();
    }
  }
  return value;
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '\"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default: {
        const uint8_t uc = static_cast<uint8_t>(c);
        if (uc < 0x20U) {
          char buf[7] = {0};
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(uc));
          escaped += buf;
        } else {
          escaped += c;
        }
        break;
      }
    }
  }
  return escaped;
}

String formatMacString(const uint8_t mac[6]) {
  char buf[18] = {0};
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool parseMacString(const String &text, uint8_t outMac[6]) {
  if (!outMac) return false;
  String s = text;
  s.trim();
  if (!s.length()) return false;

  unsigned int b[6] = {0};
  int n = sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6) {
    n = sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  }
  if (n != 6) return false;

  for (int i = 0; i < 6; ++i) {
    if (b[i] > 0xFFU) return false;
    outMac[i] = static_cast<uint8_t>(b[i]);
  }
  return true;
}

bool applyHostMacFilterSetting(const String &rawMac, bool printOnError) {
  String value = rawMac;
  value.trim();
  if (!value.length()) {
    memset(gHostMacFilter, 0, sizeof(gHostMacFilter));
    gHostMacFilterEnabled = false;
    gHostMacFilterText[0] = '\0';
    return true;
  }

  uint8_t parsed[6] = {0};
  if (!parseMacString(value, parsed)) {
    if (printOnError) {
      Serial.printf("[WEB] invalid HostMAC format: %s\n", value.c_str());
    }
    return false;
  }

  memcpy(gHostMacFilter, parsed, sizeof(gHostMacFilter));
  gHostMacFilterEnabled = true;
  const String fmt = formatMacString(gHostMacFilter);
  copyStringToBuf(fmt, gHostMacFilterText, sizeof(gHostMacFilterText));
  return true;
}

bool isLegacyInstantRefreshSettingKey(const String &key) {
  return key == "instantrefreshnokey" || key == "instantrefresh";
}

void removeLegacyInstantRefreshModeFromSettingIni() {
  if (!fatMounted) return;
  if (!FFat.exists("/setting.ini")) return;

  fs::File rf = FFat.open("/setting.ini", FILE_READ);
  if (!rf) return;
  const String original = rf.readString();
  rf.close();

  bool removed = false;
  String output;
  output.reserve(original.length());

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    const String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    bool dropLine = false;
    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (isLegacyInstantRefreshSettingKey(key)) {
          dropLine = true;
          removed = true;
        }
      }
    }

    if (!dropLine) {
      output += line;
      if (end >= 0) {
        output += '\n';
      }
    }

    if (end >= 0) {
      start = end + 1;
    } else {
      break;
    }
  }

  if (!removed) return;
  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return;
  wf.print(output);
  wf.close();
  Serial.println("[WEB] removed legacy InstantRefreshNoKey from /setting.ini");
}

bool loadInstantRefreshModeFromPreferences(bool fallbackValue, bool &outValue) {
  outValue = fallbackValue;
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, true)) {
    Serial.println("[WEB] Preferences open (RO) failed for instantRefreshNoKey");
    return false;
  }
  outValue = prefs.getBool(kPrefsInstantRefreshNoKey, fallbackValue);
  prefs.end();
  return true;
}

bool persistInstantRefreshModeToPreferences(bool enabled) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return false;
  }
  const bool ok = prefs.putBool(kPrefsInstantRefreshNoKey, enabled) > 0;
  prefs.end();
  return ok;
}

void loadApCredentialsFromSettingIni() {
  copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
  copyStringToBuf(String(kDefaultApPassword), gApPassword, sizeof(gApPassword));
  gApChannel = kDefaultApChannel;
  gEnableAp = true;
  gEnableEspNow = true;
  gInstantRefreshNoKey = false;
  applyHostMacFilterSetting("", false);

  bool persistedMode = gInstantRefreshNoKey;
  if (loadInstantRefreshModeFromPreferences(gInstantRefreshNoKey, persistedMode)) {
    gInstantRefreshNoKey = persistedMode;
  }

  if (!fatMounted) return;
  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) {
    Serial.println("[WEB] /setting.ini not found, using default AP config");
    return;
  }

  bool gotSsid = false;
  bool gotPassword = false;
  bool gotHostMac = false;
  bool gotChannel = false;
  String ssidValue;
  String passwordValue;
  String hostMacValue;
  String channelValue;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (line.startsWith("#") || line.startsWith(";")) continue;

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = stripIniValue(value);

    if (key == "ssip" || key == "ssid") {
      ssidValue = value;
      gotSsid = true;
    } else if (key == "password") {
      passwordValue = value;
      gotPassword = true;
    } else if (key == "hostmac") {
      hostMacValue = value;
      gotHostMac = true;
    } else if (key == "espnowchannel" || key == "channel" || key == "apchannel") {
      channelValue = value;
      gotChannel = true;
    } else if (key == "enableespnow") {
      bool parsed = true;
      if (parseBoolString(value, parsed)) {
        gEnableEspNow = parsed;
      }
    }
  }
  f.close();
  removeLegacyInstantRefreshModeFromSettingIni();

  if (gotSsid) {
    if (ssidValue.length()) {
      copyStringToBuf(ssidValue, gApSsid, sizeof(gApSsid));
    } else {
      copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
    }
  }

  if (gotPassword) {
    if (!passwordValue.length()) {
      gApPassword[0] = '\0';
    } else if (passwordValue.length() >= 8) {
      copyStringToBuf(passwordValue, gApPassword, sizeof(gApPassword));
    } else {
      gApPassword[0] = '\0';
      Serial.println("[WEB] password length < 8, fallback to open AP");
    }
  }

  if (gotHostMac) {
    if (!applyHostMacFilterSetting(hostMacValue, true)) {
      applyHostMacFilterSetting("", false);
    }
  }

  if (gotChannel) {
    uint8_t parsed = kDefaultApChannel;
    if (parseApChannel(channelValue, parsed)) {
      gApChannel = parsed;
    } else {
      gApChannel = kDefaultApChannel;
      Serial.printf("[WEB] invalid channel in /setting.ini: %s, fallback=%u\n",
                    channelValue.c_str(),
                    static_cast<unsigned int>(gApChannel));
    }
  }
}

bool persistHostMacToSettingIni(const String &hostMacRaw) {
  if (!fatMounted) return false;

  String hostMac = hostMacRaw;
  hostMac.trim();
  if (hostMac.length()) {
    uint8_t mac[6] = {0};
    if (!parseMacString(hostMac, mac)) return false;
    hostMac = formatMacString(mac);
  }

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundHostMac = false;
  String output;
  output.reserve(original.length() + 48);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "hostmac") {
          line = "HostMAC = \"" + hostMac + "\";";
          foundHostMac = true;
        }
      }
    }

    output += line;
    if (end >= 0) {
      output += '\n';
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundHostMac) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "HostMAC = \"" + hostMac + "\";\n";
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

bool persistApConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, uint8_t channel) {
  if (!fatMounted) return false;

  String ssid = ssidRaw;
  String password = passwordRaw;
  ssid.trim();
  password.trim();
  channel = clampApChannel(channel);

  if (!ssid.length()) return false;
  if (ssid.length() >= sizeof(gApSsid)) return false;
  if (password.length() && (password.length() < 8 || password.length() > 63)) return false;

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundChannel = false;
  String output;
  output.reserve(original.length() + 128);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);
    bool keepLine = true;

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "ssip" || key == "ssid") {
          line = "SSID = \"" + ssid + "\";";
          foundSsid = true;
        } else if (key == "password") {
          line = "Password = \"" + password + "\";";
          foundPassword = true;
        } else if (key == "espnowchannel" || key == "channel" || key == "apchannel") {
          line = "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";";
          foundChannel = true;
        } else if (key == "enableap") {
          // EnableAP is no longer a setting.ini control item.
          // AP enablement is decided by current app mode.
          keepLine = false;
        }
      }
    }

    if (keepLine) {
      output += line;
      if (end >= 0) {
        output += '\n';
      }
    }
    if (end >= 0) {
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundSsid) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "SSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "Password = \"" + password + "\";\n";
  }
  if (!foundChannel) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";\n";
  }
  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
  if (!gHostMessageQueue) return;
  if (!data || dataLen < static_cast<int>(sizeof(EspNowTextPacket))) return;

  if (gHostMacFilterEnabled) {
    if (!macAddr || memcmp(macAddr, gHostMacFilter, 6) != 0) {
      static uint32_t lastFilterDropLogMs = 0;
      const uint32_t now = millis();
      if (now - lastFilterDropLogMs >= 2000U) {
        lastFilterDropLogMs = now;
        String srcText = "<NULL>";
        if (macAddr) srcText = formatMacString(macAddr);
        Serial.printf("[ESPNOW] drop by HostMAC filter src=%s expect=%s\n",
                      srcText.c_str(),
                      gHostMacFilterText);
      }
      return;
    }
  }

  const EspNowTextPacket *pkt = reinterpret_cast<const EspNowTextPacket *>(data);
  if (pkt->magic != kEspNowTextMagic || pkt->type != kEspNowMsgTypeText) return;

  WebQueuedMessage msg = {};
  msg.text[0] = '\0';
  const size_t maxCopy = sizeof(msg.text) - 1;
  size_t n = strnlen(pkt->text, kEspNowTextMaxBytes);
  if (n > maxCopy) n = maxCopy;
  memcpy(msg.text, pkt->text, n);
  msg.text[n] = '\0';
  if (!msg.text[0]) return;

  if (xQueueSend(gHostMessageQueue, &msg, 0) != pdTRUE) {
    WebQueuedMessage drop = {};
    (void)xQueueReceive(gHostMessageQueue, &drop, 0);
    (void)xQueueSend(gHostMessageQueue, &msg, 0);
  }
}

bool initEspNowReceiver() {
  if (gEspNowReady) return true;
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    return false;
  }
  if (esp_now_register_recv_cb(onEspNowRecv) != ESP_OK) {
    Serial.println("[ESPNOW] register recv callback failed");
    esp_now_deinit();
    return false;
  }
  gEspNowReady = true;
  Serial.println("[ESPNOW] receiver ready");
  return true;
}

void deinitEspNowReceiver() {
  if (!gEspNowReady) return;
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  gEspNowReady = false;
}


void setCsvReloadRequested() {
  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = true;
  portEXIT_CRITICAL(&gFlagMux);
}

void setScheduleReloadRequested() {
  portENTER_CRITICAL(&gFlagMux);
  gScheduleReloadRequested = true;
  portEXIT_CRITICAL(&gFlagMux);
}

bool takeCsvReloadRequested() {
  bool value = false;
  portENTER_CRITICAL(&gFlagMux);
  value = gCsvReloadRequested;
  gCsvReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);
  return value;
}

bool takeScheduleReloadRequested() {
  bool value = false;
  portENTER_CRITICAL(&gFlagMux);
  value = gScheduleReloadRequested;
  gScheduleReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);
  return value;
}

String readDataCsvText() {
  if (!fatMounted) return "";
  fs::File f = FFat.open("/data.csv", FILE_READ);
  if (!f) return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveDataCsvText(const String &content) {
  if (!fatMounted) return false;
  fs::File f = FFat.open("/data.csv", "w");
  if (!f) return false;
  const size_t written = f.print(content);
  f.close();
  if (written != content.length()) return false;
  setCsvReloadRequested();
  return true;
}

String readScheduleCsvText() {
  if (!fatMounted) return "";
  fs::File f = FFat.open("/schedule.csv", FILE_READ);
  if (!f) return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveScheduleCsvText(const String &content) {
  if (!fatMounted) return false;
  fs::File f = FFat.open("/schedule.csv", "w");
  if (!f) return false;
  const size_t written = f.print(content);
  f.close();
  if (written != content.length()) return false;
  setScheduleReloadRequested();
  return true;
}

void loadStaConfigFromSettingIni(String &outSsid, String &outPassword, String &outNet) {
  outSsid = "";
  outPassword = "";
  outNet = "";
  if (!fatMounted) return;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (line.startsWith("#") || line.startsWith(";")) continue;

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = stripIniValue(value);

    if (key == "netssid") {
      outSsid = value;
    } else if (key == "netpassword") {
      outPassword = value;
    } else if (key == "net") {
      outNet = value;
    }
  }
  f.close();
}

bool persistStaConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, const String &netRaw) {
  if (!fatMounted) return false;

  String ssid = ssidRaw;
  String password = passwordRaw;
  String net = netRaw;
  ssid.trim();
  password.trim();
  net.trim();

  if (!ssid.length()) return false;
  if (ssid.length() > 32) return false;
  if (password.length() && (password.length() < 8 || password.length() > 63)) return false;
  if (net.length()) {
    String netLower = net;
    netLower.toLowerCase();
    if (!netLower.startsWith("http://") && !netLower.startsWith("https://")) return false;
  }

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundNet = false;
  String output;
  output.reserve(original.length() + 192);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "netssid") {
          line = "NetSSID = \"" + ssid + "\";";
          foundSsid = true;
        } else if (key == "netpassword") {
          line = "NetPassword = \"" + password + "\";";
          foundPassword = true;
        } else if (key == "net") {
          line = "Net = \"" + net + "\";";
          foundNet = true;
        }
      }
    }

    output += line;
    if (end >= 0) {
      output += '\n';
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundSsid) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "NetSSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "NetPassword = \"" + password + "\";\n";
  }
  if (!foundNet) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "Net = \"" + net + "\";\n";
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

String rtcDateTimeJson() {
  Ds1302DateTime dt;
  if (!rtc.readDateTime(dt) || !ds1302IsValidDateTime(dt)) {
    return "{\"ok\":false}";
  }

  int week = static_cast<int>((daysFromCivil(static_cast<int>(dt.year),
                                             static_cast<unsigned>(dt.month),
                                             static_cast<unsigned>(dt.day)) +
                               3LL) %
                              7LL);
  if (week < 0) week += 7;
  week += 1;
  String out = "{\"ok\":true,\"year\":";
  out += String(static_cast<unsigned int>(dt.year));
  out += ",\"month\":";
  out += String(static_cast<unsigned int>(dt.month));
  out += ",\"day\":";
  out += String(static_cast<unsigned int>(dt.day));
  out += ",\"hour\":";
  out += String(static_cast<unsigned int>(dt.hour));
  out += ",\"minute\":";
  out += String(static_cast<unsigned int>(dt.minute));
  out += ",\"second\":";
  out += String(static_cast<unsigned int>(dt.second));
  out += ",\"week\":";
  out += String(static_cast<unsigned int>(week));
  out += "}";
  return out;
}

bool parseRtcDateTimeFromRequest(Ds1302DateTime &out, String &errorOut) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  const String yearRaw = gWebServer->arg("year");
  const String monthRaw = gWebServer->arg("month");
  const String dayRaw = gWebServer->arg("day");
  const String hourRaw = gWebServer->arg("hour");
  const String minuteRaw = gWebServer->arg("minute");
  const String secondRaw = gWebServer->arg("second");
  if (!parseIntString(yearRaw, year) || year < 2000 || year > 2099) {
    errorOut = "invalid year";
    return false;
  }
  if (!parseIntString(monthRaw, month) || month < 1 || month > 12) {
    errorOut = "invalid month";
    return false;
  }
  if (!parseIntString(dayRaw, day) || day < 1 || day > 31) {
    errorOut = "invalid day";
    return false;
  }
  if (!parseIntString(hourRaw, hour) || hour < 0 || hour > 23) {
    errorOut = "invalid hour";
    return false;
  }
  if (!parseIntString(minuteRaw, minute) || minute < 0 || minute > 59) {
    errorOut = "invalid minute";
    return false;
  }
  if (!parseIntString(secondRaw, second) || second < 0 || second > 59) {
    errorOut = "invalid second";
    return false;
  }

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(hour);
  out.minute = static_cast<uint8_t>(minute);
  out.second = static_cast<uint8_t>(second);
  if (!ds1302IsValidDateTime(out)) {
    errorOut = "datetime out of range";
    return false;
  }
  return true;
}

bool enqueueMessageToQueue(QueueHandle_t queue, const String &textRaw, String &errorOut) {
  if (!queue) {
    errorOut = "queue not ready";
    return false;
  }

  String text = textRaw;
  text.trim();
  if (!text.length()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen) text.remove(kTextMaxLen);

  WebQueuedMessage msg = {};
  text.toCharArray(msg.text, sizeof(msg.text));

  if (xQueueSend(queue, &msg, 0) != pdTRUE) {
    errorOut = "queue is full";
    return false;
  }
  return true;
}

bool enqueueRegularMessage(const String &textRaw, String &errorOut) {
  return enqueueMessageToQueue(gMessageQueue, textRaw, errorOut);
}

bool enqueueImmediateMessage(const String &textRaw, String &errorOut) {
  String text = textRaw;
  text.trim();
  if (!text.length()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen) text.remove(kTextMaxLen);

  char local[kTextMaxLen + 1] = {0};
  text.toCharArray(local, sizeof(local));

  portENTER_CRITICAL(&gImmediateMessageMux);
  memcpy(gImmediateMessage.text, local, sizeof(gImmediateMessage.text));
  gImmediateMessage.text[sizeof(gImmediateMessage.text) - 1] = '\0';
  gImmediateMessage.pending = true;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  return true;
}

UBaseType_t immediatePendingCount() {
  bool pending = false;
  portENTER_CRITICAL(&gImmediateMessageMux);
  pending = gImmediateMessage.pending;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  return pending ? 1U : 0U;
}

bool persistAudioGainsToSettingIni() {
  if (!fatMounted) return false;

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundInsert = false;
  bool foundBg = false;
  String output;
  output.reserve(original.length() + 96);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "insertgain") {
          line = "InsertGain = " + String(gInsertGain, 3) + ";";
          foundInsert = true;
        } else if (key == "backgroundgain") {
          line = "BackGroundGain = " + String(gBgGain, 3) + ";";
          foundBg = true;
        }
      }
    }

    output += line;
    if (end >= 0) {
      output += '\n';
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundInsert) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "InsertGain = " + String(gInsertGain, 3) + ";\n";
  }
  if (!foundBg) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "BackGroundGain = " + String(gBgGain, 3) + ";\n";
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

bool persistEffectSettingsToSettingIni(int wrongProb3,
                                       int wrongProb5,
                                       bool enableReprint,
                                       float backlightLevel,
                                       int backlightTimeSec,
                                       int backlightCloseTimeSec) {
  if (!fatMounted) return false;

  wrongProb3 = constrain(wrongProb3, 0, 100);
  wrongProb5 = constrain(wrongProb5, 0, 100);
  backlightLevel = clampGain(backlightLevel);
  if (backlightCloseTimeSec < 0) backlightCloseTimeSec = 0;

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundWrong3 = false;
  bool foundWrong5 = false;
  bool foundReprint = false;
  bool foundBacklight = false;
  bool foundBacklightTime = false;
  bool foundBacklightCloseTime = false;
  String output;
  output.reserve(original.length() + 200);

  int start = 0;
  while (start <= original.length()) {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";")) {
      const int eq = trimmed.indexOf('=');
      if (eq > 0) {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "testwrongindexpersent_3area") {
          line = "TestWrongIndexPersent_3Area = " + String(wrongProb3) + ";";
          foundWrong3 = true;
        } else if (key == "testwrongindexpersent_5area") {
          line = "TestWrongIndexPersent_5Area = " + String(wrongProb5) + ";";
          foundWrong5 = true;
        } else if (key == "enablereprint") {
          line = String("EnableReprint = ") + (enableReprint ? "true;" : "false;");
          foundReprint = true;
        } else if (key == "backlight") {
          line = "BackLight = " + String(backlightLevel, 3) + ";";
          foundBacklight = true;
        } else if (key == "backlighttime") {
          line = "BacklightTime = " + String(backlightTimeSec) + ";";
          foundBacklightTime = true;
        } else if (key == "backlightclosetime") {
          line = "BacklightCloseTime = " + String(backlightCloseTimeSec) + ";";
          foundBacklightCloseTime = true;
        }
      }
    }

    output += line;
    if (end >= 0) {
      output += '\n';
      start = end + 1;
    } else {
      break;
    }
  }

  if (!foundWrong3) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "TestWrongIndexPersent_3Area = " + String(wrongProb3) + ";\n";
  }
  if (!foundWrong5) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "TestWrongIndexPersent_5Area = " + String(wrongProb5) + ";\n";
  }
  if (!foundReprint) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += String("EnableReprint = ") + (enableReprint ? "true;\n" : "false;\n");
  }
  if (!foundBacklight) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "BackLight = " + String(backlightLevel, 3) + ";\n";
  }
  if (!foundBacklightTime) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "BacklightTime = " + String(backlightTimeSec) + ";\n";
  }
  if (!foundBacklightCloseTime) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += "BacklightCloseTime = " + String(backlightCloseTimeSec) + ";\n";
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

String statusJson() {
  const String staMac = WiFi.macAddress();
  const String apMac = WiFi.softAPmacAddress();
  const UBaseType_t regularQueued = gMessageQueue ? uxQueueMessagesWaiting(gMessageQueue) : 0;
  const UBaseType_t immediateQueued = immediatePendingCount();
  const UBaseType_t queued = regularQueued + immediateQueued;
  const UBaseType_t hostQueued = gHostMessageQueue ? uxQueueMessagesWaiting(gHostMessageQueue) : 0;
  bool pendingReload = false;
  bool imagePending = false;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
  int16_t imageCenterX = kImageDefaultCenterX;
  int16_t imageCenterY = kImageDefaultCenterY;
  portENTER_CRITICAL(&gFlagMux);
  pendingReload = gCsvReloadRequested;
  portEXIT_CRITICAL(&gFlagMux);
  if (gImageMutex && xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    imagePending = gPendingImage.ready;
    imageWidth = gPendingImage.width;
    imageHeight = gPendingImage.height;
    imageCenterX = gPendingImage.centerX;
    imageCenterY = gPendingImage.centerY;
    xSemaphoreGive(gImageMutex);
  }

  String out = "{\"queue\":";
  out += String(static_cast<unsigned int>(queued));
  out += ",\"regularQueue\":";
  out += String(static_cast<unsigned int>(regularQueued));
  out += ",\"immediateQueue\":";
  out += String(static_cast<unsigned int>(immediateQueued));
  out += ",\"csvReloadPending\":";
  out += pendingReload ? "true" : "false";
  out += ",\"hostQueue\":";
  out += String(static_cast<unsigned int>(hostQueued));
  out += ",\"volume\":";
  out += String(masterVolumePercent());
  out += ",\"insertVolume\":";
  out += String(insertVolumePercent());
  out += ",\"bgVolume\":";
  out += String(bgVolumePercent());
  out += ",\"instantRefreshNoKey\":";
  out += gInstantRefreshNoKey ? "true" : "false";
  out += ",\"hostMac\":\"";
  out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
  out += "\"";
  out += ",\"apSsid\":\"";
  out += jsonEscape(String(gApSsid));
  out += "\",\"apChannel\":";
  out += String(static_cast<unsigned int>(gApChannel));
  out += ",\"apPasswordSet\":";
  out += gApPassword[0] ? "true" : "false";
  out += ",\"enableAp\":";
  out += gEnableAp ? "true" : "false";
  out += ",\"enableEspNow\":";
  out += gEnableEspNow ? "true" : "false";
  out += ",\"wrongProb3\":";
  out += String(gWrongProb3);
  out += ",\"wrongProb5\":";
  out += String(gWrongProb5);
  out += ",\"enableReprint\":";
  out += gEnableReprint ? "true" : "false";
  out += ",\"backlight\":";
  out += String(clampGain(gBacklightLevel), 3);
  out += ",\"backlightTime\":";
  out += String(gBacklightTimeSec);
  out += ",\"backlightCloseTime\":";
  out += String(gBacklightCloseTimeSec);
  out += ",\"imagePending\":";
  out += imagePending ? "true" : "false";
  out += ",\"imageWidth\":";
  out += String(static_cast<unsigned int>(imageWidth));
  out += ",\"imageHeight\":";
  out += String(static_cast<unsigned int>(imageHeight));
  out += ",\"imageCenterX\":";
  out += String(imageCenterX);
  out += ",\"imageCenterY\":";
  out += String(imageCenterY);
  out += ",\"selfMac\":\"";
  // Keep selfMac stable for ESP-NOW targeting: always use STA MAC.
  out += jsonEscape(staMac);
  out += "\"";
  out += ",\"selfStaMac\":\"";
  out += jsonEscape(staMac);
  out += "\"";
  out += ",\"selfApMac\":\"";
  out += jsonEscape(apMac);
  out += "\"";
  out += "}";
  return out;
}

String apRootUrl() {
  String url = "http://";
  url += WiFi.softAPIP().toString();
  url += "/";
  return url;
}

void redirectToPortal() {
  gWebServer->sendHeader("Location", apRootUrl(), true);
  gWebServer->send(302, "text/plain", "");
}

String detectContentType(const String &path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".txt")) return "text/plain; charset=utf-8";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

bool streamFromLittleFs(const String &path) {
  if (!LittleFS.exists(path)) return false;
  fs::File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  gWebServer->sendHeader("Cache-Control", "no-store");
  gWebServer->streamFile(f, detectContentType(path));
  f.close();
  return true;
}

void handleStaticFile(const String &path) {
  if (!streamFromLittleFs(path)) {
    gWebServer->send(404, "text/plain", "file not found");
  }
}

void registerRoutes() {
  gWebServer->on("/", HTTP_GET, []() {
    if (!streamFromLittleFs("/index.html")) {
      gWebServer->send(500, "text/plain", "missing /index.html in LittleFS");
    }
  });

  gWebServer->on("/index.html", HTTP_GET, []() {
    handleStaticFile("/index.html");
  });
  gWebServer->on("/style.css", HTTP_GET, []() {
    handleStaticFile("/style.css");
  });
  gWebServer->on("/app.js", HTTP_GET, []() {
    handleStaticFile("/app.js");
  });
  gWebServer->on("/favicon.ico", HTTP_GET, []() {
    gWebServer->send(204, "text/plain", "");
  });
  gWebServer->on("/apple-touch-icon.png", HTTP_GET, []() {
    gWebServer->send(204, "text/plain", "");
  });

  gWebServer->on("/api/status", HTTP_GET, []() {
    gWebServer->send(200, "application/json", statusJson());
  });

  gWebServer->on("/api/rtc", HTTP_GET, []() {
    gWebServer->send(410, "application/json", "{\"ok\":false,\"message\":\"rtc disabled\"}");
  });

  gWebServer->on("/api/rtc/set", HTTP_POST, []() {
    gWebServer->send(410, "text/plain", "rtc disabled");
  });

  gWebServer->on("/api/rtc/sync-phone", HTTP_POST, []() {
    gWebServer->send(410, "text/plain", "rtc disabled");
  });

  gWebServer->on("/api/battery", HTTP_GET, []() {
    gWebServer->send(200, "application/json", "{\"ok\":false,\"message\":\"battery sensor not configured\"}");
  });

  gWebServer->on("/api/volume", HTTP_GET, []() {
    String out = "{\"insertVolume\":";
    out += String(insertVolumePercent());
    out += ",\"bgVolume\":";
    out += String(bgVolumePercent());
    out += ",\"volume\":";
    out += String(masterVolumePercent());
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/volume", HTTP_POST, []() {
    String insertRaw = gWebServer->arg("insertVolume");
    if (!insertRaw.length() && gWebServer->hasArg("insert")) {
      insertRaw = gWebServer->arg("insert");
    }

    String bgRaw = gWebServer->arg("bgVolume");
    if (!bgRaw.length() && gWebServer->hasArg("backgroundVolume")) {
      bgRaw = gWebServer->arg("backgroundVolume");
    }

    String value = gWebServer->arg("volume");
    String persistArg = gWebServer->arg("persist");
    if (!value.length() && !insertRaw.length() && !bgRaw.length() && gWebServer->hasArg("plain")) {
      value = gWebServer->arg("plain");
    }

    bool persist = true;
    persistArg.trim();
    persistArg.toLowerCase();
    if (persistArg == "0" || persistArg == "false" || persistArg == "off" || persistArg == "no") {
      persist = false;
    }

    bool hasAny = false;
    int nextInsert = insertVolumePercent();
    int nextBg = bgVolumePercent();

    insertRaw.trim();
    if (insertRaw.length()) {
      int parsed = 0;
      if (!parseIntString(insertRaw, parsed) || parsed < 0 || parsed > 100) {
        gWebServer->send(400, "text/plain", "insertVolume must be 0-100");
        return;
      }
      nextInsert = parsed;
      hasAny = true;
    }

    bgRaw.trim();
    if (bgRaw.length()) {
      int parsed = 0;
      if (!parseIntString(bgRaw, parsed) || parsed < 0 || parsed > 100) {
        gWebServer->send(400, "text/plain", "bgVolume must be 0-100");
        return;
      }
      nextBg = parsed;
      hasAny = true;
    }

    value.trim();
    if (value.length()) {
      int parsed = 0;
      if (!parseIntString(value, parsed) || parsed < 0 || parsed > 100) {
        gWebServer->send(400, "text/plain", "volume must be 0-100");
        return;
      }
      if (!insertRaw.length()) nextInsert = parsed;
      if (!bgRaw.length()) nextBg = parsed;
      hasAny = true;
    }

    if (!hasAny) {
      gWebServer->send(400, "text/plain", "volume is empty");
      return;
    }

    applyVolumePercents(nextInsert, nextBg);
    if (persist && !persistAudioGainsToSettingIni()) {
      gWebServer->send(500, "text/plain", "volume applied but save /setting.ini failed");
      return;
    }

    String out = "{\"insertVolume\":";
    out += String(insertVolumePercent());
    out += ",\"bgVolume\":";
    out += String(bgVolumePercent());
    out += ",\"volume\":";
    out += String(masterVolumePercent());
    out += ",\"persisted\":";
    out += persist ? "true" : "false";
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/refreshmode", HTTP_GET, []() {
    String out = "{\"instantRefreshNoKey\":";
    out += gInstantRefreshNoKey ? "true" : "false";
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/refreshmode", HTTP_POST, []() {
    String raw = gWebServer->arg("instantRefreshNoKey");
    if (!raw.length() && gWebServer->hasArg("instantrefreshnokey")) {
      raw = gWebServer->arg("instantrefreshnokey");
    }
    if (!raw.length() && gWebServer->hasArg("instantRefresh")) {
      raw = gWebServer->arg("instantRefresh");
    }
    if (!raw.length() && gWebServer->hasArg("plain")) {
      raw = gWebServer->arg("plain");
    }
    raw.trim();

    bool next = gInstantRefreshNoKey;
    if (!parseBoolString(raw, next)) {
      gWebServer->send(400, "text/plain", "invalid instantRefreshNoKey");
      return;
    }

    gInstantRefreshNoKey = next;
    if (!persistInstantRefreshModeToPreferences(gInstantRefreshNoKey)) {
      gWebServer->send(500, "text/plain", "mode applied but save Preferences failed");
      return;
    }

    String out = "{\"instantRefreshNoKey\":";
    out += gInstantRefreshNoKey ? "true" : "false";
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/effects", HTTP_GET, []() {
    String out = "{\"wrongProb3\":";
    out += String(gWrongProb3);
    out += ",\"wrongProb5\":";
    out += String(gWrongProb5);
    out += ",\"enableReprint\":";
    out += gEnableReprint ? "true" : "false";
    out += ",\"backlight\":";
    out += String(clampGain(gBacklightLevel), 3);
    out += ",\"backlightTime\":";
    out += String(gBacklightTimeSec);
    out += ",\"backlightCloseTime\":";
    out += String(gBacklightCloseTimeSec);
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/effects", HTTP_POST, []() {
    int nextWrong3 = gWrongProb3;
    int nextWrong5 = gWrongProb5;
    bool nextEnableReprint = gEnableReprint;
    float nextBacklightLevel = gBacklightLevel;
    int nextBacklightTime = gBacklightTimeSec;
    int nextBacklightCloseTime = gBacklightCloseTimeSec;
    bool hasAny = false;

    String wrong3Raw = gWebServer->arg("wrongProb3");
    if (!wrong3Raw.length() && gWebServer->hasArg("TestWrongIndexPersent_3Area")) {
      wrong3Raw = gWebServer->arg("TestWrongIndexPersent_3Area");
    }
    wrong3Raw.trim();
    if (wrong3Raw.length()) {
      int parsed = 0;
      if (!parseIntString(wrong3Raw, parsed) || parsed < 0 || parsed > 100) {
        gWebServer->send(400, "text/plain", "wrongProb3 must be 0-100");
        return;
      }
      nextWrong3 = parsed;
      hasAny = true;
    }

    String wrong5Raw = gWebServer->arg("wrongProb5");
    if (!wrong5Raw.length() && gWebServer->hasArg("TestWrongIndexPersent_5Area")) {
      wrong5Raw = gWebServer->arg("TestWrongIndexPersent_5Area");
    }
    wrong5Raw.trim();
    if (wrong5Raw.length()) {
      int parsed = 0;
      if (!parseIntString(wrong5Raw, parsed) || parsed < 0 || parsed > 100) {
        gWebServer->send(400, "text/plain", "wrongProb5 must be 0-100");
        return;
      }
      nextWrong5 = parsed;
      hasAny = true;
    }

    String reprintRaw = gWebServer->arg("enableReprint");
    if (!reprintRaw.length() && gWebServer->hasArg("EnableReprint")) {
      reprintRaw = gWebServer->arg("EnableReprint");
    }
    reprintRaw.trim();
    if (reprintRaw.length()) {
      bool parsed = false;
      if (!parseBoolString(reprintRaw, parsed)) {
        gWebServer->send(400, "text/plain", "invalid enableReprint");
        return;
      }
      nextEnableReprint = parsed;
      hasAny = true;
    }

    String backlightRaw = gWebServer->arg("backlightTime");
    if (!backlightRaw.length() && gWebServer->hasArg("BacklightTime")) {
      backlightRaw = gWebServer->arg("BacklightTime");
    }
    backlightRaw.trim();
    if (backlightRaw.length()) {
      int parsed = 0;
      if (!parseIntString(backlightRaw, parsed)) {
        gWebServer->send(400, "text/plain", "invalid backlightTime");
        return;
      }
      nextBacklightTime = parsed;
      hasAny = true;
    }

    String backlightCloseRaw = gWebServer->arg("backlightCloseTime");
    if (!backlightCloseRaw.length() && gWebServer->hasArg("BacklightCloseTime")) {
      backlightCloseRaw = gWebServer->arg("BacklightCloseTime");
    }
    backlightCloseRaw.trim();
    if (backlightCloseRaw.length()) {
      int parsed = 0;
      if (!parseIntString(backlightCloseRaw, parsed) || parsed < 0) {
        gWebServer->send(400, "text/plain", "invalid backlightCloseTime");
        return;
      }
      nextBacklightCloseTime = parsed;
      hasAny = true;
    }

    String backlightLevelRaw = gWebServer->arg("backlight");
    if (!backlightLevelRaw.length() && gWebServer->hasArg("BackLight")) {
      backlightLevelRaw = gWebServer->arg("BackLight");
    }
    backlightLevelRaw.trim();
    if (backlightLevelRaw.length()) {
      float parsed = 0.0f;
      if (!parseFloatString(backlightLevelRaw, parsed) || parsed < 0.0f || parsed > 1.0f) {
        gWebServer->send(400, "text/plain", "backlight must be 0.0-1.0");
        return;
      }
      nextBacklightLevel = parsed;
      hasAny = true;
    }

    if (!hasAny) {
      gWebServer->send(400, "text/plain", "no effect settings provided");
      return;
    }

    gWrongProb3 = nextWrong3;
    gWrongProb5 = nextWrong5;
    gEnableReprint = nextEnableReprint;
    setBacklightLevel(nextBacklightLevel);
    setBacklightTimeSeconds(nextBacklightTime);
    gBacklightCloseTimeSec = nextBacklightCloseTime;

    if (!persistEffectSettingsToSettingIni(gWrongProb3,
                                           gWrongProb5,
                                           gEnableReprint,
                                           gBacklightLevel,
                                           gBacklightTimeSec,
                                           gBacklightCloseTimeSec)) {
      gWebServer->send(500, "text/plain", "settings applied but save /setting.ini failed");
      return;
    }

    String out = "{\"wrongProb3\":";
    out += String(gWrongProb3);
    out += ",\"wrongProb5\":";
    out += String(gWrongProb5);
    out += ",\"enableReprint\":";
    out += gEnableReprint ? "true" : "false";
    out += ",\"backlight\":";
    out += String(clampGain(gBacklightLevel), 3);
    out += ",\"backlightTime\":";
    out += String(gBacklightTimeSec);
    out += ",\"backlightCloseTime\":";
    out += String(gBacklightCloseTimeSec);
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/hostmac", HTTP_GET, []() {
    String out = "{\"hostMac\":\"";
    out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
    out += "\",\"enabled\":";
    out += gHostMacFilterEnabled ? "true" : "false";
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/hostmac", HTTP_POST, []() {
    String hostMac = gWebServer->arg("hostMac");
    if (!hostMac.length() && gWebServer->hasArg("hostmac")) {
      hostMac = gWebServer->arg("hostmac");
    }
    if (!hostMac.length() && gWebServer->hasArg("plain")) {
      hostMac = gWebServer->arg("plain");
    }
    hostMac.trim();

    if (!applyHostMacFilterSetting(hostMac, false)) {
      gWebServer->send(400, "text/plain", "invalid hostMac format");
      return;
    }
    if (!persistHostMacToSettingIni(hostMac)) {
      gWebServer->send(500, "text/plain", "hostMac applied but save /setting.ini failed");
      return;
    }

    String out = "{\"hostMac\":\"";
    out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
    out += "\",\"enabled\":";
    out += gHostMacFilterEnabled ? "true" : "false";
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/apconfig", HTTP_GET, []() {
    String out = "{\"ssid\":\"";
    out += jsonEscape(String(gApSsid));
    out += "\",\"password\":\"";
    out += jsonEscape(String(gApPassword));
    out += "\",\"passwordSet\":";
    out += gApPassword[0] ? "true" : "false";
    out += ",\"channel\":";
    out += String(static_cast<unsigned int>(gApChannel));
    out += "}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/apconfig", HTTP_POST, []() {
    String ssid = gWebServer->arg("ssid");
    if (!ssid.length() && gWebServer->hasArg("ssip")) {
      ssid = gWebServer->arg("ssip");
    }
    if (!ssid.length() && gWebServer->hasArg("plain")) {
      ssid = gWebServer->arg("plain");
    }

    String password = gWebServer->arg("password");
    String channelArg = gWebServer->arg("channel");

    ssid.trim();
    password.trim();
    channelArg.trim();

    if (!ssid.length()) {
      gWebServer->send(400, "text/plain", "ssid is empty");
      return;
    }
    if (ssid.length() >= sizeof(gApSsid)) {
      gWebServer->send(400, "text/plain", "ssid too long");
      return;
    }
    if (password.length() && (password.length() < 8 || password.length() > 63)) {
      gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
      return;
    }

    uint8_t nextChannel = gApChannel;
    if (channelArg.length() && !parseApChannel(channelArg, nextChannel)) {
      gWebServer->send(400, "text/plain", "channel must be 1-13");
      return;
    }

    if (!persistApConfigToSettingIni(ssid, password, nextChannel)) {
      gWebServer->send(500, "text/plain", "save /setting.ini failed");
      return;
    }

    copyStringToBuf(ssid, gApSsid, sizeof(gApSsid));
    if (!password.length()) {
      gApPassword[0] = '\0';
    } else {
      copyStringToBuf(password, gApPassword, sizeof(gApPassword));
    }
    gApChannel = nextChannel;

    String out = "{\"ssid\":\"";
    out += jsonEscape(String(gApSsid));
    out += "\",\"password\":\"";
    out += jsonEscape(String(gApPassword));
    out += "\",\"passwordSet\":";
    out += gApPassword[0] ? "true" : "false";
    out += ",\"channel\":";
    out += String(static_cast<unsigned int>(gApChannel));
    out += ",\"rebootRequired\":true}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/staconfig", HTTP_GET, []() {
    String staSsid;
    String staPassword;
    String staNet;
    loadStaConfigFromSettingIni(staSsid, staPassword, staNet);

    String out = "{\"ssid\":\"";
    out += jsonEscape(staSsid);
    out += "\",\"password\":\"";
    out += jsonEscape(staPassword);
    out += "\",\"passwordSet\":";
    out += staPassword.length() ? "true" : "false";
    out += ",\"net\":\"";
    out += jsonEscape(staNet);
    out += "\"}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/staconfig", HTTP_POST, []() {
    String ssid = gWebServer->arg("ssid");
    String password = gWebServer->arg("password");
    const bool hasNetArg = gWebServer->hasArg("net") || gWebServer->hasArg("Net");
    String net = gWebServer->arg("net");
    if (!net.length() && gWebServer->hasArg("Net")) {
      net = gWebServer->arg("Net");
    }
    if (!ssid.length() && gWebServer->hasArg("plain")) {
      ssid = gWebServer->arg("plain");
    }
    ssid.trim();
    password.trim();
    net.trim();
    if (!hasNetArg) {
      String currentSsid;
      String currentPassword;
      String currentNet;
      loadStaConfigFromSettingIni(currentSsid, currentPassword, currentNet);
      net = currentNet;
    }

    if (!ssid.length()) {
      gWebServer->send(400, "text/plain", "ssid is empty");
      return;
    }
    if (ssid.length() > 32) {
      gWebServer->send(400, "text/plain", "ssid too long");
      return;
    }
    if (password.length() && (password.length() < 8 || password.length() > 63)) {
      gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
      return;
    }
    if (net.length()) {
      String netLower = net;
      netLower.toLowerCase();
      if (!netLower.startsWith("http://") && !netLower.startsWith("https://")) {
        gWebServer->send(400, "text/plain", "net must start with http:// or https://");
        return;
      }
    }
    if (!persistStaConfigToSettingIni(ssid, password, net)) {
      gWebServer->send(500, "text/plain", "save /setting.ini failed");
      return;
    }

    String out = "{\"ssid\":\"";
    out += jsonEscape(ssid);
    out += "\",\"password\":\"";
    out += jsonEscape(password);
    out += "\",\"passwordSet\":";
    out += password.length() ? "true" : "false";
    out += ",\"net\":\"";
    out += jsonEscape(net);
    out += "\"}";
    gWebServer->send(200, "application/json", out);
  });

  gWebServer->on("/api/csv", HTTP_GET, []() {
    if (!fatMounted) {
      gWebServer->send(503, "text/plain", "FAT not mounted");
      return;
    }
    const String csvText = readDataCsvText();
    gWebServer->send(200, "text/plain", csvText);
  });

  gWebServer->on("/api/csv", HTTP_POST, []() {
    String content = gWebServer->arg("content");
    if (!content.length() && gWebServer->hasArg("plain")) {
      content = gWebServer->arg("plain");
    }
    if (!content.length()) {
      gWebServer->send(400, "text/plain", "content is empty");
      return;
    }
    if (!saveDataCsvText(content)) {
      gWebServer->send(500, "text/plain", "save failed");
      return;
    }
    gWebServer->send(200, "text/plain", "saved /data.csv, reload scheduled");
  });

  gWebServer->on("/api/schedule", HTTP_GET, []() {
    gWebServer->send(410, "text/plain", "schedule disabled");
  });

  gWebServer->on("/api/schedule", HTTP_POST, []() {
    gWebServer->send(410, "text/plain", "schedule disabled");
  });

  gWebServer->on("/api/send", HTTP_POST, []() {
    String error;
    String text = gWebServer->arg("text");
    if (!text.length() && gWebServer->hasArg("plain")) {
      text = gWebServer->arg("plain");
    }
    bool immediateMode = gInstantRefreshNoKey;
    String immediateArg = gWebServer->arg("immediate");
    if (!immediateArg.length() && gWebServer->hasArg("instant")) {
      immediateArg = gWebServer->arg("instant");
    }
    if (immediateArg.length()) {
      bool parsed = immediateMode;
      if (!parseBoolString(immediateArg, parsed)) {
        gWebServer->send(400, "text/plain", "invalid immediate");
        return;
      }
      immediateMode = parsed;
    }
    const bool ok = immediateMode
                        ? enqueueImmediateMessage(text, error)
                        : enqueueRegularMessage(text, error);
    if (!ok) {
      const int code = (error == "queue is full") ? 503 : 400;
      gWebServer->send(code, "text/plain", error);
      return;
    }
    const UBaseType_t regularQueued = gMessageQueue ? uxQueueMessagesWaiting(gMessageQueue) : 0;
    const UBaseType_t immediateQueued = immediatePendingCount();
    String reply = immediateMode ? "queued(immediate), waiting=" : "queued(regular), waiting=";
    reply += String(static_cast<unsigned int>(regularQueued + immediateQueued));
    reply += " (regular=";
    reply += String(static_cast<unsigned int>(regularQueued));
    reply += ", immediate=";
    reply += String(static_cast<unsigned int>(immediateQueued));
    reply += ")";
    gWebServer->send(200, "text/plain", reply);
  });

  gWebServer->on("/api/image", HTTP_POST, []() {
    handleImageUploadFinalize();
  }, []() {
    handleImageUpload();
  });

  auto captiveRedirect = []() {
    redirectToPortal();
  };

  gWebServer->on("/generate_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/gen_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
  gWebServer->on("/connecttest.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/ncsi.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/fwlink", HTTP_GET, captiveRedirect);

  gWebServer->onNotFound([]() {
    const String uri = gWebServer->uri();
    if (uri.startsWith("/api/")) {
      gWebServer->send(404, "text/plain", "not found");
      return;
    }

    // Captive mini-browsers may probe odd private paths; skip filesystem lookup.
    if (uri.length() > 96 ||
        uri.startsWith("/mmtls/") ||
        uri.startsWith("/group/") ||
        uri.startsWith("/cgi-bin/")) {
      gWebServer->send(204, "text/plain", "");
      return;
    }

    if (LittleFS.exists(uri)) {
      handleStaticFile(uri);
      return;
    }

    redirectToPortal();
  });
}

void webServerTask(void *param) {
  (void)param;
  gWebServer = new PortalWebServer(kHttpPort);
  gDnsServer = new DNSServer();
  if (!gWebServer || !gDnsServer) {
    if (gDnsServer) {
      delete gDnsServer;
      gDnsServer = nullptr;
    }
    if (gWebServer) {
      delete gWebServer;
      gWebServer = nullptr;
    }
    gWebTaskRunning = false;
    gWebTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  gDnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  gDnsServer->start(kDnsPort, "*", WiFi.softAPIP());

  registerRoutes();
  gWebServer->begin();
  Serial.printf("[WEB] HTTP ready: %s\n", apRootUrl().c_str());

  while (gWebTaskRunning) {
    gDnsServer->processNextRequest();
    gWebServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(8));
  }

  gWebServer->stop();
  gDnsServer->stop();
  delete gWebServer;
  delete gDnsServer;
  gWebServer = nullptr;
  gDnsServer = nullptr;
  gWebTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool wirelessPortalStart() {
  if (gPortalStarted) return true;

  if (!gImageMutex) {
    gImageMutex = xSemaphoreCreateMutex();
    if (!gImageMutex) {
      Serial.println("[WEB] image mutex create failed");
      return false;
    }
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    resetImageUploadStateLocked();
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  loadApCredentialsFromSettingIni();
  if (!gEnableAp && !gEnableEspNow) {
    Serial.println("[WEB] EnableAP=false and EnableESPNOW=false, wireless off");
    WiFi.mode(WIFI_OFF);
    return true;
  }

  if (gEnableAp) {
    if (!LittleFS.exists("/index.html") ||
        !LittleFS.exists("/style.css") ||
        !LittleFS.exists("/app.js")) {
      Serial.println("[WEB] missing web files in LittleFS (/index.html /style.css /app.js)");
      Serial.println("[WEB] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
      return false;
    }

    if (!gMessageQueue) {
      gMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
      if (!gMessageQueue) {
        Serial.println("[WEB] regular queue create failed");
        return false;
      }
    }
  }
  if (gEnableEspNow) {
    if (!gHostMessageQueue) {
      gHostMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
      if (!gHostMessageQueue) {
        Serial.println("[ESPNOW] host queue create failed");
        return false;
      }
    }
  }

  WiFi.mode(gEnableAp ? WIFI_AP_STA : WIFI_STA);
  if (!gEnableAp && gEnableEspNow) {
    delay(10);
    if (!forceStaChannel(gApChannel)) {
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableAp) {
    bool apOk = false;
    if (gApPassword[0] == '\0') {
      apOk = WiFi.softAP(gApSsid, nullptr, gApChannel);
    } else {
      apOk = WiFi.softAP(gApSsid, gApPassword, gApChannel);
    }
    if (!apOk) {
      Serial.println("[WEB] softAP start failed");
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableEspNow) {
    if (!initEspNowReceiver()) {
      if (gEnableAp) WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableAp) {
    gWebTaskRunning = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(
        webServerTask,
        "WebPortal",
        kWebTaskStack,
        nullptr,
        kWebTaskPriority,
        &gWebTaskHandle,
        kWebTaskCore);

    if (ok != pdPASS) {
      gWebTaskRunning = false;
      deinitEspNowReceiver();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[WEB] task create failed");
      return false;
    }
  }

  gPortalStarted = true;
  Serial.printf("[WEB] wireless started EnableAP=%d EnableESPNOW=%d SSID=%s PASS=%s AP_IP=%s CH=%d HostMAC=%s\n",
                gEnableAp ? 1 : 0,
                gEnableEspNow ? 1 : 0,
                gEnableAp ? gApSsid : "<AP-OFF>",
                gEnableAp ? (gApPassword[0] ? gApPassword : "<OPEN>") : "<AP-OFF>",
                gEnableAp ? WiFi.softAPIP().toString().c_str() : "<AP-OFF>",
                WiFi.channel(),
                gHostMacFilterEnabled ? gHostMacFilterText : "<ANY>");
  return true;
}

bool wirelessPortalStartEspNowOnly() {
  if (gPortalStarted) return true;

  if (!gImageMutex) {
    gImageMutex = xSemaphoreCreateMutex();
    if (!gImageMutex) {
      Serial.println("[ESPNOW] image mutex create failed");
      return false;
    }
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    resetImageUploadStateLocked();
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  loadApCredentialsFromSettingIni();
  gEnableAp = false;
  gEnableEspNow = true;

  if (!gHostMessageQueue) {
    gHostMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
    if (!gHostMessageQueue) {
      Serial.println("[ESPNOW] host queue create failed");
      return false;
    }
  }

  WiFi.mode(WIFI_STA);
  delay(10);
  if (!forceStaChannel(gApChannel)) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  if (!initEspNowReceiver()) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  gPortalStarted = true;
  Serial.printf("[ESPNOW] STA-only receiver started CH=%d HostMAC=%s\n",
                WiFi.channel(),
                gHostMacFilterEnabled ? gHostMacFilterText : "<ANY>");
  return true;
}

void wirelessPortalStop() {
  if (!gPortalStarted) return;

  gWebTaskRunning = false;
  for (int i = 0; i < 120 && gWebTaskHandle != nullptr; ++i) {
    delay(5);
  }

  if (gWebTaskHandle != nullptr) {
    vTaskDelete(gWebTaskHandle);
    gWebTaskHandle = nullptr;
  }

  if (gWebServer) {
    gWebServer->stop();
    delete gWebServer;
    gWebServer = nullptr;
  }

  if (gDnsServer) {
    gDnsServer->stop();
    delete gDnsServer;
    gDnsServer = nullptr;
  }

  deinitEspNowReceiver();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  if (gMessageQueue) {
    WebQueuedMessage drop = {};
    while (xQueueReceive(gMessageQueue, &drop, 0) == pdTRUE) {}
  }
  portENTER_CRITICAL(&gImmediateMessageMux);
  gImmediateMessage.text[0] = '\0';
  gImmediateMessage.pending = false;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  if (gHostMessageQueue) {
    WebQueuedMessage drop = {};
    while (xQueueReceive(gHostMessageQueue, &drop, 0) == pdTRUE) {}
  }

  if (gImageMutex && xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    resetImageUploadStateLocked();
    if (gPendingImage.pixels) {
      free(gPendingImage.pixels);
    }
    gPendingImage.pixels = nullptr;
    gPendingImage.pixelCapacity = 0;
    gPendingImage.pixelCount = 0;
    gPendingImage.width = 0;
    gPendingImage.height = 0;
    gPendingImage.centerX = kImageDefaultCenterX;
    gPendingImage.centerY = kImageDefaultCenterY;
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = false;
  gScheduleReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);

  gPortalStarted = false;
  Serial.println("[WEB] AP stopped");
}

bool wirelessPortalPopMessage(String &outMessage) {
  outMessage = "";
  if (!gMessageQueue) return false;

  WebQueuedMessage msg = {};
  if (xQueueReceive(gMessageQueue, &msg, 0) != pdTRUE) return false;

  outMessage = msg.text;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingMessage() {
  if (!gMessageQueue) return false;
  return uxQueueMessagesWaiting(gMessageQueue) > 0;
}

bool wirelessPortalPopImmediateMessage(String &outMessage) {
  outMessage = "";
  char local[kTextMaxLen + 1] = {0};
  bool has = false;
  portENTER_CRITICAL(&gImmediateMessageMux);
  if (gImmediateMessage.pending) {
    memcpy(local, gImmediateMessage.text, sizeof(local));
    local[sizeof(local) - 1] = '\0';
    gImmediateMessage.pending = false;
    has = true;
  }
  portEXIT_CRITICAL(&gImmediateMessageMux);
  if (!has) return false;

  outMessage = local;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingImmediateMessage() {
  return immediatePendingCount() > 0;
}

bool wirelessPortalPopHostMessage(String &outMessage) {
  outMessage = "";
  if (!gHostMessageQueue) return false;

  WebQueuedMessage msg = {};
  if (xQueueReceive(gHostMessageQueue, &msg, 0) != pdTRUE) return false;

  outMessage = msg.text;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingHostMessage() {
  if (!gHostMessageQueue) return false;
  return uxQueueMessagesWaiting(gHostMessageQueue) > 0;
}

bool wirelessPortalPushMessageForRestore(const String &text) {
  String error;
  return enqueueRegularMessage(text, error);
}

bool wirelessPortalPushImmediateMessageForRestore(const String &text) {
  String error;
  return enqueueImmediateMessage(text, error);
}

bool wirelessPortalPushHostMessageForRestore(const String &text) {
  if (!gHostMessageQueue) return false;

  String normalized = text;
  normalized.trim();
  if (!normalized.length()) return false;
  if (normalized.length() > kTextMaxLen) {
    normalized.remove(kTextMaxLen);
  }

  WebQueuedMessage msg = {};
  normalized.toCharArray(msg.text, sizeof(msg.text));
  return xQueueSend(gHostMessageQueue, &msg, 0) == pdTRUE;
}

bool wirelessPortalConsumeCsvReloadRequest() {
  return takeCsvReloadRequested();
}

bool wirelessPortalConsumeScheduleReloadRequest() {
  return takeScheduleReloadRequested();
}

bool wirelessPortalInstantRefreshNoKeyEnabled() {
  return gInstantRefreshNoKey;
}

bool wirelessPortalTakePendingImage(uint16_t *outPixels,
                                    size_t outCapacityPixels,
                                    uint16_t &outWidth,
                                    uint16_t &outHeight,
                                    int16_t &outCenterX,
                                    int16_t &outCenterY,
                                    size_t &outPixelCount) {
  outWidth = 0;
  outHeight = 0;
  outCenterX = kImageDefaultCenterX;
  outCenterY = kImageDefaultCenterY;
  outPixelCount = 0;

  if (!outPixels || outCapacityPixels == 0 || !gImageMutex) return false;
  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

  if (!gPendingImage.ready || !gPendingImage.pixels || gPendingImage.pixelCount == 0) {
    xSemaphoreGive(gImageMutex);
    return false;
  }
  if (gPendingImage.pixelCount > outCapacityPixels) {
    xSemaphoreGive(gImageMutex);
    return false;
  }

  memcpy(outPixels, gPendingImage.pixels, gPendingImage.pixelCount * sizeof(uint16_t));
  outWidth = gPendingImage.width;
  outHeight = gPendingImage.height;
  outCenterX = gPendingImage.centerX;
  outCenterY = gPendingImage.centerY;
  outPixelCount = gPendingImage.pixelCount;
  gPendingImage.ready = false;

  xSemaphoreGive(gImageMutex);
  return true;
}
