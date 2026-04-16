#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <FFat.h>
#include <USB.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <algorithm>
#include <limits>
#include <vector>
#include <ctype.h>
#include <time.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "AppGlobals.h"
#include "UsbAppMode.h"
#include "DisplayEffects.h"
#include "WirelessPortal.h"
#include "Ds1302Rtc.h"
#include "Index_B.h"
#include "Key_Drv.h"

static bool ensureStaQueueMutex();
static void clearStaMessageQueue();
static bool pushStaMessageQueue(const String &message);
static bool loadStaCredentialsFromSettingIni(String &outSsid, String &outPassword, String &outNet);
static void ensureStaFetcherTaskStarted();
static void beginStaConnectAttempt();
static bool refreshTodayReminderSlotsFromRtc(bool forceReload, bool markPastTriggered);
static bool parseAppModeFromIniValue(String value, AppLoopMode &outMode);
static const char *appModeToIniValue(AppLoopMode mode);
static bool loadAppModeFromSettingIni(AppLoopMode &outMode);
static bool persistAppModeToSettingIni(AppLoopMode mode);
void waitWakeKeyReleaseBeforeSleep();
bool saveSleepSnapshotToFat();
void markSleepRtcContextForSleep();
[[noreturn]] void enterDeepSleepNow(uint32_t wakeSec);
bool handleRtcMaintenanceWake();

namespace {

constexpr uint8_t kBacklightDutyOff = 0;
constexpr int kDefaultBacklightCloseTimeSec = 20;
constexpr uint32_t kBacklightTaskTickMs = 100UL;
constexpr uint32_t kBootAnimPollMs = 10UL;
constexpr uint32_t kBootAnimMaxWaitMs = 12000UL;
constexpr uint32_t kStaOnlySleepAfterOffMs = 60000UL;
constexpr gpio_num_t kWakeKeyGpio = GPIO_NUM_2;
constexpr uint32_t kRtcWakeDefaultSec = 60U;
constexpr uint32_t kRtcWakeMaxSec = 30U * 60U;
constexpr uint32_t kRtcMismatchToleranceSec = 2U;
constexpr uint32_t kReminderTriggerWindowSec = 59U;
constexpr size_t kSleepTextMaxLen = 240;
constexpr size_t kSleepPortalQueueMax = 128;
constexpr size_t kSleepStaQueueMax = 20;
constexpr size_t kMaxReminderTimes = 128;
constexpr size_t kScheduleInterruptQueueMax = 16;
constexpr char kSleepSnapshotPath[] = "/sleep_state.bin";
constexpr const char *kDefaultReminderMessage = u8"这个时候你似乎有什么事要干";
constexpr uint32_t kSleepFileMagic = 0x53534E50UL;      // "SSNP"
constexpr uint16_t kSleepFileVersion = 1;
constexpr uint32_t kSleepRtcCtxMagic = 0x54534654UL;    // "TSFT"
constexpr uint16_t kSleepRtcCtxVersion = 1;

struct SleepRtcContext {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t snapshotValid = 0;
  uint8_t appMode = 0;
  uint32_t nextWakeSec = 0;
  uint8_t pendingReminder = 0;
  uint64_t expectedUnix = 0;
  char reminderMessage[kSleepTextMaxLen + 1] = {0};
};

struct SleepSnapshotHeader {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t appMode = 0;
  uint8_t runState = 0;
  uint8_t firstFlag = 0;
  uint8_t staPhase = 0;
  int32_t csvCount = 0;
  uint8_t staRetryCount = 0;
  uint8_t staQueueCount = 0;
  uint8_t regularQueueCount = 0;
  uint8_t hostQueueCount = 0;
  uint8_t hasImmediate = 0;
  int32_t backlightTimeSec = -1;
  char lastDisplayed[kSleepTextMaxLen + 1] = {0};
  char immediateMessage[kSleepTextMaxLen + 1] = {0};
};

RTC_DATA_ATTR SleepRtcContext gSleepRtcCtx;

enum BacklightState : uint8_t {
  kBacklightBright = 0,
  kBacklightDim = 1,
  kBacklightOff = 2,
};

TaskHandle_t gBacklightTaskHandle = nullptr;
portMUX_TYPE gBacklightMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gBacklightLastActivityMs = 0;
BacklightState gBacklightState = kBacklightBright;
TaskHandle_t gBootAnimWaiter = nullptr;
bool gBootAnimRunning = false;
bool gBootAnimUsbDetected = false;
bool gSimheiFontPreloaded = false;
bool gSettingsPreloadedAtBoot = false;
char gLastDisplayedText[kSleepTextMaxLen + 1] = {0};

struct ScheduleInterruptQueueItem {
  char text[kSleepTextMaxLen + 1] = {0};
};

struct DailyReminderSlot {
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint16_t order = 0;
  bool triggered = false;
};

ScheduleInterruptQueueItem gScheduleInterruptQueue[kScheduleInterruptQueueMax];
size_t gScheduleInterruptHead = 0;
size_t gScheduleInterruptCount = 0;
DailyReminderSlot gTodayReminderSlots[kMaxReminderTimes];
size_t gTodayReminderCount = 0;
uint32_t gTodayReminderDateKey = 0;
uint16_t gTodayReminderLastCheckedMinute = 0xFFFFU;

void sanitizeMessageForSnapshot(const String &in, char out[kSleepTextMaxLen + 1]) {
  if (!out) return;
  String normalized = in;
  normalized.replace("\r", " ");
  normalized.replace("\n", " ");
  normalized.trim();
  if (normalized.length() > kSleepTextMaxLen) {
    normalized.remove(kSleepTextMaxLen);
  }
  normalized.toCharArray(out, kSleepTextMaxLen + 1);
  out[kSleepTextMaxLen] = '\0';
}

void rememberLastDisplayedText(const char *text) {
  if (!text) return;
  String normalized = text;
  sanitizeMessageForSnapshot(normalized, gLastDisplayedText);
}

void clearScheduleInterruptQueue() {
  gScheduleInterruptHead = 0;
  gScheduleInterruptCount = 0;
  for (size_t i = 0; i < kScheduleInterruptQueueMax; ++i) {
    gScheduleInterruptQueue[i].text[0] = '\0';
  }
}

bool enqueueScheduleInterruptMessage(const String &rawText) {
  String normalized = rawText;
  normalized.replace("\r", " ");
  normalized.replace("\n", " ");
  normalized.trim();
  if (!normalized.length()) {
    normalized = kDefaultReminderMessage;
  }
  if (normalized.length() > kSleepTextMaxLen) {
    normalized.remove(kSleepTextMaxLen);
  }

  if (gScheduleInterruptCount >= kScheduleInterruptQueueMax) {
    gScheduleInterruptHead = (gScheduleInterruptHead + 1) % kScheduleInterruptQueueMax;
    gScheduleInterruptCount--;
  }

  const size_t writeIdx = (gScheduleInterruptHead + gScheduleInterruptCount) % kScheduleInterruptQueueMax;
  normalized.toCharArray(gScheduleInterruptQueue[writeIdx].text,
                         sizeof(gScheduleInterruptQueue[writeIdx].text));
  gScheduleInterruptQueue[writeIdx].text[kSleepTextMaxLen] = '\0';
  gScheduleInterruptCount++;
  return true;
}

bool popScheduleInterruptMessage(String &outText) {
  outText = "";
  if (gScheduleInterruptCount == 0) return false;
  const size_t readIdx = gScheduleInterruptHead;
  outText = String(gScheduleInterruptQueue[readIdx].text);
  gScheduleInterruptQueue[readIdx].text[0] = '\0';
  gScheduleInterruptHead = (gScheduleInterruptHead + 1) % kScheduleInterruptQueueMax;
  gScheduleInterruptCount--;
  outText.trim();
  return outText.length() > 0;
}

bool writeExact(fs::File &f, const void *data, size_t len) {
  if (!data || len == 0) return len == 0;
  return f.write(static_cast<const uint8_t *>(data), len) == len;
}

bool readExact(fs::File &f, void *data, size_t len) {
  if (!data || len == 0) return len == 0;
  return f.read(static_cast<uint8_t *>(data), len) == static_cast<int>(len);
}

void clearSleepRtcContext() {
  gSleepRtcCtx.magic = 0;
  gSleepRtcCtx.version = 0;
  gSleepRtcCtx.snapshotValid = 0;
  gSleepRtcCtx.appMode = 0;
  gSleepRtcCtx.nextWakeSec = 0;
  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.expectedUnix = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';
}

bool hasValidSleepRtcContext() {
  return gSleepRtcCtx.magic == kSleepRtcCtxMagic &&
         gSleepRtcCtx.version == kSleepRtcCtxVersion &&
         gSleepRtcCtx.snapshotValid != 0;
}

float clampUnitFloat(float value) {
  if (value != value) return 1.0f;  // NaN fallback
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint8_t backlightDutyBrightFromLevel(float level) {
  const float clamped = clampUnitFloat(level);
  const int duty = static_cast<int>(clamped * 255.0f + 0.5f);
  if (duty < 0) return 0;
  if (duty > 255) return 255;
  return static_cast<uint8_t>(duty);
}

uint8_t backlightDutyDimFromBright(uint8_t brightDuty) {
  if (brightDuty == 0) return 0;
  uint8_t dimDuty = static_cast<uint8_t>(brightDuty / 2);
  if (dimDuty == 0) dimDuty = 1;
  return dimDuty;
}

void applyBacklightState(BacklightState state) {
  const uint8_t brightDuty = backlightDutyBrightFromLevel(gBacklightLevel);
  const uint8_t dimDuty = backlightDutyDimFromBright(brightDuty);
  switch (state) {
    case kBacklightBright:
      ledcWrite(0, brightDuty);
      break;
    case kBacklightDim:
      ledcWrite(0, dimDuty);
      break;
    case kBacklightOff:
      ledcWrite(0, kBacklightDutyOff);
      break;
  }
}

bool wakeBacklightByKeyIfNeeded() {
  bool wakeOnly = false;
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  if (gBacklightState != kBacklightBright) {
    gBacklightState = kBacklightBright;
    wakeOnly = true;
  }
  portEXIT_CRITICAL(&gBacklightMux);
  if (wakeOnly) {
    applyBacklightState(kBacklightBright);
  }
  return wakeOnly;
}

uint32_t backlightCloseDelayMs() {
  const int closeSec = (gBacklightCloseTimeSec < 0) ? 0 : gBacklightCloseTimeSec;
  return static_cast<uint32_t>(closeSec) * 1000UL;
}

bool staOnlySleepTimeoutReached() {
  int backlightTimeSec = -1;
  uint32_t lastActivity = 0;
  BacklightState stateNow = kBacklightBright;
  portENTER_CRITICAL(&gBacklightMux);
  backlightTimeSec = gBacklightTimeSec;
  lastActivity = gBacklightLastActivityMs;
  stateNow = gBacklightState;
  portEXIT_CRITICAL(&gBacklightMux);

  if (stateNow != kBacklightOff || backlightTimeSec < 0) return false;

  const uint32_t dimMs = static_cast<uint32_t>(backlightTimeSec) * 1000UL;
  const uint32_t offAtMs = lastActivity + dimMs + backlightCloseDelayMs();
  const uint32_t nowMs = millis();
  return (nowMs - offAtMs) >= kStaOnlySleepAfterOffMs;
}

[[noreturn]] void enterStaOnlyDeepSleep() {
  Serial.println("[STA_ONLY] backlight off for 60s, entering deep sleep");
  const bool snapshotOk = saveSleepSnapshotToFat();
  if (snapshotOk) {
    markSleepRtcContextForSleep();
  } else {
    Serial.println("[SLEEP] snapshot save failed, fallback to plain sleep");
    clearSleepRtcContext();
  }
  mixer.stopBG();
  mixer.stopInsert();
  wirelessPortalStop();
  WiFi.mode(WIFI_OFF);
  waitWakeKeyReleaseBeforeSleep();
  enterDeepSleepNow(kRtcWakeDefaultSec);
}

void backlightTask(void *param) {
  (void)param;
  while (true) {
    int backlightTimeSec = -1;
    uint32_t lastActivity = 0;
    BacklightState stateNow = kBacklightBright;
    const uint32_t nowMs = millis();

    portENTER_CRITICAL(&gBacklightMux);
    backlightTimeSec = gBacklightTimeSec;
    lastActivity = gBacklightLastActivityMs;
    stateNow = gBacklightState;
    portEXIT_CRITICAL(&gBacklightMux);

    BacklightState desired = kBacklightBright;
    if (backlightTimeSec >= 0) {
      const uint32_t dimMs = static_cast<uint32_t>(backlightTimeSec) * 1000UL;
      const uint32_t closeDelayMs = backlightCloseDelayMs();
      const uint32_t elapsedMs = nowMs - lastActivity;
      if (elapsedMs >= dimMs + closeDelayMs) {
        desired = kBacklightOff;
      } else if (elapsedMs >= dimMs) {
        desired = kBacklightDim;
      }
    }

    if (desired != stateNow) {
      portENTER_CRITICAL(&gBacklightMux);
      gBacklightState = desired;
      portEXIT_CRITICAL(&gBacklightMux);
      applyBacklightState(desired);
    }

    vTaskDelay(pdMS_TO_TICKS(kBacklightTaskTickMs));
  }
}

void ensureBacklightTaskStarted() {
  if (gBacklightTaskHandle) return;
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  gBacklightState = kBacklightBright;
  portEXIT_CRITICAL(&gBacklightMux);
  applyBacklightState(kBacklightBright);
  xTaskCreatePinnedToCore(backlightTask, "BacklightTask", 4096, nullptr, 1, &gBacklightTaskHandle, 1);
}

}  // namespace

static bool wlWriteRmw(size_t addr, const uint8_t *src, size_t len) {
  if (wlHandle == WL_INVALID_HANDLE) return false;

  const size_t wlSector = wl_sector_size(wlHandle);
  if (wlSector == 0) return false;

  std::vector<uint8_t> cache(wlSector);
  if (cache.empty()) return false;

  while (len > 0) {
    const size_t base = (addr / wlSector) * wlSector;
    const size_t inSector = addr - base;
    size_t chunk = wlSector - inSector;
    if (chunk > len) chunk = len;

    if (wl_read(wlHandle, base, cache.data(), wlSector) != ESP_OK) return false;
    memcpy(cache.data() + inSector, src, chunk);
    if (wl_erase_range(wlHandle, base, wlSector) != ESP_OK) return false;
    if (wl_write(wlHandle, base, cache.data(), wlSector) != ESP_OK) return false;

    addr += chunk;
    src += chunk;
    len -= chunk;
  }

  return true;
}

void ensureDisplayReady() {
  if (displayBootstrapped) return;
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  displayBootstrapped = true;
}

void showUsbModeScreen() {
  ensureDisplayReady();

  spriteBoot.createSprite(320, 120);
  tft.fillScreen(TFT_BLACK);
  spriteBoot.setTextWrap(false, false);
  spriteBoot.setTextColor(0xff36, TFT_BLACK);
  spriteBoot.setTextSize(2);
  delay(300);
  ledcWrite(0, backlightDutyBrightFromLevel(gBacklightLevel));

  const char *line1 = "USB MODE";
  const char *line2 = "Mass Storage Connected";
  const char *line3 = "Edit files on your PC...";

  auto typeLine = [&](int x, int y, const char *line, int stepDelayMs) {
    String buf;
    for (int i = 0; line[i] != '\0'; ++i) {
      buf += line[i];
      spriteBoot.setCursor(x, y);
      spriteBoot.print(buf);
      spriteBoot.pushSprite(0, 100);
      delay(stepDelayMs);
    }
  };

  typeLine(18, 20, line1, 10);
  typeLine(18, 60, line2, 10);
  typeLine(18, 100, line3, 10);
}

static void preloadSettingIniForBootAnimation() {
  if (gSettingsPreloadedAtBoot) return;

  bool mountedTemp = false;
  if (!FFat.begin(false, kFatMountPoint, 10, kFatPartitionLabel)) {
    Serial.println("[BOOT] skip setting.ini preload (FAT not ready)");
    return;
  }
  mountedTemp = true;

  applyAudioGainsFromSettingIni();
  gSettingsPreloadedAtBoot = true;
  Serial.printf("[BOOT] setting.ini preloaded, backlight=%.3f\n", gBacklightLevel);

  if (mountedTemp) {
    FFat.end();
  }
}

void runBootAnimationTaskStart() {
  if (gBootAnimRunning) return;

  ensureDisplayReady();
  tft.fillScreen(TFT_BLACK);

  // Preload config first so boot animation brightness follows setting.ini BackLight.
  preloadSettingIniForBootAnimation();

  bool littleFsReady = false;
  if (LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel)) {
    littleFsReady = true;
  } else {
    Serial.println("[BOOT] LittleFS mount failed during boot preload");
  }

