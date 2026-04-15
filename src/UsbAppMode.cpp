#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <FFat.h>
#include <USB.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include <ctype.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "AppGlobals.h"
#include "UsbAppMode.h"
#include "DisplayEffects.h"
#include "WirelessPortal.h"
#include "Index_B.h"
#include "Key_Drv.h"
namespace {

constexpr uint8_t kBacklightDutyOff = 0;
constexpr uint32_t kBacklightDimToOffMs = 20000UL;
constexpr uint32_t kBacklightTaskTickMs = 100UL;
constexpr uint32_t kBootAnimPollMs = 10UL;
constexpr uint32_t kBootAnimMaxWaitMs = 12000UL;

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
      const uint32_t elapsedMs = nowMs - lastActivity;
      if (elapsedMs >= dimMs + kBacklightDimToOffMs) {
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

  gInsertGain = kDefaultInsertGain;
  gBgGain = kDefaultBgGain;
  gBacklightLevel = kDefaultBacklightLevel;
  gWrongProb3 = kDefaultWrongProb3;
  gWrongProb5 = kDefaultWrongProb5;
  gEnableReprint = kDefaultEnableReprint;
  gBacklightTimeSec = kDefaultBacklightTimeSec;

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
  Serial.printf("[APP] gains: insert=%.3f bg=%.3f backlight=%.3f\n",
                gInsertGain,
                gBgGain,
                gBacklightLevel);
  Serial.printf("[APP] glitch: p3=%d p5=%d reprint=%d backlightTime=%d\n",
                gWrongProb3,
                gWrongProb5,
                gEnableReprint ? 1 : 0,
                gBacklightTimeSec);
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
static uint8_t gStaRetryCount = 0;
static uint32_t gStaAttemptStartMs = 0;
static constexpr uint32_t kStaAttemptTimeoutMs = 10000UL;
static constexpr uint8_t kStaMaxRetryCount = 5;
static constexpr const char *kStaPromptMsg =
    u8"\u6A21\u5F0F:\u8054\u7F51|\u77ED\u6309\u4EE5\u8FDE\u63A5WiFi";
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
static constexpr const char *kStaCloudApiUrl = "http://115.190.145.254:8080/random";
static constexpr size_t kStaPrefetchDepth = 20;
static constexpr uint32_t kStaQueueEmptyHintCooldownMs = 1800UL;
static constexpr uint32_t kStaFetchFailCooldownMs = 1000UL;
static constexpr uint32_t kStaFetcherTickMs = 200UL;
static constexpr uint32_t kStaHttpConnectTimeoutMs = 3500UL;
static constexpr uint32_t kStaHttpReadTimeoutMs = 4500UL;
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

  String apiUrl = kStaCloudApiUrl;
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

static bool loadStaCredentialsFromSettingIni(String &outSsid, String &outPassword) {
  outSsid = "";
  outPassword = "";

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
    }
  }
  f.close();

  return outSsid.length() > 0;
}

static void playStaMessage(const String &text) {
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
    if (irq.immediatePreemptedWeb) {
      irq.webActive = true;
      irq.webKeyLatch = false;
      Serial.println("[WEB] immediate interrupt resume web");
    } else {
      Serial.println("[WEB] immediate interrupt finished");
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

  const bool allowWebInterrupts =
      (gAppLoopMode == APP_MODE_AP_STA) ||
      (gAppLoopMode == APP_MODE_STA_ONLINE &&
       gStaOnlinePhase == StaOnlinePhase::kConnected);

  if (allowWebInterrupts) {
    String hostBroadcastMessage;
    if (wirelessPortalPopHostMessage(hostBroadcastMessage)) {
      preemptByHost();
      playMessageWithGlitch(hostBroadcastMessage.c_str());
      return;
    }

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
            Serial.println("[WEB] interrupt finished");
          }
        } else {
          irq.webActive = false;
          irq.webKeyLatch = false;
          Serial.println("[WEB] interrupt finished");
        }
        if (irq.webActive) {
          return;
        }
        // Web queue finished: require a new physical keypress before AP normal flow continues.
        return;
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
  }
  else
  {
    switchAppMode(APP_MODE_AP_STA);
  }
}

void onApStaInit(AppLoopMode mode)
{
  (void)mode;
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNetSsid = "";
  gStaNetPassword = "";
  clearStaMessageQueue();
  if (!wirelessPortalStart()) {
    Serial.println("[AP] wirelessPortalStart failed on AP init");
  }
}

void onStaOnlineInit(AppLoopMode mode)
{
  (void)mode;
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
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  clearStaMessageQueue();

  const bool loaded = loadStaCredentialsFromSettingIni(gStaNetSsid, gStaNetPassword);
  if (!loaded) {
    Serial.println("[STA] NetSSID missing in /setting.ini");
  } else {
    Serial.printf("[STA] target ssid: %s\\n", gStaNetSsid.c_str());
  }

  ensureStaFetcherTaskStarted();
  playStaMessage(kStaPromptMsg);
}

void onStaOnlyInit(AppLoopMode mode)
{
  (void)mode;
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  wirelessPortalStop();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  clearStaMessageQueue();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}
