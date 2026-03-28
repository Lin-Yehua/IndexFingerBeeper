#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <FFat.h>
#include <USB.h>
#include <vector>
#include <ctype.h>
#include "esp_system.h"
#include "AppGlobals.h"
#include "UsbAppMode.h"
#include "DisplayEffects.h"
#include "WirelessPortal.h"
#include "Index_B.h"
#include "Key_Drv.h"

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
  ledcWrite(0, 255);

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

  gInsertGain = kDefaultInsertGain;
  gBgGain = kDefaultBgGain;
  gWrongProb3 = kDefaultWrongProb3;
  gWrongProb5 = kDefaultWrongProb5;
  gEnableReprint = kDefaultEnableReprint;

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
    } else if (key == "testwrongindexpersent_3area") {
      gWrongProb3 = constrain(value.toInt(), 0, 100);
      gotWrong3 = true;
    } else if (key == "testwrongindexpersent_5area") {
      gWrongProb5 = constrain(value.toInt(), 0, 100);
      gotWrong5 = true;
    } else if (key == "enablereprint") {
      gEnableReprint = (value == "1" || value == "true" || value == "on" || value == "yes");
      gotReprint = true;
    }
  }
  f.close();

  if (!gotInsert) Serial.printf("[APP] InsertGain missing, default=%.3f\n", gInsertGain);
  if (!gotBg) Serial.printf("[APP] BackGroundGain missing, default=%.3f\n", gBgGain);
  if (!gotWrong3) Serial.printf("[APP] TestWrongIndexPersent_3Area missing, default=%d\n", gWrongProb3);
  if (!gotWrong5) Serial.printf("[APP] TestWrongIndexPersent_5Area missing, default=%d\n", gWrongProb5);
  if (!gotReprint) Serial.printf("[APP] EnableReprint missing, default=%d\n", gEnableReprint ? 1 : 0);
  Serial.printf("[APP] gains: insert=%.3f bg=%.3f\n", gInsertGain, gBgGain);
  Serial.printf("[APP] glitch: p3=%d p5=%d reprint=%d\n", gWrongProb3, gWrongProb5, gEnableReprint ? 1 : 0);
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
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  delay(500);

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

  bool missing = false;
  if (!LittleFS.exists("/simhei15.vlw")) missing = true;
  if (!LittleFS.exists("/Oxta14.vlw")) missing = true;
  if (missing) {
    Serial.println("[APP] LittleFS font files missing");
    Serial.println("[APP] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
    return false;
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
  applyAudioGainsFromSettingIni();

  if (!csv.load(FFat, "/data.csv")) {
    Serial.println("[APP] /data.csv load failed from FAT");
    return false;
  }

  Text.createSprite(320, 100);
  Text.loadFont("Oxta14", LittleFS);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0xff36, 0x0000);
  Text.drawString("PROJECT MOON", 180, 60);
  Text.setTextWrap(true, true);
  spriteBG.createSprite(320, 120);
  spriteBG.fillRect(0,0,320,120,0x0000);
  spriteBG.pushImage(160 -60, 0, 120,120,(uint16_t*)Index_B);
  xTaskCreate(task_LogoFadeInAndMove, "LogoFadeMove", 20480, NULL, 1, NULL);

  message = csv.getTextById(1);
  Text.unloadFont();
  Text.loadFont("simhei15", LittleFS);
  Text.setTextColor(0x07ff, TFT_BLACK);

  delay(8000);

  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
  firstFlag = true;
  appInitialized = true;
  Serial.println("[APP] project initialized");
  return true;
}

static void playMessageWithGlitch(const char *text) {
  if (!text || !text[0]) return;

  mixer.playBG("/BG.wav");
  mixer.playInsert("/BGstart.wav");
  Text.fillRect(0, 0, tft.width(), 100, TFT_BLACK);
  Text.pushImage(160 - 60, 50 - 60, 120, 120, (uint16_t *)Index_B);
  Text.pushSprite(0, 150 - 50);
  showGlitchEffectUTF8(text);
  mixer.stopBG();
  mixer.playInsert("/BGend.wav");
}

void processAppLoop() {
  if (wirelessPortalConsumeCsvReloadRequest()) {
    if (csv.load(FFat, "/data.csv")) {
      csvCount = 0;
      RUNSTATE = 0;
      Serial.println("[WEB] /data.csv reloaded");
    } else {
      Serial.println("[WEB] /data.csv reload failed");
    }
  }

  String queuedMessage;
  if (wirelessPortalPopMessage(queuedMessage)) {
    do {
      playMessageWithGlitch(queuedMessage.c_str());
    } while (wirelessPortalPopMessage(queuedMessage));
    return;
  }

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
  if (RUNSTATE == 1) {
    Key_loop();
    uint8_t key = get_Keycode();

    if (key == 2 || firstFlag) {
      if (firstFlag) {
        firstFlag = false;
      }
      if (csvCount >= csvTotal) {
        csvCount = 0;
        RUNSTATE = 0;
        return;
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

      //tft.drawNumber(currentCsvId,20,100);
      //tft.drawNumber(csvCount,20,120);
    }
  }
}