  Text.createSprite(320, 120);
  Text.fillSprite(TFT_BLACK);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0xff36, TFT_BLACK);

  bool usedOxta = false;
  if (littleFsReady && LittleFS.exists("/Oxta14.vlw")) {
    Text.loadFont("Oxta14", LittleFS);
    usedOxta = true;
    Serial.println("[BOOT] Oxta14 preloaded for boot animation");
  } else {
    Text.setTextSize(2);
    if (littleFsReady) {
      Serial.println("[BOOT] Oxta14.vlw missing, fallback to default font");
    }
  }

  Text.drawString("PROJECT MOON", 180, 60);
  if (usedOxta) {
    Text.unloadFont();
  }
  Text.setTextWrap(true, true);

  

  gBootAnimWaiter = xTaskGetCurrentTaskHandle();
  gBootAnimUsbDetected = usbHostActive;
  const BaseType_t taskOk = xTaskCreate(
      task_LogoFadeInAndMove,
      "LogoFadeMove",
      20480,
      gBootAnimWaiter,
      1,
      nullptr);
  if (taskOk != pdPASS) {
    Serial.println("[BOOT] animation task create failed");
    gBootAnimWaiter = nullptr;
    gBootAnimRunning = false;
    return;
  }
  if (!usbHostActive && littleFsReady && !gSimheiFontPreloaded) {
    if (LittleFS.exists("/simhei15.vlw")) {
      Text.loadFont("simhei15", LittleFS);
      gSimheiFontPreloaded = true;
      Serial.println("[BOOT] simhei15 preloaded before animation end");
    } else {
      Serial.println("[BOOT] simhei15.vlw missing during boot preload");
    }
  }
  gBootAnimRunning = true;
}

void runBootAnimationTaskWait() {
  if (!gBootAnimRunning) return;

  bool animDone = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < kBootAnimMaxWaitMs) {
    if (usbHostActive) {
      gBootAnimUsbDetected = true;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kBootAnimPollMs)) > 0) {
      animDone = true;
      break;
    }
  }

  if (!animDone) {
    Serial.println("[BOOT] animation wait timeout");
  }
  if (gBootAnimUsbDetected) {
    Serial.println("[BOOT] USB detected during boot animation");
  }

  gBootAnimRunning = false;
  gBootAnimWaiter = nullptr;
}

void runBootAnimationTaskAndWait() {
  runBootAnimationTaskStart();
  runBootAnimationTaskWait();
}

void notifyBacklightActivity() {
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  const bool wasNotBright = (gBacklightState != kBacklightBright);
  gBacklightState = kBacklightBright;
  portEXIT_CRITICAL(&gBacklightMux);
  if (wasNotBright) {
    applyBacklightState(kBacklightBright);
  }
}

void setBacklightTimeSeconds(int seconds) {
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightTimeSec = seconds;
  portEXIT_CRITICAL(&gBacklightMux);
  notifyBacklightActivity();
}

void setBacklightLevel(float level) {
  gBacklightLevel = clampUnitFloat(level);

  BacklightState stateNow = kBacklightBright;
  portENTER_CRITICAL(&gBacklightMux);
  stateNow = gBacklightState;
  portEXIT_CRITICAL(&gBacklightMux);

  applyBacklightState(stateNow);
}

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (wlHandle == WL_INVALID_HANDLE) return -1;
  const size_t addr = static_cast<size_t>(lba) * mscBlockSize + offset;
  if (wl_read(wlHandle, addr, buffer, bufsize) != ESP_OK) return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (wlHandle == WL_INVALID_HANDLE) return -1;
  const size_t addr = static_cast<size_t>(lba) * mscBlockSize + offset;
  if (!wlWriteRmw(addr, buffer, bufsize)) return -1;
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  Serial.printf("[MSC] start=%d eject=%d\n", start, load_eject);
  return true;
}

void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_data;
  if (event_base != ARDUINO_USB_EVENTS) return;

  switch (event_id) {
    case ARDUINO_USB_RESUME_EVENT:
      usbHostActive = true;
      Serial.println("[USB] host active");
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      Serial.println("[USB] host suspended");
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      usbHostActive = false;
      if (!usbDisconnectedLogged) {
        Serial.println("[USB] host disconnected (cable removed or host detached)");
        usbDisconnectedLogged = true;
      }
      break;
    case ARDUINO_USB_STARTED_EVENT:
      usbHostActive = true;
      usbDisconnectedLogged = false;
      Serial.println("[USB] device started");
      break;
    default:
      break;
  }
}

bool mountFat() {
  if (fatMounted) return true;
  if (!FFat.begin(false, kFatMountPoint, 10, kFatPartitionLabel)) {
    Serial.println("[APP] FFat.begin failed");
    return false;
  }
  fatMounted = true;
  Serial.println("[APP] FAT mounted");
  return true;
}

void applyAudioGainsFromSettingIni() {
  static constexpr float kDefaultInsertGain = 0.2f;
  static constexpr float kDefaultBgGain = 0.2f;
  static constexpr int kDefaultWrongProb3 = 25;
  static constexpr int kDefaultWrongProb5 = 12;
  static constexpr bool kDefaultEnableReprint = true;
  static constexpr float kDefaultBacklightLevel = 1.0f;
  static constexpr int kDefaultBacklightTimeSec = -1;
  static constexpr int kDefaultBacklightCloseTime = kDefaultBacklightCloseTimeSec;

  gInsertGain = kDefaultInsertGain;
  gBgGain = kDefaultBgGain;
  gBacklightLevel = kDefaultBacklightLevel;
  gWrongProb3 = kDefaultWrongProb3;
  gWrongProb5 = kDefaultWrongProb5;
  gEnableReprint = kDefaultEnableReprint;
  gBacklightTimeSec = kDefaultBacklightTimeSec;
  gBacklightCloseTimeSec = kDefaultBacklightCloseTime;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) {
    Serial.println("[APP] /setting.ini not found, using default gains");
    return;
  }

  bool gotInsert = false;
  bool gotBg = false;
  bool gotWrong3 = false;
  bool gotWrong5 = false;
  bool gotReprint = false;
  bool gotBacklight = false;
  bool gotBacklightTime = false;
  bool gotBacklightCloseTime = false;
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
    value.trim();
    value.replace(";", "");
    key.toLowerCase();
    value.toLowerCase();
    value.trim();
    while (value.length() && !isalnum((unsigned char)value[value.length() - 1])) {
      value.remove(value.length() - 1);
    }

    const float parsed = value.toFloat();
    if (key == "insertgain") {
      gInsertGain = parsed;
      gotInsert = true;
    } else if (key == "backgroundgain") {
      gBgGain = parsed;
      gotBg = true;
    } else if (key == "backlight") {
      gBacklightLevel = clampUnitFloat(parsed);
      gotBacklight = true;
    } else if (key == "testwrongindexpersent_3area") {
      gWrongProb3 = constrain(value.toInt(), 0, 100);
      gotWrong3 = true;
    } else if (key == "testwrongindexpersent_5area") {
      gWrongProb5 = constrain(value.toInt(), 0, 100);
      gotWrong5 = true;
    } else if (key == "enablereprint") {
      gEnableReprint = (value == "1" || value == "true" || value == "on" || value == "yes");
      gotReprint = true;
    } else if (key == "backlighttime") {
      gBacklightTimeSec = value.toInt();
      gotBacklightTime = true;
    } else if (key == "backlightclosetime") {
      const int parsedCloseSec = static_cast<int>(value.toInt());
      gBacklightCloseTimeSec = (parsedCloseSec < 0) ? 0 : parsedCloseSec;
      gotBacklightCloseTime = true;
    }
  }
  f.close();

  if (!gotInsert) Serial.printf("[APP] InsertGain missing, default=%.3f\n", gInsertGain);
  if (!gotBg) Serial.printf("[APP] BackGroundGain missing, default=%.3f\n", gBgGain);
  if (!gotWrong3) Serial.printf("[APP] TestWrongIndexPersent_3Area missing, default=%d\n", gWrongProb3);
  if (!gotWrong5) Serial.printf("[APP] TestWrongIndexPersent_5Area missing, default=%d\n", gWrongProb5);
  if (!gotReprint) Serial.printf("[APP] EnableReprint missing, default=%d\n", gEnableReprint ? 1 : 0);
  if (!gotBacklight) Serial.printf("[APP] BackLight missing, default=%.3f\n", gBacklightLevel);
  if (!gotBacklightTime) Serial.printf("[APP] BacklightTime missing, default=%d\n", gBacklightTimeSec);
  if (!gotBacklightCloseTime) Serial.printf("[APP] BacklightCloseTime missing, default=%d\n", gBacklightCloseTimeSec);
  Serial.printf("[APP] gains: insert=%.3f bg=%.3f backlight=%.3f\n",
                gInsertGain,
                gBgGain,
                gBacklightLevel);
  Serial.printf("[APP] glitch: p3=%d p5=%d reprint=%d backlightTime=%d closeTime=%d\n",
                gWrongProb3,
                gWrongProb5,
                gEnableReprint ? 1 : 0,
                gBacklightTimeSec,
                gBacklightCloseTimeSec);
}

void unmountFat() {
  if (!fatMounted) return;
  FFat.end();
  fatMounted = false;
  Serial.println("[APP] FAT unmounted");
}

bool openRawBackend() {
  if (wlHandle != WL_INVALID_HANDLE) return true;

  if (!fatPart) {
    fatPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, kFatPartitionLabel);
  }
  if (!fatPart) {
    Serial.printf("[MSC] FAT partition '%s' not found\n", kFatPartitionLabel);
    return false;
  }

  if (wl_mount(fatPart, &wlHandle) != ESP_OK) {
    Serial.println("[MSC] wl_mount failed");
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  flashBytes = wl_size(wlHandle);
  mscBlockSize = static_cast<uint32_t>(wl_sector_size(wlHandle));
  if (mscBlockSize == 0 || mscBlockSize > 65535) {
    Serial.printf("[MSC] invalid block size: %lu\n", static_cast<unsigned long>(mscBlockSize));
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  sectorCount = static_cast<uint32_t>(flashBytes / mscBlockSize);
  if (sectorCount == 0) {
    Serial.println("[MSC] invalid sector count");
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  Serial.printf("[MSC] backend ready: %lu sectors x %lu bytes\n",
                static_cast<unsigned long>(sectorCount),
                static_cast<unsigned long>(mscBlockSize));
  return true;
}

void closeRawBackend() {
  if (wlHandle == WL_INVALID_HANDLE) return;
  wl_unmount(wlHandle);
  wlHandle = WL_INVALID_HANDLE;
  Serial.println("[MSC] backend closed");
}

bool enterUsbMode() {
  if (usbModeActive) return true;
  showUsbModeScreen();
  unmountFat();
  if (!openRawBackend()) return false;
  msc.mediaPresent(true);
  usbModeActive = true;
  Serial.println("[MSC] USB mode active");
  return true;
}

bool enterAppMode() {
  if (!usbModeActive && fatMounted) return true;
  if (usbModeActive) {
    msc.mediaPresent(false);
    usbModeActive = false;
    delay(200);
  }
  closeRawBackend();
  return mountFat();
}

bool initProjectResources() {
  if (appInitialized) return true;

  ensureDisplayReady();
  //tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  pinMode(42, OUTPUT);
  digitalWrite(42, HIGH);

  Key_init();
  clearScheduleInterruptQueue();

  if (!LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel)) {
    Serial.println("[APP] LittleFS mount failed, trying format...");
    if (!LittleFS.format()) {
      Serial.println("[APP] LittleFS format failed");
      return false;
    }
    if (!LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel)) {
      Serial.println("[APP] LittleFS init failed after format");
      return false;
    }
    Serial.println("[APP] LittleFS formatted and mounted");
    Serial.println("[APP] note: LittleFS was rebuilt, run uploadfs to restore font/audio files");
  }

  if (!LittleFS.exists("/simhei15.vlw")) {
    Serial.println("[APP] LittleFS font files missing");
    Serial.println("[APP] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
    return false;
  }
  if (!LittleFS.exists("/Oxta14.vlw")) {
    Serial.println("[APP] Oxta14.vlw missing, boot logo text falls back to default font");
  }

  WavMixerI2S::I2SPinConfig pins = {.bck = 40, .ws = 39, .dout = 41};
  if (!mixer.begin(pins)) {
    Serial.println("[APP] Mixer init failed");
    return false;
  }
  mixer.startOnCore(0);

  if (!mountFat()) {
    Serial.println("[APP] mount FAT failed");
    return false;
  }
  if (!gSettingsPreloadedAtBoot) {
    applyAudioGainsFromSettingIni();
  } else {
    Serial.printf("[APP] setting.ini already preloaded, backlight=%.3f\n", gBacklightLevel);
  }

  if (!csv.load(FFat, "/data.csv")) {
    Serial.println("[APP] /data.csv load failed from FAT");
    return false;
  }
  (void)refreshTodayReminderSlotsFromRtc(true, true);

  Text.createSprite(320, 120);
  Text.fillSprite(TFT_BLACK);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0x07ff, TFT_BLACK);
  Text.setTextWrap(true, true);
  message = csv.getTextById(1);
  if (!gSimheiFontPreloaded) {
    Text.loadFont("simhei15", LittleFS);
    gSimheiFontPreloaded = true;
    Serial.println("[APP] simhei15 loaded in app init");
  } else {
    Serial.println("[APP] simhei15 already preloaded");
  }

  ensureBacklightTaskStarted();
  setBacklightLevel(gBacklightLevel);
  setBacklightTimeSeconds(gBacklightTimeSec);

  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
  firstFlag = true;
  appInitialized = true;
  Serial.println("[APP] project initialized");
  return true;
}

static void playMessageWithGlitch(const char *text) {
  if (!text || !text[0]) return;
  static int8_t sBbEndExists = -1;

  rememberLastDisplayedText(text);
  notifyBacklightActivity();
  mixer.playBG("/BG.wav");
  mixer.playInsert("/BGstart.wav");
  Text.fillRect(0, 0, tft.width(), 100, TFT_BLACK);
  Text.pushImage(160 - 60, 50 - 60, 120, 120, (uint16_t *)Index_B);
  Text.pushSprite(0, 150 - 50);
  showGlitchEffectUTF8(text);
  mixer.stopBG();
  mixer.playBGnoLoop("/BGend.wav");
  if (sBbEndExists < 0 && fatMounted) {
    sBbEndExists = FFat.exists("/sound/BBend.wav") ? 1 : 0;
    if (sBbEndExists == 0) {
      Serial.println("[AUDIO] /sound/BBend.wav missing, skip BBend insert");
    }
  }
  if (sBbEndExists != 0) {
    mixer.playInsert("/BBend.wav");
  }
}

static constexpr uint16_t kWebImageMaxWidth = 320;
static constexpr uint16_t kWebImageMaxHeight = 240;
static constexpr size_t kWebImageMaxPixels =
    static_cast<size_t>(kWebImageMaxWidth) * static_cast<size_t>(kWebImageMaxHeight);
static uint16_t *gWebImageScratch = nullptr;
static size_t gWebImageScratchPixels = 0;
static uint16_t *gWebImageLineBuffer = nullptr;
static size_t gWebImageLineBufferPixels = 0;

static bool ensureWebImageScratch(size_t pixelCount) {
  if (pixelCount == 0 || pixelCount > kWebImageMaxPixels) return false;
  if (gWebImageScratch && gWebImageScratchPixels >= pixelCount) return true;

  const size_t bytes = pixelCount * sizeof(uint16_t);
  uint16_t *next = nullptr;
  if (psramFound()) {
    next = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!next) {
    next = static_cast<uint16_t *>(malloc(bytes));
  }
  if (!next) return false;

  if (gWebImageScratch) {
    free(gWebImageScratch);
  }
  gWebImageScratch = next;
  gWebImageScratchPixels = pixelCount;
  return true;
}

static bool ensureWebImageLineBuffer(uint16_t width) {
  if (width == 0 || width > kWebImageMaxWidth) return false;
  if (gWebImageLineBuffer && gWebImageLineBufferPixels >= width) return true;

  const size_t bytes = static_cast<size_t>(width) * sizeof(uint16_t);
  uint16_t *next = static_cast<uint16_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (!next) {
    next = static_cast<uint16_t *>(malloc(bytes));
  }
  if (!next) return false;

  if (gWebImageLineBuffer) {
    free(gWebImageLineBuffer);
  }
  gWebImageLineBuffer = next;
  gWebImageLineBufferPixels = width;
  return true;
}

static void showWebInterruptImage(const uint16_t *pixels,
                                  uint16_t width,
                                  uint16_t height,
                                  int16_t centerX,
                                  int16_t centerY) {
  if (!pixels || width == 0 || height == 0) return;
  if (width > tft.width() || height > tft.height()) return;

  int drawX = static_cast<int>(centerX) - static_cast<int>(width) / 2;
  int drawY = static_cast<int>(centerY) - static_cast<int>(height) / 2;

  if (drawX < 0) drawX = 0;
  if (drawY < 0) drawY = 0;
  if (drawX + static_cast<int>(width) > tft.width()) {
    drawX = tft.width() - static_cast<int>(width);
  }
  if (drawY + static_cast<int>(height) > tft.height()) {
    drawY = tft.height() - static_cast<int>(height);
  }
  if (drawX < 0 || drawY < 0) return;
  if (!ensureWebImageLineBuffer(width)) {
    Serial.println("[WEB] image line buffer alloc failed");
    return;
  }

  tft.fillScreen(TFT_BLACK);
  for (uint16_t row = 0; row < height; ++row) {
    memcpy(gWebImageLineBuffer,
           pixels + static_cast<size_t>(row) * static_cast<size_t>(width),
           static_cast<size_t>(width) * sizeof(uint16_t));
    tft.pushImage(drawX, drawY + row, width, 1, gWebImageLineBuffer);
  }
}

static AppLoopMode gAppLoopMode = APP_MODE_AP_STA;
static AppModeEnterCallback gAppModeEnterCallback = nullptr;
static AppModeEnterCallback gAppModeInitCallbacks[3] = {nullptr, nullptr, nullptr};
static bool gAppModeEnterPending = true;
static bool gSkipStartupPromptOnce = true;

static bool consumeStartupPromptSkip(AppLoopMode mode) {
  if (!gSkipStartupPromptOnce) return false;
  gSkipStartupPromptOnce = false;
  Serial.printf("[BOOT] startup mode=%s, skip prompt once\n", appModeToIniValue(mode));
  return true;
}

enum class StaOnlinePhase : uint8_t {
  kPromptWaitShort = 0,
  kConnecting = 1,
  kFailWaitShort = 2,
  kConnected = 3,
  kDisconnectedWaitShort = 4,
};

static StaOnlinePhase gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
static String gStaNetSsid;
static String gStaNetPassword;
static String gStaNetApi;
static uint8_t gStaRetryCount = 0;
static uint32_t gStaAttemptStartMs = 0;
static constexpr uint32_t kStaAttemptTimeoutMs = 10000UL;
static constexpr uint8_t kStaMaxRetryCount = 5;
static constexpr const char *kStaPromptMsg =
    u8"模式:联网 | 短按开始连接WiFi";
static constexpr const char *kStaMissingCfgMsg =
    u8"WIFI\u914D\u7F6E\u7F3A\u5931\uFF1A\u77ED\u6309\u5207\u6362\u6A21\u5F0F";
static constexpr const char *kStaConnectingPrefix =
    u8"\u6B63\u5728\u8FDE\u63A5\uFF1A";
static constexpr const char *kStaRetryPrefix =
    u8"|重试次数";
static constexpr const char *kStaConnectOkPrefix =
    u8"WIFI连接成功";
static constexpr const char *kStaConnectFailMsg =
    u8"WIFI\u8FDE\u63A5\u5931\u8D25\uFF1A\u77ED\u6309\u5207\u6362\u6A21\u5F0F";
static constexpr const char *kStaCloudQueueEmptyMsg =
    u8"正在连接都市神经网络...";
static constexpr const char *kStaDisconnectedMsg =
    u8"WIFI已断开，按键重新连接";

static constexpr const char *kApPromptMsg =
    u8"模式：正常 | 热点已启动";

static constexpr const char *kStaonlyPromptMsg =
    u8"模式：省电 | 无线功能已禁用";

static constexpr const char *kStaCloudApiUrlDefault = "http://115.190.145.254:8080/random";
static constexpr size_t kStaPrefetchDepth = 20;
static_assert(kStaPrefetchDepth == kSleepStaQueueMax,
              "kSleepStaQueueMax must match kStaPrefetchDepth");
static constexpr uint32_t kStaQueueEmptyHintCooldownMs = 1800UL;
static constexpr uint32_t kStaFetchFailCooldownMs = 1000UL;
static constexpr uint32_t kStaFetcherTickMs = 200UL;
static constexpr uint32_t kStaHttpConnectTimeoutMs = 3500UL;
static constexpr uint32_t kStaHttpReadTimeoutMs = 4500UL;
static constexpr uint32_t kStaNtpPollTimeoutMs = 6000UL;
static constexpr uint32_t kStaNtpAttemptCooldownMs = 10000UL;
static constexpr int32_t kStaNtpGmtOffsetSec = 8 * 3600;
static constexpr const char *kStaNtpServer1 = "ntp.ntsc.ac.cn";
static constexpr const char *kStaNtpServer2 = "cn.pool.ntp.org";
static constexpr const char *kStaNtpServer3 = "pool.ntp.org";
static constexpr UBaseType_t kStaFetcherPriority = 1;
static constexpr BaseType_t kStaFetcherCore = 0;
static constexpr uint32_t kStaFetcherStackSize = 12288UL;
static String gStaMsgQueue[kStaPrefetchDepth];
static size_t gStaMsgQueueHead = 0;
static size_t gStaMsgQueueSize = 0;
static uint32_t gStaLastQueueEmptyHintMs = 0;
static uint32_t gStaNextFetchAllowedMs = 0;
static volatile bool gStaFetchInProgress = false;
static WiFiClientSecure gStaHttpsClient;
static bool gStaHttpsClientReady = false;
static SemaphoreHandle_t gStaMsgQueueMutex = nullptr;
static TaskHandle_t gStaFetcherTaskHandle = nullptr;
static bool gStaNtpSyncedThisSession = false;
static uint32_t gStaNtpLastAttemptMs = 0;

struct SleepSnapshotData {
  SleepSnapshotHeader header;
  std::vector<String> staQueue;
  std::vector<String> regularQueue;
  std::vector<String> hostQueue;
};

uint64_t absDiffU64(uint64_t a, uint64_t b) {
  return (a >= b) ? (a - b) : (b - a);
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

bool ds1302DateTimeToUnix(const Ds1302DateTime &dt, uint64_t &outUnix) {
  if (!ds1302IsValidDateTime(dt)) return false;
  const int64_t days = daysFromCivil(static_cast<int>(dt.year), dt.month, dt.day);
  const int64_t seconds = days * 86400LL + static_cast<int64_t>(dt.hour) * 3600LL +
                          static_cast<int64_t>(dt.minute) * 60LL + static_cast<int64_t>(dt.second);
  if (seconds < 0) return false;
  outUnix = static_cast<uint64_t>(seconds);
  return true;
}

bool readRtcUnix(uint64_t &outUnix) {
  Ds1302DateTime dt;
  if (!rtc.readDateTime(dt)) return false;
  if (!ds1302IsValidDateTime(dt)) return false;
  return ds1302DateTimeToUnix(dt, outUnix);
}

struct ReminderSchedule {
  struct Entry {
    uint16_t sourceIndex = 0;
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t week = 0;  // Monday=1 ... Sunday=7
    bool repeatDay = false;
    bool repeatMonth = false;
    bool repeatWeek = false;
    bool repeatYear = false;
  };

  Entry entries[kMaxReminderTimes];
  size_t count = 0;
};

ReminderSchedule &scheduleScratchBuffer() {
  static ReminderSchedule schedule;
  return schedule;
}

bool isLeapYearLocal(uint16_t year) {
  if ((year % 4U) != 0U) return false;
  if ((year % 100U) != 0U) return true;
  return (year % 400U) == 0U;
}

uint8_t maxDayInMonthLocal(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYearLocal(year)) return 29;
  return kDays[month - 1];
}

void civilFromDays(int64_t z, int &year, unsigned &month, unsigned &day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - static_cast<int64_t>(era) * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += (month <= 2);
}

bool unixToDs1302DateTime(uint64_t unixSeconds, Ds1302DateTime &out) {
  const uint64_t days = unixSeconds / 86400ULL;
  const uint32_t secOfDay = static_cast<uint32_t>(unixSeconds % 86400ULL);
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(static_cast<int64_t>(days), year, month, day);

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(secOfDay / 3600U);
  out.minute = static_cast<uint8_t>((secOfDay % 3600U) / 60U);
  out.second = static_cast<uint8_t>(secOfDay % 60U);
  return ds1302IsValidDateTime(out);
}

uint8_t weekdayMondayOneFromDays(int64_t daysSinceEpoch) {
  int weekday = static_cast<int>((daysSinceEpoch + 3LL) % 7LL);  // 1970-01-01 is Thursday.
  if (weekday < 0) weekday += 7;
  return static_cast<uint8_t>(weekday + 1);  // Monday=1 ... Sunday=7
}

uint8_t weekdayMondayOneFromUnix(uint64_t unixSeconds) {
  return weekdayMondayOneFromDays(static_cast<int64_t>(unixSeconds / 86400ULL));
}

uint8_t weekdayMondayOneFromCivil(uint16_t year, uint8_t month, uint8_t day) {
  return weekdayMondayOneFromDays(daysFromCivil(static_cast<int>(year), month, day));
}

bool parseStrictInt(String value, int &out) {
  value.trim();
  if (!value.length()) return false;
  int start = 0;
  if (value[0] == '+' || value[0] == '-') {
    if (value.length() == 1) return false;
    start = 1;
  }
  for (int i = start; i < value.length(); ++i) {
    if (!isDigit(value[i])) return false;
  }
  out = value.toInt();
  return true;
}

String decodeScheduleMessageValue(String value) {
  value.trim();
  if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"') {
    value = value.substring(1, value.length() - 1);
    value.replace("\"\"", "\"");
  }
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();
  return value;
}

bool parseScheduleCsvLine(const String &lineRaw,
                          ReminderSchedule::Entry &outEntry,
                          String *outMessage) {
  String line = lineRaw;
  line.trim();
  if (!line.length()) return false;
  if (line.startsWith("#") || line.startsWith(";")) return false;
  if (outMessage) {
    *outMessage = "";
  }

  String cols[12];
  size_t colCount = 0;
  int start = 0;
  while (start <= line.length()) {
    if (colCount >= 11) {
      cols[colCount++] = line.substring(start);
      cols[colCount - 1].trim();
      break;
    }
    const int comma = line.indexOf(',', start);
    String token = (comma >= 0) ? line.substring(start, comma) : line.substring(start);
    token.trim();
    cols[colCount++] = token;
    if (comma < 0) break;
    start = comma + 1;
  }

  if (colCount < 11) return false;

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int week = 0;
  int repeatDay = 0;
  int repeatMonth = 0;
  int repeatWeek = 0;
  int repeatYear = 0;

  if (!parseStrictInt(cols[0], year) || year < 2000 || year > 2099) return false;
  if (!parseStrictInt(cols[1], month) || month < 1 || month > 12) return false;
  if (!parseStrictInt(cols[2], day) || day < 1 || day > 31) return false;
  if (!parseStrictInt(cols[3], hour) || hour < 0 || hour > 23) return false;
  if (!parseStrictInt(cols[4], minute) || minute < 0 || minute > 59) return false;
  if (!parseStrictInt(cols[5], second) || second < 0 || second > 59) return false;
  if (!parseStrictInt(cols[6], week) || week < 0 || week > 7) return false;
  if (!parseStrictInt(cols[7], repeatDay) || (repeatDay != 0 && repeatDay != 1)) return false;
  if (!parseStrictInt(cols[8], repeatMonth) || (repeatMonth != 0 && repeatMonth != 1)) return false;
  if (!parseStrictInt(cols[9], repeatWeek) || (repeatWeek != 0 && repeatWeek != 1)) return false;
  if (!parseStrictInt(cols[10], repeatYear) || (repeatYear != 0 && repeatYear != 1)) return false;

  Ds1302DateTime base;
  base.year = static_cast<uint16_t>(year);
  base.month = static_cast<uint8_t>(month);
  base.day = static_cast<uint8_t>(day);
  base.hour = static_cast<uint8_t>(hour);
  base.minute = static_cast<uint8_t>(minute);
  base.second = static_cast<uint8_t>(second);
  if (!ds1302IsValidDateTime(base)) return false;

  outEntry.year = base.year;
  outEntry.month = base.month;
  outEntry.day = base.day;
  outEntry.hour = base.hour;
  outEntry.minute = base.minute;
  outEntry.second = 0;  // Minute precision: ignore second field from CSV.
  outEntry.week = (week == 0)
                      ? weekdayMondayOneFromCivil(outEntry.year, outEntry.month, outEntry.day)
                      : static_cast<uint8_t>(week);
  outEntry.repeatDay = (repeatDay != 0);
  outEntry.repeatMonth = (repeatMonth != 0);
  outEntry.repeatWeek = (repeatWeek != 0);
  outEntry.repeatYear = (repeatYear != 0);

  if (outMessage) {
    String decoded = (colCount > 11) ? decodeScheduleMessageValue(cols[11]) : String();
    if (!decoded.length()) {
      decoded = kDefaultReminderMessage;
    }
    *outMessage = decoded;
  }
  return true;
}

bool openScheduleCsvRead(fs::File &outFile, bool &outMountedTemp) {
  outMountedTemp = false;
  if (!fatMounted) {
    if (!mountFat()) {
      return false;
    }
    outMountedTemp = true;
  }

  outFile = FFat.open("/schedule.csv", FILE_READ);
  if (!outFile) {
    if (outMountedTemp) {
      unmountFat();
      outMountedTemp = false;
    }
    return false;
  }
  return true;
}

bool loadReminderSchedule(ReminderSchedule &outSchedule) {
  outSchedule.count = 0;

  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp)) return false;

  while (f.available() && outSchedule.count < kMaxReminderTimes) {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    if (parseScheduleCsvLine(line, entry, nullptr)) {
      entry.sourceIndex = static_cast<uint16_t>(outSchedule.count);
      outSchedule.entries[outSchedule.count++] = entry;
    }
  }
  f.close();

  if (mountedTemp) {
    unmountFat();
  }

  return outSchedule.count > 0;
}

uint32_t dateKeyFromDateTime(const Ds1302DateTime &dt) {
  return static_cast<uint32_t>(dt.year) * 10000U +
         static_cast<uint32_t>(dt.month) * 100U +
         static_cast<uint32_t>(dt.day);
}

bool reminderEntryMatchesToday(const ReminderSchedule::Entry &entry,
                               const Ds1302DateTime &today,
                               uint8_t todayWeek) {
  const bool anyRepeat = entry.repeatDay || entry.repeatMonth || entry.repeatWeek || entry.repeatYear;
  if (!anyRepeat) {
    return entry.year == today.year &&
           entry.month == today.month &&
           entry.day == today.day;
  }
  if (entry.repeatDay) return true;
  if (entry.repeatWeek && entry.week == todayWeek) return true;
  if (entry.repeatMonth && entry.day == today.day) return true;
  if (entry.repeatYear && entry.month == today.month && entry.day == today.day) return true;
  return false;
}

bool loadReminderMessageBySourceIndex(uint16_t sourceIndex, String &outMessage) {
  outMessage = "";
  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp)) return false;

  uint16_t currentIndex = 0;
  while (f.available()) {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    String message;
    if (!parseScheduleCsvLine(line, entry, &message)) continue;

    if (currentIndex == sourceIndex) {
      f.close();
      if (mountedTemp) {
        unmountFat();
      }
      message.trim();
      if (!message.length()) {
        message = kDefaultReminderMessage;
      }
      outMessage = message;
      return true;
    }
    ++currentIndex;
  }

  f.close();
  if (mountedTemp) {
    unmountFat();
  }
  return false;
}

bool loadReminderMessageByDateMinute(const Ds1302DateTime &today,
                                     uint8_t hour,
                                     uint8_t minute,
                                     String &outMessage) {
  outMessage = "";
  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp)) return false;

  const uint8_t todayWeek = weekdayMondayOneFromCivil(today.year, today.month, today.day);
  while (f.available()) {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    String message;
    if (!parseScheduleCsvLine(line, entry, &message)) continue;
    if (!reminderEntryMatchesToday(entry, today, todayWeek)) continue;
    if (entry.hour != hour || entry.minute != minute) continue;

    f.close();
    if (mountedTemp) {
      unmountFat();
    }
    message.trim();
    if (!message.length()) {
      message = kDefaultReminderMessage;
    }
    outMessage = message;
    return true;
  }

  f.close();
  if (mountedTemp) {
    unmountFat();
  }
  return false;
}

void refreshTodayReminderSlots(const Ds1302DateTime &nowDt, bool markPastTriggered) {
  ReminderSchedule &schedule = scheduleScratchBuffer();
  const bool loaded = loadReminderSchedule(schedule);
  gTodayReminderCount = 0;
  gTodayReminderDateKey = dateKeyFromDateTime(nowDt);
  gTodayReminderLastCheckedMinute = 0xFFFFU;

  if (!loaded) {
    Serial.printf("[SCHEDULE] today slot refresh: no schedule (%04u-%02u-%02u)\n",
                  static_cast<unsigned int>(nowDt.year),
                  static_cast<unsigned int>(nowDt.month),
                  static_cast<unsigned int>(nowDt.day));
    return;
  }

  const uint8_t todayWeek = weekdayMondayOneFromCivil(nowDt.year, nowDt.month, nowDt.day);
  for (size_t i = 0; i < schedule.count && gTodayReminderCount < kMaxReminderTimes; ++i) {
    const ReminderSchedule::Entry &entry = schedule.entries[i];
    if (!reminderEntryMatchesToday(entry, nowDt, todayWeek)) continue;
    DailyReminderSlot &slot = gTodayReminderSlots[gTodayReminderCount];
    slot.hour = entry.hour;
    slot.minute = entry.minute;
    slot.order = static_cast<uint16_t>(gTodayReminderCount);
    slot.triggered = false;
    ++gTodayReminderCount;
  }

  std::sort(gTodayReminderSlots,
            gTodayReminderSlots + gTodayReminderCount,
            [](const DailyReminderSlot &a, const DailyReminderSlot &b) {
              const uint16_t aMin = static_cast<uint16_t>(a.hour) * 60U + static_cast<uint16_t>(a.minute);
              const uint16_t bMin = static_cast<uint16_t>(b.hour) * 60U + static_cast<uint16_t>(b.minute);
              if (aMin != bMin) return aMin < bMin;
              return a.order < b.order;
            });

  if (markPastTriggered) {
    const uint16_t nowMinuteOfDay =
        static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
    for (size_t i = 0; i < gTodayReminderCount; ++i) {
      const uint16_t slotMinuteOfDay =
          static_cast<uint16_t>(gTodayReminderSlots[i].hour) * 60U +
          static_cast<uint16_t>(gTodayReminderSlots[i].minute);
      if (slotMinuteOfDay < nowMinuteOfDay) {
        gTodayReminderSlots[i].triggered = true;
      }
    }
  }

  Serial.printf("[SCHEDULE] today slot refresh count=%u (%04u-%02u-%02u)\n",
                static_cast<unsigned int>(gTodayReminderCount),
                static_cast<unsigned int>(nowDt.year),
                static_cast<unsigned int>(nowDt.month),
                static_cast<unsigned int>(nowDt.day));
}

bool refreshTodayReminderSlotsFromRtc(bool forceReload, bool markPastTriggered) {
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt)) {
    return false;
  }
  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (!forceReload && gTodayReminderDateKey == dateKey) {
    return true;
  }
  refreshTodayReminderSlots(nowDt, markPastTriggered);
  return true;
}

void serviceScheduleInterruptByRtcMinute() {
  const bool scheduleChanged = wirelessPortalConsumeScheduleReloadRequest();
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt)) {
    return;
  }

  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (scheduleChanged || gTodayReminderDateKey != dateKey) {
    refreshTodayReminderSlots(nowDt, true);
  }

  if (gTodayReminderDateKey != dateKey || gTodayReminderCount == 0) {
    return;
  }

  const uint16_t nowMinuteOfDay =
      static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
  if (gTodayReminderLastCheckedMinute == nowMinuteOfDay) {
    return;
  }
  gTodayReminderLastCheckedMinute = nowMinuteOfDay;

  for (size_t i = 0; i < gTodayReminderCount; ++i) {
    DailyReminderSlot &slot = gTodayReminderSlots[i];
    if (slot.triggered) continue;
    const uint16_t slotMinuteOfDay =
        static_cast<uint16_t>(slot.hour) * 60U + static_cast<uint16_t>(slot.minute);
    if (slotMinuteOfDay < nowMinuteOfDay) {
      slot.triggered = true;
      continue;
    }
    if (slot.hour == nowDt.hour && slot.minute == nowDt.minute) {
      slot.triggered = true;
      String reminderMessage;
      if (!loadReminderMessageByDateMinute(nowDt, slot.hour, slot.minute, reminderMessage)) {
        reminderMessage = kDefaultReminderMessage;
      }
      enqueueScheduleInterruptMessage(reminderMessage);
      Serial.printf("[SCHEDULE] due now %02u:%02u\n",
                    static_cast<unsigned int>(slot.hour),
                    static_cast<unsigned int>(slot.minute));
    }
  }
}

static bool markCurrentMinuteScheduleAsTriggeredFromRtc() {
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt)) {
    return false;
  }

  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (gTodayReminderDateKey != dateKey) {
    refreshTodayReminderSlots(nowDt, true);
  }

  const uint16_t nowMinuteOfDay =
      static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
  gTodayReminderLastCheckedMinute = nowMinuteOfDay;

  if (gTodayReminderDateKey != dateKey) {
    return false;
  }

  for (size_t i = 0; i < gTodayReminderCount; ++i) {
    DailyReminderSlot &slot = gTodayReminderSlots[i];
    if (slot.hour == nowDt.hour && slot.minute == nowDt.minute) {
      slot.triggered = true;
    }
  }
  return true;
}

bool computeNextReminderDelta(const ReminderSchedule &schedule,
                              uint64_t nowUnix,
                              uint32_t &outDeltaSec,
                              char outMessage[kSleepTextMaxLen + 1]) {
  if (schedule.count == 0) return false;

  Ds1302DateTime nowDt;
  if (!unixToDs1302DateTime(nowUnix, nowDt)) return false;
  const uint32_t nowSecOfDay = static_cast<uint32_t>(nowDt.hour) * 3600U +
                               static_cast<uint32_t>(nowDt.minute) * 60U +
                               static_cast<uint32_t>(nowDt.second);
  const uint64_t dayStartUnix = nowUnix - static_cast<uint64_t>(nowSecOfDay);
  const uint8_t nowWeek = weekdayMondayOneFromUnix(nowUnix);
  uint32_t bestDelta = std::numeric_limits<uint32_t>::max();
  uint16_t bestSourceIndex = 0xFFFFU;

  auto considerCandidate = [&](uint64_t candidateUnix, uint16_t sourceIndex) {
    uint32_t delta = 0;
    if (candidateUnix >= nowUnix) {
      const uint64_t diff = candidateUnix - nowUnix;
      if (diff > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) return;
      delta = static_cast<uint32_t>(diff);
    } else {
      const uint64_t late = nowUnix - candidateUnix;
      if (late > kReminderTriggerWindowSec) return;
      delta = 0;
    }
    if (delta < bestDelta || (delta == bestDelta && sourceIndex < bestSourceIndex)) {
      bestDelta = delta;
      bestSourceIndex = sourceIndex;
    }
  };

  auto makeUnix = [](int year, int month, int day, int hour, int minute, int second,
                     uint64_t &outUnix) -> bool {
    if (month < 1 || month > 12) return false;
    const uint8_t maxDay = maxDayInMonthLocal(static_cast<uint16_t>(year),
                                              static_cast<uint8_t>(month));
    if (maxDay == 0) return false;
    if (day < 1) day = 1;
    if (day > maxDay) day = maxDay;
    Ds1302DateTime dt;
    dt.year = static_cast<uint16_t>(year);
    dt.month = static_cast<uint8_t>(month);
    dt.day = static_cast<uint8_t>(day);
    dt.hour = static_cast<uint8_t>(hour);
    dt.minute = static_cast<uint8_t>(minute);
    dt.second = static_cast<uint8_t>(second);
    if (!ds1302IsValidDateTime(dt)) return false;
    return ds1302DateTimeToUnix(dt, outUnix);
  };

  for (size_t i = 0; i < schedule.count; ++i) {
    const ReminderSchedule::Entry &entry = schedule.entries[i];
    const uint32_t entrySecOfDay = static_cast<uint32_t>(entry.hour) * 3600U +
                                   static_cast<uint32_t>(entry.minute) * 60U;

    const bool anyRepeat = entry.repeatDay || entry.repeatMonth || entry.repeatWeek || entry.repeatYear;
    if (!anyRepeat) {
      uint64_t candidateUnix = 0;
      if (makeUnix(entry.year, entry.month, entry.day,
                   entry.hour, entry.minute, 0, candidateUnix)) {
        considerCandidate(candidateUnix, entry.sourceIndex);
      }
      continue;
    }

    if (entry.repeatDay) {
      uint64_t candidateUnix = dayStartUnix + static_cast<uint64_t>(entrySecOfDay);
      if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix) {
        candidateUnix += 86400ULL;
      }
      considerCandidate(candidateUnix, entry.sourceIndex);
    }

    if (entry.repeatWeek) {
      uint8_t targetWeek = entry.week;
      if (targetWeek < 1 || targetWeek > 7) {
        targetWeek = weekdayMondayOneFromCivil(entry.year, entry.month, entry.day);
      }
      int dayOffset = static_cast<int>(targetWeek) - static_cast<int>(nowWeek);
      if (dayOffset < 0) dayOffset += 7;
      uint64_t candidateUnix = dayStartUnix +
                               static_cast<uint64_t>(dayOffset) * 86400ULL +
                               static_cast<uint64_t>(entrySecOfDay);
      if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix) {
        candidateUnix += 7ULL * 86400ULL;
      }
      considerCandidate(candidateUnix, entry.sourceIndex);
    }

    if (entry.repeatMonth) {
      int targetYear = nowDt.year;
      int targetMonth = nowDt.month;
      uint64_t candidateUnix = 0;
      if (makeUnix(targetYear, targetMonth, entry.day,
                   entry.hour, entry.minute, 0, candidateUnix)) {
        if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix) {
          targetMonth += 1;
          if (targetMonth > 12) {
            targetMonth = 1;
            targetYear += 1;
          }
          if (makeUnix(targetYear, targetMonth, entry.day,
                       entry.hour, entry.minute, 0, candidateUnix)) {
            considerCandidate(candidateUnix, entry.sourceIndex);
          }
        } else {
          considerCandidate(candidateUnix, entry.sourceIndex);
        }
      }
    }

    if (entry.repeatYear) {
      int targetYear = nowDt.year;
      uint64_t candidateUnix = 0;
      if (makeUnix(targetYear, entry.month, entry.day,
                   entry.hour, entry.minute, 0, candidateUnix)) {
        if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix) {
          targetYear += 1;
          if (makeUnix(targetYear, entry.month, entry.day,
                       entry.hour, entry.minute, 0, candidateUnix)) {
            considerCandidate(candidateUnix, entry.sourceIndex);
          }
        } else {
          considerCandidate(candidateUnix, entry.sourceIndex);
        }
      }
    }
  }

  if (bestDelta == std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  outDeltaSec = bestDelta;
  if (outMessage) {
    String message;
    if (!loadReminderMessageBySourceIndex(bestSourceIndex, message)) {
      message = kDefaultReminderMessage;
    }
    sanitizeMessageForSnapshot(message, outMessage);
  }
  return true;
}

uint32_t chooseRtcRefillStepSec(uint64_t diffSec) {
  if (diffSec >= 1800ULL) return 1800U;
  if (diffSec >= 600ULL) return 600U;
  if (diffSec >= 300ULL) return 300U;
  if (diffSec >= 60ULL) return 60U;
  return 10U;
}

void appendSnapshotMessage(std::vector<String> &out, const String &raw, size_t cap) {
  if (out.size() >= cap) return;
  String normalized = raw;
  normalized.replace("\r", " ");
  normalized.replace("\n", " ");
  normalized.trim();
  if (!normalized.length()) return;
  if (normalized.length() > kSleepTextMaxLen) {
    normalized.remove(kSleepTextMaxLen);
  }
  out.push_back(normalized);
}

bool writeFixedMessage(fs::File &f, const String &msg) {
  char fixed[kSleepTextMaxLen + 1] = {0};
  sanitizeMessageForSnapshot(msg, fixed);
  return writeExact(f, fixed, sizeof(fixed));
}

bool readFixedMessage(fs::File &f, String &out) {
  char fixed[kSleepTextMaxLen + 1] = {0};
  if (!readExact(f, fixed, sizeof(fixed))) return false;
  fixed[kSleepTextMaxLen] = '\0';
  out = String(fixed);
  out.trim();
  return true;
}

void restorePortalQueuesFromCapture(const std::vector<String> &regularQueue,
                                    bool hasImmediate,
                                    const String &immediateMessage,
                                    const std::vector<String> &hostQueue) {
  for (const String &msg : regularQueue) {
    (void)wirelessPortalPushMessageForRestore(msg);
  }
  if (hasImmediate) {
    (void)wirelessPortalPushImmediateMessageForRestore(immediateMessage);
  }
  for (const String &msg : hostQueue) {
    (void)wirelessPortalPushHostMessageForRestore(msg);
  }
}

bool saveSleepSnapshotToFat() {
  if (!fatMounted) {
    Serial.println("[SLEEP] FAT not mounted, skip snapshot");
    return false;
  }

  SleepSnapshotHeader header;
  header.magic = kSleepFileMagic;
  header.version = kSleepFileVersion;
  header.appMode = static_cast<uint8_t>(gAppLoopMode);
  header.runState = RUNSTATE;
  header.firstFlag = firstFlag ? 1U : 0U;
  header.staPhase = static_cast<uint8_t>(gStaOnlinePhase);
  header.csvCount = csvCount;
  header.staRetryCount = gStaRetryCount;
  header.backlightTimeSec = gBacklightTimeSec;
  sanitizeMessageForSnapshot(String(gLastDisplayedText), header.lastDisplayed);

  std::vector<String> staQueue;
  staQueue.reserve(kSleepStaQueueMax);
  if (ensureStaQueueMutex() &&
      xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    const size_t count = std::min(gStaMsgQueueSize, kSleepStaQueueMax);
    for (size_t i = 0; i < count; ++i) {
      const size_t idx = (gStaMsgQueueHead + i) % kStaPrefetchDepth;
      appendSnapshotMessage(staQueue, gStaMsgQueue[idx], kSleepStaQueueMax);
    }
    xSemaphoreGive(gStaMsgQueueMutex);
  }

  std::vector<String> regularQueue;
  std::vector<String> hostQueue;
  regularQueue.reserve(kSleepPortalQueueMax);
  hostQueue.reserve(kSleepPortalQueueMax);
  String tmp;
  while (regularQueue.size() < kSleepPortalQueueMax && wirelessPortalPopMessage(tmp)) {
    appendSnapshotMessage(regularQueue, tmp, kSleepPortalQueueMax);
  }
  while (hostQueue.size() < kSleepPortalQueueMax && wirelessPortalPopHostMessage(tmp)) {
    appendSnapshotMessage(hostQueue, tmp, kSleepPortalQueueMax);
  }
  String immediateMessage;
  const bool hasImmediate = wirelessPortalPopImmediateMessage(immediateMessage);
  if (hasImmediate) {
    sanitizeMessageForSnapshot(immediateMessage, header.immediateMessage);
    header.hasImmediate = 1U;
  }

  header.staQueueCount = static_cast<uint8_t>(staQueue.size());
  header.regularQueueCount = static_cast<uint8_t>(regularQueue.size());
  header.hostQueueCount = static_cast<uint8_t>(hostQueue.size());

  fs::File f = FFat.open(kSleepSnapshotPath, "w");
  if (!f) {
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] open snapshot file failed");
    return false;
  }

  bool ok = writeExact(f, &header, sizeof(header));
  if (ok) {
    ok = writeExact(f, csvArray, sizeof(csvArray));
  }
  if (ok) {
    for (const String &msg : staQueue) {
      if (!writeFixedMessage(f, msg)) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    for (const String &msg : regularQueue) {
      if (!writeFixedMessage(f, msg)) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    for (const String &msg : hostQueue) {
      if (!writeFixedMessage(f, msg)) {
        ok = false;
        break;
      }
    }
  }
  f.close();

  if (!ok) {
    (void)FFat.remove(kSleepSnapshotPath);
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] snapshot write failed");
    return false;
  }

  Serial.printf("[SLEEP] snapshot saved mode=%u staQ=%u webQ=%u hostQ=%u\n",
                static_cast<unsigned int>(header.appMode),
                static_cast<unsigned int>(header.staQueueCount),
                static_cast<unsigned int>(header.regularQueueCount),
                static_cast<unsigned int>(header.hostQueueCount));
  return true;
}

bool loadSleepSnapshotFromFat(SleepSnapshotData &outData) {
  if (!fatMounted) return false;
  fs::File f = FFat.open(kSleepSnapshotPath, FILE_READ);
  if (!f) return false;

  SleepSnapshotHeader header;
  if (!readExact(f, &header, sizeof(header))) {
    f.close();
    return false;
  }
  if (header.magic != kSleepFileMagic || header.version != kSleepFileVersion) {
    f.close();
    return false;
  }
  if (header.staQueueCount > kSleepStaQueueMax ||
      header.regularQueueCount > kSleepPortalQueueMax ||
      header.hostQueueCount > kSleepPortalQueueMax) {
    f.close();
    return false;
  }
  if (!readExact(f, csvArray, sizeof(csvArray))) {
    f.close();
    return false;
  }

  outData.header = header;
  outData.staQueue.clear();
  outData.regularQueue.clear();
  outData.hostQueue.clear();
  outData.staQueue.reserve(header.staQueueCount);
  outData.regularQueue.reserve(header.regularQueueCount);
  outData.hostQueue.reserve(header.hostQueueCount);

  String item;
  for (uint8_t i = 0; i < header.staQueueCount; ++i) {
    if (!readFixedMessage(f, item)) {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.staQueue, item, kSleepStaQueueMax);
  }
  for (uint8_t i = 0; i < header.regularQueueCount; ++i) {
    if (!readFixedMessage(f, item)) {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.regularQueue, item, kSleepPortalQueueMax);
  }
  for (uint8_t i = 0; i < header.hostQueueCount; ++i) {
    if (!readFixedMessage(f, item)) {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.hostQueue, item, kSleepPortalQueueMax);
  }

  f.close();
  return true;
}

AppLoopMode decodeSnapshotMode(uint8_t rawMode) {
  switch (rawMode) {
    case APP_MODE_AP_STA:
    case APP_MODE_STA_ONLINE:
    case APP_MODE_STA_ONLY:
      return static_cast<AppLoopMode>(rawMode);
    default:
      return APP_MODE_AP_STA;
  }
}

StaOnlinePhase decodeSnapshotStaPhase(uint8_t rawPhase) {
  switch (rawPhase) {
    case static_cast<uint8_t>(StaOnlinePhase::kPromptWaitShort):
    case static_cast<uint8_t>(StaOnlinePhase::kConnecting):
    case static_cast<uint8_t>(StaOnlinePhase::kFailWaitShort):
    case static_cast<uint8_t>(StaOnlinePhase::kConnected):
    case static_cast<uint8_t>(StaOnlinePhase::kDisconnectedWaitShort):
      return static_cast<StaOnlinePhase>(rawPhase);
    default:
      return StaOnlinePhase::kPromptWaitShort;
  }
}

bool applySleepSnapshot(const SleepSnapshotData &snapshot, bool replayLastDisplayed) {
  gAppLoopMode = decodeSnapshotMode(snapshot.header.appMode);
  gAppModeEnterPending = false;

  RUNSTATE = snapshot.header.runState;
  firstFlag = snapshot.header.firstFlag != 0;
  csvCount = snapshot.header.csvCount;
  if (csvCount < 0) csvCount = 0;
  if (csvCount > kCsvArrayCapacity) csvCount = kCsvArrayCapacity;

  gStaOnlinePhase = decodeSnapshotStaPhase(snapshot.header.staPhase);
  gStaRetryCount = snapshot.header.staRetryCount;
  gStaAttemptStartMs = millis();
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;

  wirelessPortalStop();
  bool portalOk = false;
  if (gAppLoopMode == APP_MODE_STA_ONLY) {
    WiFi.disconnect(true, false);
    portalOk = wirelessPortalStartEspNowOnly();
  } else {
    portalOk = wirelessPortalStart();
  }
  if (!portalOk) {
    Serial.println("[SLEEP] restore portal start failed");
  }

  clearStaMessageQueue();
  for (const String &msg : snapshot.staQueue) {
    (void)pushStaMessageQueue(msg);
  }

  for (const String &msg : snapshot.regularQueue) {
    (void)wirelessPortalPushMessageForRestore(msg);
  }
  if (snapshot.header.hasImmediate) {
    (void)wirelessPortalPushImmediateMessageForRestore(String(snapshot.header.immediateMessage));
  }
  for (const String &msg : snapshot.hostQueue) {
    (void)wirelessPortalPushHostMessageForRestore(msg);
  }

  if (gAppLoopMode == APP_MODE_STA_ONLINE) {
    (void)loadStaCredentialsFromSettingIni(gStaNetSsid, gStaNetPassword, gStaNetApi);
    ensureStaFetcherTaskStarted();
    if (gStaOnlinePhase == StaOnlinePhase::kConnecting && gStaNetSsid.length()) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gStaNetSsid.c_str(), gStaNetPassword.c_str());
    }
  } else {
    gStaNetSsid = "";
    gStaNetPassword = "";
    gStaNetApi = kStaCloudApiUrlDefault;
  }

  gBacklightTimeSec = snapshot.header.backlightTimeSec;
  setBacklightTimeSeconds(gBacklightTimeSec);
  notifyBacklightActivity();

  rememberLastDisplayedText(snapshot.header.lastDisplayed);
  if (replayLastDisplayed && snapshot.header.lastDisplayed[0]) {
    playMessageWithGlitch(snapshot.header.lastDisplayed);
  }

  return true;
}

void markSleepRtcContextForSleep() {
  gSleepRtcCtx.magic = kSleepRtcCtxMagic;
  gSleepRtcCtx.version = kSleepRtcCtxVersion;
  gSleepRtcCtx.snapshotValid = 1;
  gSleepRtcCtx.appMode = static_cast<uint8_t>(gAppLoopMode);
  gSleepRtcCtx.nextWakeSec = kRtcWakeDefaultSec;
  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';

  uint64_t nowUnix = 0;
  if (!readRtcUnix(nowUnix)) {
    gSleepRtcCtx.expectedUnix = 0;
    return;
  }
  gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(kRtcWakeDefaultSec);
}

void waitWakeKeyReleaseBeforeSleep() {
  pinMode(static_cast<uint8_t>(kWakeKeyGpio), INPUT_PULLUP);
  const uint32_t started = millis();
  while (digitalRead(static_cast<uint8_t>(kWakeKeyGpio)) == LOW) {
    if ((millis() - started) > 2500U) break;
    delay(10);
  }
}

[[noreturn]] void enterDeepSleepNow(uint32_t wakeSec) {
  if (wakeSec == 0) wakeSec = kRtcWakeDefaultSec;
  if (wakeSec > kRtcWakeMaxSec) wakeSec = kRtcWakeMaxSec;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(kWakeKeyGpio, 0);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeSec) * 1000000ULL);
  Serial.printf("[SLEEP] enter deep sleep ext0(gpio=%d) timer=%us\n",
                static_cast<int>(kWakeKeyGpio),
                static_cast<unsigned int>(wakeSec));
  delay(20);
  esp_deep_sleep_start();
  while (true) {
    delay(1000);
  }
}

bool handleRtcMaintenanceWake() {
  ledcWrite(0, 0);
  rtc.begin();

  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';

  uint32_t nextWakeSec = kRtcWakeDefaultSec;
  uint64_t nowUnix = 0;
  const bool rtcOk = readRtcUnix(nowUnix);

  if (!rtcOk) {
    nextWakeSec = 10U;
    gSleepRtcCtx.expectedUnix = 0;
    Serial.println("[SLEEP] RTC read invalid, no external time, quick retry");
  } else {
    ReminderSchedule &schedule = scheduleScratchBuffer();
    uint32_t nextReminderDelta = 0;
    char nextReminderMessage[kSleepTextMaxLen + 1] = {0};
    const bool hasSchedule = loadReminderSchedule(schedule);
    if (hasSchedule &&
        computeNextReminderDelta(schedule, nowUnix, nextReminderDelta, nextReminderMessage)) {
      if (nextReminderDelta <= kReminderTriggerWindowSec) {
        gSleepRtcCtx.pendingReminder = 1;
        memcpy(gSleepRtcCtx.reminderMessage,
               nextReminderMessage,
               sizeof(gSleepRtcCtx.reminderMessage));
        gSleepRtcCtx.reminderMessage[sizeof(gSleepRtcCtx.reminderMessage) - 1] = '\0';
        gSleepRtcCtx.expectedUnix = nowUnix;
        gSleepRtcCtx.nextWakeSec = 0;
        Serial.printf("[SLEEP] reminder due now (delta=%u sec), continue boot\n",
                      static_cast<unsigned int>(nextReminderDelta));
        return true;
      }

      const uint32_t tierSec = chooseRtcRefillStepSec(nextReminderDelta);
      nextWakeSec = std::min(nextReminderDelta, tierSec);
      if (nextWakeSec == 0) {
        nextWakeSec = 1;
      }
      nextWakeSec = std::min(nextWakeSec, kRtcWakeMaxSec);
      gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(nextWakeSec);
      Serial.printf("[SLEEP] schedule pending delta=%u sec, tier=%u sec, next=%u sec\n",
                    static_cast<unsigned int>(nextReminderDelta),
                    static_cast<unsigned int>(tierSec),
                    static_cast<unsigned int>(nextWakeSec));
    } else {
      const uint64_t targetUnix = gSleepRtcCtx.expectedUnix;
      const uint64_t diffSec = (targetUnix == 0ULL) ? 0ULL : absDiffU64(nowUnix, targetUnix);
      if (targetUnix != 0ULL && diffSec <= kRtcMismatchToleranceSec) {
        nextWakeSec = kRtcWakeDefaultSec;
      } else {
        const uint32_t refillStep = chooseRtcRefillStepSec(diffSec == 0ULL ? kRtcWakeDefaultSec : diffSec);
        nextWakeSec = std::min(refillStep, kRtcWakeMaxSec);
      }
      gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(nextWakeSec);
      Serial.printf("[SLEEP] no schedule, heartbeat next=%u sec\n",
                    static_cast<unsigned int>(nextWakeSec));
    }
  }

  gSleepRtcCtx.magic = kSleepRtcCtxMagic;
  gSleepRtcCtx.version = kSleepRtcCtxVersion;
  gSleepRtcCtx.snapshotValid = 1;
  gSleepRtcCtx.nextWakeSec = nextWakeSec;
  enterDeepSleepNow(nextWakeSec);
  return false;
}

static bool ensureStaQueueMutex() {
  if (gStaMsgQueueMutex) return true;
  gStaMsgQueueMutex = xSemaphoreCreateMutex();
  return gStaMsgQueueMutex != nullptr;
}

static void resetStaHttpClient() {
  if (gStaHttpsClientReady) {
    gStaHttpsClient.stop();
  }
  gStaHttpsClientReady = false;
}

static void clearStaMessageQueue() {
  if (!ensureStaQueueMutex()) return;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  for (size_t i = 0; i < kStaPrefetchDepth; ++i) {
    gStaMsgQueue[i] = "";
  }
  gStaMsgQueueHead = 0;
  gStaMsgQueueSize = 0;
  xSemaphoreGive(gStaMsgQueueMutex);
}

static bool pushStaMessageQueue(const String &message) {
  if (!ensureStaQueueMutex()) return false;
  String normalized = message;
  normalized.trim();
  if (!normalized.length()) return false;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  if (gStaMsgQueueSize >= kStaPrefetchDepth) {
    xSemaphoreGive(gStaMsgQueueMutex);
    return false;
  }

  const size_t tail = (gStaMsgQueueHead + gStaMsgQueueSize) % kStaPrefetchDepth;
  gStaMsgQueue[tail] = normalized;
  gStaMsgQueueSize++;
  xSemaphoreGive(gStaMsgQueueMutex);
  return true;
}

static bool popStaMessageQueue(String &outMessage) {
  if (!ensureStaQueueMutex()) return false;
  outMessage = "";
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  if (gStaMsgQueueSize == 0) {
    xSemaphoreGive(gStaMsgQueueMutex);
    return false;
  }

  outMessage = gStaMsgQueue[gStaMsgQueueHead];
  gStaMsgQueue[gStaMsgQueueHead] = "";
  gStaMsgQueueHead = (gStaMsgQueueHead + 1) % kStaPrefetchDepth;
  gStaMsgQueueSize--;
  xSemaphoreGive(gStaMsgQueueMutex);
  return outMessage.length() > 0;
}

static size_t staMessageQueueSize() {
  if (!ensureStaQueueMutex()) return 0;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
  const size_t size = gStaMsgQueueSize;
  xSemaphoreGive(gStaMsgQueueMutex);
  return size;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static void appendUtf8Codepoint(String &out, uint16_t codepoint) {
  if (codepoint <= 0x7F) {
    out += static_cast<char>(codepoint);
    return;
  }
  if (codepoint <= 0x7FF) {
    out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
    return;
  }
  out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
  out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out += static_cast<char>(0x80 | (codepoint & 0x3F));
}

static bool extractJsonTextField(const String &json, String &outText) {
  outText = "";

  const int keyPos = json.indexOf("\"text\"");
  if (keyPos < 0) return false;

  int colon = json.indexOf(':', keyPos + 6);
  if (colon < 0) return false;
  colon++;
  while (colon < static_cast<int>(json.length()) &&
         (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n')) {
    colon++;
  }
  if (colon >= static_cast<int>(json.length()) || json[colon] != '"') return false;

  String decoded;
  decoded.reserve(128);
  for (int i = colon + 1; i < static_cast<int>(json.length()); ++i) {
    const char c = json[i];
    if (c == '"') {
      decoded.trim();
      if (!decoded.length()) return false;
      outText = decoded;
      return true;
    }
    if (c != '\\') {
      decoded += c;
      continue;
    }
    if (i + 1 >= static_cast<int>(json.length())) return false;
    const char esc = json[++i];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        decoded += esc;
        break;
      case 'b':
        decoded += '\b';
        break;
      case 'f':
        decoded += '\f';
        break;
      case 'n':
        decoded += '\n';
        break;
      case 'r':
        decoded += '\r';
        break;
      case 't':
        decoded += '\t';
        break;
      case 'u': {
        if (i + 4 >= static_cast<int>(json.length())) return false;
        const int h0 = hexNibble(json[i + 1]);
        const int h1 = hexNibble(json[i + 2]);
        const int h2 = hexNibble(json[i + 3]);
        const int h3 = hexNibble(json[i + 4]);
        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
        const uint16_t cp = static_cast<uint16_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
        appendUtf8Codepoint(decoded, cp);
        i += 4;
        break;
      }
      default:
        decoded += esc;
        break;
    }
  }

  return false;
}

static bool fetchStaMessageFromCloud(String &outMessage) {
  outMessage = "";
  if (WiFi.status() != WL_CONNECTED) return false;

  String apiUrl = gStaNetApi;
  if (!apiUrl.length()) {
    apiUrl = kStaCloudApiUrlDefault;
  }
  apiUrl.trim();
  String apiUrlLower = apiUrl;
  apiUrlLower.toLowerCase();
  const bool useHttps = apiUrlLower.startsWith("https://");
  const bool useHttp = apiUrlLower.startsWith("http://");
  if (!useHttps && !useHttp) {
    Serial.printf("[STA] API URL scheme unsupported: %s\n", apiUrl.c_str());
    return false;
  }

  if (useHttps && !gStaHttpsClientReady) {
    gStaHttpsClient.setInsecure();
    gStaHttpsClient.setTimeout(kStaHttpReadTimeoutMs);
    gStaHttpsClientReady = true;
  }

  HTTPClient http;
  http.setConnectTimeout(kStaHttpConnectTimeoutMs);
  http.setTimeout(kStaHttpReadTimeoutMs);
  http.setReuse(true);
  const bool beginOk = useHttps ? http.begin(gStaHttpsClient, apiUrl) : http.begin(apiUrl);
  if (!beginOk) {
    Serial.println("[STA] API HTTP begin failed");
    if (useHttps) {
      resetStaHttpClient();
    }
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.setUserAgent("TestTFT-ESP32S3/1.0");

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[STA] API GET failed code=%d\n", httpCode);
    http.end();
    if (httpCode < 0 && useHttps) {
      resetStaHttpClient();
    }
    return false;
  }

  const String payload = http.getString();
  http.end();
  if (!extractJsonTextField(payload, outMessage)) {
    Serial.printf("[STA] API JSON parse failed, len=%u\n",
                  static_cast<unsigned int>(payload.length()));
    return false;
  }
  return true;
}

static bool fetchAndQueueOneStaMessage() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (staMessageQueueSize() >= kStaPrefetchDepth) return false;
  const uint32_t nowMs = millis();
  if (gStaNextFetchAllowedMs != 0 &&
      static_cast<int32_t>(nowMs - gStaNextFetchAllowedMs) < 0) {
    return false;
  }

  gStaFetchInProgress = true;
  String fetched;
  const bool fetchedOk = fetchStaMessageFromCloud(fetched);
  gStaFetchInProgress = false;

  if (!fetchedOk) {
    gStaNextFetchAllowedMs = nowMs + kStaFetchFailCooldownMs;
    return false;
  }
  if (!pushStaMessageQueue(fetched)) return false;
  const size_t queued = staMessageQueueSize();
  gStaNextFetchAllowedMs = 0;
  Serial.printf("[STA] queued cloud message count=%u\n",
                static_cast<unsigned int>(queued));
  return true;
}

static void staMessageFetcherTask(void *param) {
  (void)param;
  while (true) {
    const bool shouldFetch = (gAppLoopMode == APP_MODE_STA_ONLINE) &&
                             (gStaOnlinePhase == StaOnlinePhase::kConnected) &&
                             (WiFi.status() == WL_CONNECTED);

    if (shouldFetch && staMessageQueueSize() < kStaPrefetchDepth) {
      while (staMessageQueueSize() < kStaPrefetchDepth) {
        if (!fetchAndQueueOneStaMessage()) {
          break;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kStaFetcherTickMs));
  }
}

static void waitStaFetcherIdle(uint32_t maxWaitMs) {
  const uint32_t started = millis();
  while (gStaFetchInProgress && (millis() - started) < maxWaitMs) {
    delay(10);
  }
}

static void ensureStaFetcherTaskStarted() {
  if (gStaFetcherTaskHandle) return;
  if (!ensureStaQueueMutex()) {
    Serial.println("[STA] queue mutex create failed");
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(
      staMessageFetcherTask,
      "StaMsgFetch",
      kStaFetcherStackSize,
      nullptr,
      kStaFetcherPriority,
      &gStaFetcherTaskHandle,
      kStaFetcherCore);
  if (ok != pdPASS) {
    gStaFetcherTaskHandle = nullptr;
    Serial.println("[STA] fetcher task create failed");
  }
}

static String trimIniValue(String value) {
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0) {
    value = value.substring(0, semicolon);
  }
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

static bool parseAppModeFromIniValue(String value, AppLoopMode &outMode) {
  value.trim();
  value.toLowerCase();
  value.replace("-", "_");
  value.replace(" ", "");

  if (value == "ap_config" || value == "ap_sta" || value == "ap") {
    outMode = APP_MODE_AP_STA;
    return true;
  }
  if (value == "sta_online" || value == "staonline") {
    outMode = APP_MODE_STA_ONLINE;
    return true;
  }
  if (value == "sta_only" || value == "staonly") {
    outMode = APP_MODE_STA_ONLY;
    return true;
  }
  return false;
}

static const char *appModeToIniValue(AppLoopMode mode) {
  switch (mode) {
    case APP_MODE_AP_STA:
      return "AP_Config";
    case APP_MODE_STA_ONLINE:
      return "STA_Online";
    case APP_MODE_STA_ONLY:
      return "STA_Only";
    default:
      return "AP_Config";
  }
}

static bool loadAppModeFromSettingIni(AppLoopMode &outMode) {
  outMode = APP_MODE_AP_STA;
  if (!fatMounted) return false;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) return false;

  bool found = false;
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
    value = trimIniValue(value);

    if (key == "mode") {
      AppLoopMode parsed = APP_MODE_AP_STA;
      if (parseAppModeFromIniValue(value, parsed)) {
        outMode = parsed;
        found = true;
      } else {
        Serial.printf("[BOOT] invalid Mode in /setting.ini: %s\n", value.c_str());
      }
    }
  }
  f.close();
  return found;
}

static bool persistAppModeToSettingIni(AppLoopMode mode) {
  if (!fatMounted) return false;

  String original;
  if (FFat.exists("/setting.ini")) {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf) return false;
    original = rf.readString();
    rf.close();
  }

  bool foundMode = false;
  const String modeLine = String("Mode = ") + appModeToIniValue(mode) + ";";
  String output;
  output.reserve(original.length() + 32);

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
        if (key == "mode") {
          line = modeLine;
          foundMode = true;
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

  if (!foundMode) {
    if (output.length() && output[output.length() - 1] != '\n') output += '\n';
    output += modeLine;
    output += '\n';
  }

  fs::File wf = FFat.open("/setting.ini", "w");
  if (!wf) return false;
  const size_t written = wf.print(output);
  wf.close();
  return written == output.length();
}

static bool loadStaCredentialsFromSettingIni(String &outSsid, String &outPassword, String &outNet) {
  outSsid = "";
  outPassword = "";
  outNet = kStaCloudApiUrlDefault;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f) {
    Serial.println("[STA] /setting.ini not found");
    return false;
  }

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
    value = trimIniValue(value);

    if (key == "netssid") {
      outSsid = value;
    } else if (key == "netpassword") {
      outPassword = value;
    } else if (key == "net") {
      String netLower = value;
      netLower.toLowerCase();
      if (netLower.startsWith("http://") || netLower.startsWith("https://")) {
        outNet = value;
      } else if (value.length()) {
        Serial.printf("[STA] invalid Net in /setting.ini: %s\n", value.c_str());
      }
    }
  }
  f.close();

  return outSsid.length() > 0;
}

static bool tmToDs1302DateTime(const struct tm &in, Ds1302DateTime &out) {
  out.year = static_cast<uint16_t>(in.tm_year + 1900);
  out.month = static_cast<uint8_t>(in.tm_mon + 1);
  out.day = static_cast<uint8_t>(in.tm_mday);
  out.hour = static_cast<uint8_t>(in.tm_hour);
  out.minute = static_cast<uint8_t>(in.tm_min);
  out.second = static_cast<uint8_t>(in.tm_sec);
  return ds1302IsValidDateTime(out);
}

static bool syncDs1302FromStaNtp(String &detailOut) {
  detailOut = "";
  if (WiFi.status() != WL_CONNECTED) {
    detailOut = "wifi disconnected";
    return false;
  }

  const uint32_t nowMs = millis();
  if (gStaNtpLastAttemptMs != 0 &&
      (nowMs - gStaNtpLastAttemptMs) < kStaNtpAttemptCooldownMs) {
    detailOut = gStaNtpSyncedThisSession ? "already synced" : "cooldown";
    return gStaNtpSyncedThisSession;
  }
  gStaNtpLastAttemptMs = nowMs;

  configTime(kStaNtpGmtOffsetSec, 0, kStaNtpServer1, kStaNtpServer2, kStaNtpServer3);
  struct tm tmNow = {};
  const uint32_t started = millis();
  bool gotTime = false;
  while ((millis() - started) < kStaNtpPollTimeoutMs) {
    if (getLocalTime(&tmNow, 300)) {
      gotTime = true;
      break;
    }
    delay(60);
  }

  if (!gotTime) {
    detailOut = "ntp timeout";
    return false;
  }

  Ds1302DateTime synced;
  if (!tmToDs1302DateTime(tmNow, synced)) {
    detailOut = "invalid ntp datetime";
    return false;
  }
  if (!rtc.writeDateTime(synced)) {
    detailOut = "ds1302 write failed";
    return false;
  }

  gStaNtpSyncedThisSession = true;
  char buf[48] = {0};
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
           static_cast<unsigned int>(synced.year),
           static_cast<unsigned int>(synced.month),
           static_cast<unsigned int>(synced.day),
           static_cast<unsigned int>(synced.hour),
           static_cast<unsigned int>(synced.minute),
           static_cast<unsigned int>(synced.second));
  detailOut = String(buf);
  return true;
}

static void playStaMessage(const String &text) {
  playMessageWithGlitch(text.c_str());
}
static void playAPMessage(const String &text) {
  playMessageWithGlitch(text.c_str());
}
static void playStaOnlyMessage(const String &text) {
  playMessageWithGlitch(text.c_str());
}
static void beginStaConnectAttempt() {
  if (!gStaNetSsid.length()) {
    gStaOnlinePhase = StaOnlinePhase::kFailWaitShort;
    playStaMessage(kStaMissingCfgMsg);
    return;
  }

  String msg = kStaConnectingPrefix;
  msg += gStaNetSsid;
  if (gStaRetryCount > 0) {
    msg += kStaRetryPrefix;
    msg += String(gStaRetryCount);
  }
  playStaMessage(msg);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(20);
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  WiFi.begin(gStaNetSsid.c_str(), gStaNetPassword.c_str());

  gStaAttemptStartMs = millis();
  gStaOnlinePhase = StaOnlinePhase::kConnecting;
}

static uint8_t modeToIndex(AppLoopMode mode) {
  switch (mode) {
    case APP_MODE_AP_STA:
      return 0;
    case APP_MODE_STA_ONLINE:
      return 1;
    case APP_MODE_STA_ONLY:
      return 2;
    default:
      return 0;
  }
}

static void switchAppMode(AppLoopMode mode) {
  if (gAppLoopMode == mode) return;
  gAppLoopMode = mode;
  gAppModeEnterPending = true;
  if (!persistAppModeToSettingIni(mode)) {
    Serial.println("[MODE] save Mode to /setting.ini failed");
  } else {
    Serial.printf("[MODE] switched -> %s\n", appModeToIniValue(mode));
  }
}

static void dispatchModeEnterIfNeeded() {
  if (!gAppModeEnterPending) return;
  gAppModeEnterPending = false;
  AppModeEnterCallback callback = gAppModeInitCallbacks[modeToIndex(gAppLoopMode)];
  if (!callback) {
    callback = gAppModeEnterCallback;  // Fallback generic callback.
  }
  if (callback) {
    callback(gAppLoopMode);
  }
}

void setAppModeEnterCallback(AppModeEnterCallback callback) {
  gAppModeEnterCallback = callback;
  if (callback) {
    // Ensure current mode triggers once after callback registration.
    gAppModeEnterPending = true;
  }
}

void setAppModeInitCallback(AppLoopMode mode, AppModeEnterCallback callback) {
  gAppModeInitCallbacks[modeToIndex(mode)] = callback;
  if (callback && mode == gAppLoopMode) {
    // Ensure current mode triggers once after per-mode callback registration.
    gAppModeEnterPending = true;
  }
}

AppLoopMode getAppLoopMode() {
  return gAppLoopMode;
}

void applyStartupModeFromSettingIni() {
  AppLoopMode startupMode = gAppLoopMode;
  if (loadAppModeFromSettingIni(startupMode)) {
    gAppLoopMode = startupMode;
    Serial.printf("[BOOT] startup Mode=%s\n", appModeToIniValue(gAppLoopMode));
  } else {
    Serial.printf("[BOOT] startup Mode fallback=%s\n", appModeToIniValue(gAppLoopMode));
  }
  gAppModeEnterPending = true;
  dispatchModeEnterIfNeeded();
}


void processAppLoop() {
  struct InterruptController {
    bool webActive = false;
    bool webKeyLatch = false;
    bool imageActive = false;
    bool imageKeyLatch = false;
    bool imagePreemptedWeb = false;
    uint16_t imageWidth = 0;
    uint16_t imageHeight = 0;
    int16_t imageCenterX = 160;
    int16_t imageCenterY = 155;
    bool immediateActive = false;
    bool immediateKeyLatch = false;
    bool immediatePreemptedWeb = false;
  };
  
  
  enum class ImageResumeTarget : uint8_t {
    kNone = 0,
    kWeb = 1,
  };

  static InterruptController irq;
  bool syntheticKeyPress = false;
  const bool instantRefreshNoKey = wirelessPortalInstantRefreshNoKeyEnabled();
  dispatchModeEnterIfNeeded();
  static AppLoopMode lastLoopMode = APP_MODE_AP_STA;
  if (lastLoopMode != gAppLoopMode) {
    irq = InterruptController{};
    lastLoopMode = gAppLoopMode;
  }

  auto preemptByHost = [&]() {
    if (irq.imageActive) {
      irq.imageActive = false;
      irq.imageKeyLatch = false;
      if (irq.imagePreemptedWeb) {
        irq.webActive = true;
        irq.webKeyLatch = false;
      }
      irq.imagePreemptedWeb = false;
      tft.fillScreen(0x0000);
      Serial.println("[WEB] image interrupt preempted by host");
    }
    if (irq.immediateActive) {
      irq.immediateKeyLatch = false;
      Serial.println("[ESPNOW] preempt immediate interrupt");
    }
    if (irq.webActive) {
      Serial.println("[ESPNOW] preempt web interrupt");
    }
  };

  auto startImmediateInterrupt = [&]() {
    if (!irq.immediateActive) {
      const bool hadImageInterrupt = irq.imageActive;
      const bool hadImagePreemptedWebInterrupt = irq.imagePreemptedWeb;
      const bool hadWebInterrupt = irq.webActive;

      if (hadImageInterrupt) {
        irq.imageActive = false;
        irq.imageKeyLatch = false;
        irq.imagePreemptedWeb = false;
        tft.fillScreen(0x0000);
        Serial.println("[WEB] image interrupt preempted by immediate");
      }
      if (hadWebInterrupt) {
        irq.webActive = false;
        irq.webKeyLatch = false;
        Serial.println("[WEB] web interrupt preempted by immediate");
      }

      irq.immediateActive = true;
      irq.immediateKeyLatch = false;
      // Image and immediate are same-layer interrupts: do not preserve each other.
      // But keep lower-layer web resume chain when either one had preempted web.
      irq.immediatePreemptedWeb = hadWebInterrupt || hadImagePreemptedWebInterrupt;
      Serial.println("[WEB] immediate interrupt started");
    } else {
      irq.immediateKeyLatch = false;
      Serial.println("[WEB] immediate interrupt updated");
    }
  };

  auto startImageInterrupt = [&](uint16_t imageW, uint16_t imageH, int16_t centerX, int16_t centerY) {
    notifyBacklightActivity();
    const bool hadImmediateInterrupt = irq.immediateActive;
    const bool hadImmediatePreemptedWeb = irq.immediatePreemptedWeb;
    const bool hadWebInterrupt = irq.webActive;
    const bool keepPreemptedWeb = irq.imageActive && irq.imagePreemptedWeb;

    if (hadImmediateInterrupt) {
      irq.immediateActive = false;
      irq.immediateKeyLatch = false;
      Serial.println("[WEB] immediate interrupt preempted by image");
    }
    if (hadWebInterrupt) {
      irq.webActive = false;
      irq.webKeyLatch = false;
    }

    showWebInterruptImage(gWebImageScratch, imageW, imageH, centerX, centerY);
    irq.imageActive = true;
    irq.imageKeyLatch = false;
    // Image and immediate are same-layer interrupts: do not preserve each other.
    irq.imagePreemptedWeb = hadWebInterrupt || hadImmediatePreemptedWeb || keepPreemptedWeb;
    irq.imageWidth = imageW;
    irq.imageHeight = imageH;
    irq.imageCenterX = centerX;
    irq.imageCenterY = centerY;
    Serial.printf("[WEB] image interrupt started %ux%u\n",
                  static_cast<unsigned int>(imageW),
                  static_cast<unsigned int>(imageH));
  };

  auto finishImmediateInterrupt = [&]() {
    irq.immediateActive = false;
    irq.immediateKeyLatch = false;
    const bool canResumeWeb = irq.immediatePreemptedWeb && wirelessPortalHasPendingMessage();
    if (canResumeWeb) {
      irq.webActive = true;
      irq.webKeyLatch = false;
      Serial.println("[WEB] immediate interrupt resume web");
    } else {
      if (irq.immediatePreemptedWeb) {
        irq.webActive = false;
        irq.webKeyLatch = false;
        Serial.println("[WEB] immediate interrupt finished (no pending web message)");
      } else {
        Serial.println("[WEB] immediate interrupt finished");
      }
    }
    // Pass-through the same physical keypress so immediate close does not require a second press.
    syntheticKeyPress = true;
    irq.immediatePreemptedWeb = false;
  };

  auto finishImageInterrupt = [&]() -> ImageResumeTarget {
    irq.imageActive = false;
    irq.imageKeyLatch = false;
    ImageResumeTarget resumeTarget = ImageResumeTarget::kNone;
    if (irq.imagePreemptedWeb) {
      irq.webActive = true;
      irq.webKeyLatch = false;
      resumeTarget = ImageResumeTarget::kWeb;
    }
    irq.imagePreemptedWeb = false;
    // Pass-through the same physical keypress so image close does not require a second press.
    syntheticKeyPress = true;
    tft.fillScreen(0x0000);
    Serial.println("[WEB] image interrupt finished");
    return resumeTarget;
  };

  {
    const size_t wantedPixels = static_cast<size_t>(kWebImageMaxWidth) * static_cast<size_t>(kWebImageMaxHeight);
    if (!gWebImageScratch) {
      if (!ensureWebImageScratch(wantedPixels)) {
        static uint32_t lastAllocLogMs = 0;
        const uint32_t now = millis();
        if (now - lastAllocLogMs > 5000U) {
          lastAllocLogMs = now;
          Serial.println("[WEB] image scratch alloc failed");
        }
      }
    }
  }

  if (wirelessPortalConsumeCsvReloadRequest()) {
    notifyBacklightActivity();
    if (csv.load(FFat, "/data.csv")) {
      csvCount = 0;
      RUNSTATE = 0;
      if (instantRefreshNoKey) {
        firstFlag = true;
      }
      Serial.println("[WEB] /data.csv reloaded");
    } else {
      Serial.println("[WEB] /data.csv reload failed");
    }
  }
  serviceScheduleInterruptByRtcMinute();

  const bool allowHostInterrupts =
      (gAppLoopMode == APP_MODE_AP_STA) ||
      (gAppLoopMode == APP_MODE_STA_ONLY) ||
      (gAppLoopMode == APP_MODE_STA_ONLINE &&
       gStaOnlinePhase == StaOnlinePhase::kConnected);

  if (allowHostInterrupts) {
    String hostBroadcastMessage;
    if (wirelessPortalPopHostMessage(hostBroadcastMessage)) {
      preemptByHost();
      playMessageWithGlitch(hostBroadcastMessage.c_str());
      return;
    }

    String scheduleInterruptMessage;
    if (popScheduleInterruptMessage(scheduleInterruptMessage)) {
      preemptByHost();
      playMessageWithGlitch(scheduleInterruptMessage.c_str());
      return;
    }
  }

  //网页来源中断（网页常规|网页立即|网页图片）门控：只有在AP模式或者STA已连接模式启用
  const bool allowWebInterrupts =
      (gAppLoopMode == APP_MODE_AP_STA) ||
      (gAppLoopMode == APP_MODE_STA_ONLINE &&
       gStaOnlinePhase == StaOnlinePhase::kConnected);

  if (allowWebInterrupts) {
    String immediateMessage;
    if (wirelessPortalPopImmediateMessage(immediateMessage)) {
      startImmediateInterrupt();
      playMessageWithGlitch(immediateMessage.c_str());
      return;
    }

    // Image interrupt and immediate interrupt are peer-level:
    // image polling must happen before "immediate active" wait branch.
    if (gWebImageScratch) {
      uint16_t imageW = 0;
      uint16_t imageH = 0;
      int16_t centerX = 160;
      int16_t centerY = 155;
      size_t pixelCount = 0;
      if (wirelessPortalTakePendingImage(gWebImageScratch,
                                         gWebImageScratchPixels,
                                         imageW,
                                         imageH,
                                         centerX,
                                         centerY,
                                         pixelCount)) {
        (void)pixelCount;
        startImageInterrupt(imageW, imageH, centerX, centerY);
        return;
      }
    }

    if (irq.immediateActive) {
      Key_loop();
      const uint8_t key = get_Keycode();
      if (key == 2 && !irq.immediateKeyLatch) {
        irq.immediateKeyLatch = true;
        if (wakeBacklightByKeyIfNeeded()) {
          return;
        }
        finishImmediateInterrupt();
      }
      if (key != 2) {
        irq.immediateKeyLatch = false;
      }
      if (irq.immediateActive) {
        return;
      }
    }

    if (irq.imageActive) {
      Key_loop();
      const uint8_t key = get_Keycode();
      if (key == 2 && !irq.imageKeyLatch) {
        irq.imageKeyLatch = true;
        if (wakeBacklightByKeyIfNeeded()) {
          // Backlight wake is always effective and does not end image interrupt.
          return;
        }
        (void)finishImageInterrupt();
      } else {
        if (key != 2) {
          irq.imageKeyLatch = false;
        }
        return;
      }
    }

    if (!irq.webActive && wirelessPortalHasPendingMessage()) {
      String queuedMessage;
      if (wirelessPortalPopMessage(queuedMessage)) {
        irq.webActive = true;
        irq.webKeyLatch = false;
        Serial.println("[WEB] interrupt started");
        playMessageWithGlitch(queuedMessage.c_str());
        return;
      }
    }

    if (irq.webActive) {
      uint8_t key = 255;
      if (syntheticKeyPress) {
        key = 2;
        syntheticKeyPress = false;
      } else {
        Key_loop();
        key = get_Keycode();
      }
      if (key == 2 && !irq.webKeyLatch) {
        irq.webKeyLatch = true;
        if (wakeBacklightByKeyIfNeeded()) {
          return;
        }
        String queuedMessage;
        if (wirelessPortalPopMessage(queuedMessage)) {
          playMessageWithGlitch(queuedMessage.c_str());
          if (!wirelessPortalHasPendingMessage()) {
            irq.webActive = false;
            irq.webKeyLatch = false;
            // This key press just consumed the final web message:
            // pass it through so AP/STA normal flow does not need a second press.
            syntheticKeyPress = true;
            Serial.println("[WEB] interrupt finished");
          }
        } else {
          irq.webActive = false;
          irq.webKeyLatch = false;
          // No next web message to pop: pass this same physical keypress
          // to AP normal flow so a single-message web interrupt does not
          // require pressing the key twice to continue.
          syntheticKeyPress = true;
          Serial.println("[WEB] interrupt finished");
        }
        if (irq.webActive) {
          return;
        }
        // Web queue finished: continue in this same loop so the key-press
        // pass-through can be consumed by AP/STA normal flow immediately.
        // (do not return here, otherwise syntheticKeyPress is lost)
      }
      if (key != 2) {
        irq.webKeyLatch = false;
        return;
      }
    }
  }

  if(gAppLoopMode == APP_MODE_AP_STA)
  {
    int csvTotal = csv.size();
    if (csvTotal > kCsvArrayCapacity) {
      csvTotal = kCsvArrayCapacity;
    }
    if (csvTotal <= 0) {
      return;
    }
    if (RUNSTATE == 0) {
      generateUniqueRandomNumbers(1, csv.size(), csvTotal, csvArray);
      csvCount = 0;
      RUNSTATE = 1;
    }
    if (RUNSTATE == 1) 
    {
      uint8_t key = 255;
      if (syntheticKeyPress) 
      {
        key = 2;
        syntheticKeyPress = false;
      } 
      else 
      {
        Key_loop();
        key = get_Keycode();
      }
      if ((key == 2 || key == 3) && wakeBacklightByKeyIfNeeded())
      {
        return;
      }
      if (key == 3) 
      {
        switchAppMode(APP_MODE_STA_ONLINE);
        return;
      }

      if (key == 2 || firstFlag) 
      {
        if (firstFlag) 
        {
          firstFlag = false;
        }
        if (csvCount >= csvTotal) 
        {
          generateUniqueRandomNumbers(1, csv.size(), csvTotal, csvArray);
          csvCount = 0;
          RUNSTATE = 1;
        }

        const int currentCsvId = csvArray[csvCount];
        csvCount++;

        String localMessage;
        const char *csvMessage = csv.getTextById(currentCsvId);
        if (csvMessage) {
          localMessage = csvMessage;
        } else {
          localMessage = "CSV id not found: ";
          localMessage += String(currentCsvId);
        }
        message = localMessage.c_str();
        playMessageWithGlitch(message);

        
      }
    }
  }
  else if(gAppLoopMode == APP_MODE_STA_ONLINE)
  {
    uint8_t key = 255;
    if (syntheticKeyPress)
    {
      key = 2;
      syntheticKeyPress = false;
    }
    else
    {
      Key_loop();
      key = get_Keycode();
    }
    if ((key == 2 || key == 3) && wakeBacklightByKeyIfNeeded())
    {
      return;
    }

    // Long press always switches mode, including while connecting.
    if (key == 3)
    {
      switchAppMode(APP_MODE_STA_ONLY);
      return;
    }

    switch (gStaOnlinePhase) {
      case StaOnlinePhase::kPromptWaitShort:
        if (key == 2) {
          beginStaConnectAttempt();
        }
        return;

      case StaOnlinePhase::kConnecting: {
        const wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
          gStaOnlinePhase = StaOnlinePhase::kConnected;
          clearStaMessageQueue();
          gStaLastQueueEmptyHintMs = 0;
          gStaNextFetchAllowedMs = 0;
          String okMsg = kStaConnectOkPrefix;
          okMsg += gStaNetSsid;
          String ntpDetail;
          if (syncDs1302FromStaNtp(ntpDetail)) {
            Serial.printf("[STA] NTP sync -> DS1302 ok: %s\n", ntpDetail.c_str());
          } else {
            Serial.printf("[STA] NTP sync skipped/failed: %s\n", ntpDetail.c_str());
          }
          playStaMessage(okMsg);
          return;
        }

        const uint32_t elapsed = millis() - gStaAttemptStartMs;
        if (elapsed < kStaAttemptTimeoutMs) {
          return;
        }

        gStaRetryCount++;
        if (gStaRetryCount >= kStaMaxRetryCount) {
          gStaOnlinePhase = StaOnlinePhase::kFailWaitShort;
          playStaMessage(kStaConnectFailMsg);
          return;
        }

        beginStaConnectAttempt();
        return;
      }

      case StaOnlinePhase::kFailWaitShort:
        if (key == 2) {
          switchAppMode(APP_MODE_STA_ONLY);
        }
        return;

      case StaOnlinePhase::kConnected:
        if (WiFi.status() != WL_CONNECTED) {
          gStaOnlinePhase = StaOnlinePhase::kDisconnectedWaitShort;
          gStaRetryCount = 0;
          gStaAttemptStartMs = 0;
          gStaLastQueueEmptyHintMs = 0;
          gStaNextFetchAllowedMs = 0;
          gStaNtpSyncedThisSession = false;
          gStaNtpLastAttemptMs = 0;
          clearStaMessageQueue();
          playStaMessage(kStaDisconnectedMsg);
          return;
        }
        if (key == 2) {
          String nextMessage;
          if (!popStaMessageQueue(nextMessage)) {
            const uint32_t nowMs = millis();
            if ((nowMs - gStaLastQueueEmptyHintMs) < kStaQueueEmptyHintCooldownMs) {
              return;
            }
            gStaLastQueueEmptyHintMs = nowMs;
            playStaMessage(kStaCloudQueueEmptyMsg);
            return;
          }
          gStaLastQueueEmptyHintMs = 0;
          playStaMessage(nextMessage);
          // Refill is handled by background fetch task.
          return;
        }
        return;

      case StaOnlinePhase::kDisconnectedWaitShort:
        if (key == 2) {
          beginStaConnectAttempt();
        }
        return;
    }
  }
  else if(gAppLoopMode == APP_MODE_STA_ONLY)
  {
    int csvTotal = csv.size();
    if (csvTotal > kCsvArrayCapacity) {
      csvTotal = kCsvArrayCapacity;
    }
    if (csvTotal <= 0) {
      return;
    }
    if (RUNSTATE == 0) {
      generateUniqueRandomNumbers(1, csv.size(), csvTotal, csvArray);
      csvCount = 0;
      RUNSTATE = 1;
    }

    uint8_t key = 255;
    if (syntheticKeyPress)
    {
      key = 2;
      syntheticKeyPress = false;
    }
    else
    {
      Key_loop();
      key = get_Keycode();
    }
    if ((key == 2 || key == 3) && wakeBacklightByKeyIfNeeded())
    {
      return;
    }

    if (key == 3)
    {
      switchAppMode(APP_MODE_AP_STA);
      return;
    }

    if (staOnlySleepTimeoutReached()) {
      enterStaOnlyDeepSleep();
    }

    if (key == 2 || firstFlag)
    {
      if (firstFlag)
      {
        firstFlag = false;
      }
      if (csvCount >= csvTotal)
      {
        generateUniqueRandomNumbers(1, csv.size(), csvTotal, csvArray);
        csvCount = 0;
        RUNSTATE = 1;
      }

      const int currentCsvId = csvArray[csvCount];
      csvCount++;

      String localMessage;
      const char *csvMessage = csv.getTextById(currentCsvId);
      if (csvMessage) {
        localMessage = csvMessage;
      } else {
        localMessage = "CSV id not found: ";
        localMessage += String(currentCsvId);
      }
      message = localMessage.c_str();
      playMessageWithGlitch(message);
    }
  }
  else
  {
    switchAppMode(APP_MODE_AP_STA);
  }
}

void onApStaInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  // Force a clean restart so switching back from STA-only always re-creates AP.
  wirelessPortalStop();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaNetSsid = "";
  gStaNetPassword = "";
  gStaNetApi = kStaCloudApiUrlDefault;
  clearStaMessageQueue();
  if (!wirelessPortalStart()) {
    Serial.println("[AP] wirelessPortalStart failed on AP init");
  }
  if (!consumeStartupPromptSkip(mode)) {
    playAPMessage(kApPromptMsg);
  }
}

void onStaOnlineInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  if (!wirelessPortalStart()) {
    Serial.println("[STA] wirelessPortalStart failed on STA_Online init");
  }
  WiFi.disconnect(true, false);

  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  clearStaMessageQueue();

  const bool loaded = loadStaCredentialsFromSettingIni(gStaNetSsid, gStaNetPassword, gStaNetApi);
  if (!loaded) {
    Serial.println("[STA] NetSSID missing in /setting.ini");
  } else {
    Serial.printf("[STA] target ssid: %s\\n", gStaNetSsid.c_str());
  }
  Serial.printf("[STA] target net: %s\\n", gStaNetApi.c_str());

  ensureStaFetcherTaskStarted();
  if (consumeStartupPromptSkip(mode)) {
    beginStaConnectAttempt();
  } else {
    playStaMessage(kStaPromptMsg);
  }
}

void onStaOnlyInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  wirelessPortalStop();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaNetApi = kStaCloudApiUrlDefault;
  clearStaMessageQueue();
  WiFi.disconnect(true, false);
  if (!wirelessPortalStartEspNowOnly()) {
    Serial.println("[STA_ONLY] wirelessPortalStartEspNowOnly failed");
  }
  if (!consumeStartupPromptSkip(mode)) {
    get_Keycode();
    playStaOnlyMessage(kStaonlyPromptMsg);
  }
}

bool appHandleRtcMaintenanceWakeIfNeeded() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
    return false;
  }
  if (!hasValidSleepRtcContext()) {
    return false;
  }
  Serial.println("[SLEEP] timer wake detected, run RTC maintenance");
  const bool shouldContinueBoot = handleRtcMaintenanceWake();
  return !shouldContinueBoot;
}

bool appShouldFastResumeFromDeepSleep() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    return hasValidSleepRtcContext();
  }
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    return hasValidSleepRtcContext() && (gSleepRtcCtx.pendingReminder != 0);
  }
  return false;
}

bool appRestoreFromDeepSleepSnapshot() {
  if (!appShouldFastResumeFromDeepSleep()) {
    return false;
  }
  const bool timerReminderWake =
      (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) &&
      (gSleepRtcCtx.pendingReminder != 0);
  char reminderMessage[kSleepTextMaxLen + 1] = {0};
  if (timerReminderWake) {
    memcpy(reminderMessage,
           gSleepRtcCtx.reminderMessage,
           sizeof(reminderMessage));
    reminderMessage[sizeof(reminderMessage) - 1] = '\0';
    if (!reminderMessage[0]) {
      sanitizeMessageForSnapshot(String(kDefaultReminderMessage), reminderMessage);
    }
  }

  SleepSnapshotData snapshot;
  if (!loadSleepSnapshotFromFat(snapshot)) {
    Serial.println("[SLEEP] snapshot not found or invalid, fallback normal app boot");
    clearSleepRtcContext();
    return false;
  }
  if (!applySleepSnapshot(snapshot, !timerReminderWake)) {
    Serial.println("[SLEEP] snapshot apply failed, fallback normal app boot");
    clearSleepRtcContext();
    return false;
  }
  gSkipStartupPromptOnce = false;

  if (timerReminderWake) {
    (void)markCurrentMinuteScheduleAsTriggeredFromRtc();
    playMessageWithGlitch(reminderMessage);
  }

  (void)FFat.remove(kSleepSnapshotPath);
  clearSleepRtcContext();
  Serial.println("[SLEEP] snapshot restored");
  return true;
}
