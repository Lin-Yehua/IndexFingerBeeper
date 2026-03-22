#include <Arduino.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <FFat.h>
#include <USB.h>
#include <USBMSC.h>
#include <vector>
#include <ctype.h>

extern "C" {
#include "wear_levelling.h"
#include "esp_partition.h"
}

#include <Logo_Moon_B.h>
#include <Index_B.h>
#include "esp_system.h"
#include "WavMixerI2S.h"
#include "CsvTextReader.h"
#include <Key_Drv.h>

static constexpr const char *kFatPartitionLabel = "fatfs";
static constexpr const char *kFatMountPoint = "/fat";
static constexpr const char *kLittleFsPartitionLabel = "littlefs";

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite Text = TFT_eSprite(&tft);
TFT_eSprite spriteBoot = TFT_eSprite(&tft);
CsvTextReader csv;
WavMixerI2S mixer;

const char *message = nullptr;
const char *junkChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01289!@#$%^&*()[]{}<>?/|\\~`+-=_";

USBMSC msc;
wl_handle_t wlHandle = WL_INVALID_HANDLE;
const esp_partition_t *fatPart = nullptr;
size_t flashBytes = 0;
uint32_t sectorCount = 0;
uint32_t mscBlockSize = 512;

bool fatMounted = false;
bool usbModeActive = false;
volatile bool usbHostActive = false;
bool usbHostActivePrev = false;
bool usbDisconnectedLogged = false;

bool appInitialized = false;
bool displayBootstrapped = false;
uint8_t Sound_count = 0;
uint8_t csvCount = 0;
int csvArray[8192] = {0};
float gInsertGain = 0.2f;
float gBgGain = 0.2f;
int gWrongProb3 = 25;
int gWrongProb5 = 12;
bool gEnableReprint = true;
bool firstFlag = false;
uint8_t RUNSTATE = 0;
void showGlitchEffectUTF8(const char *text);
void task_LogoFadeInAndMove(void *pvParameters);
void ensureDisplayReady();
void showUsbModeScreen();
void applyAudioGainsFromSettingIni();
void generateUniqueRandomNumbers(int low, int high, int count, int* result);

void ensureDisplayReady() {
  if (displayBootstrapped) return;
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  displayBootstrapped = true;
}

void showUsbModeScreen() {
  ensureDisplayReady();
  
  spriteBoot.createSprite(320,120);
  tft.fillScreen(TFT_BLACK);
  spriteBoot.setTextWrap(false, false);
  spriteBoot.setTextColor(0xff36, TFT_BLACK);
  spriteBoot.setTextSize(2);
  delay(300);
  ledcWrite(0,255);
  //spriteBoot.pushImage(160-60, 0-60, 120, 120, (uint16_t*)Index_B);
  const char *line1 = "USB MODE";
  const char *line2 = "Mass Storage Connected";
  const char *line3 = "Edit files on your PC...";

  auto typeLine = [&](int x, int y, const char *line, int stepDelayMs) {
    String buf;
    for (int i = 0; line[i] != '\0'; ++i) {
      buf += line[i];
      //spriteBoot.pushImage(160-60, 0, 120, 120, (uint16_t*)Index_B);
      //spriteBoot.fillRect(x, y, 320 - x, 20, TFT_WHITE);
      spriteBoot.setCursor(x, y);
      spriteBoot.print(buf);
      spriteBoot.pushSprite(0, 100);
      delay(stepDelayMs);
    }

  };

  typeLine(18, 20, line1, 35);
  typeLine(18, 60, line2, 22);
  typeLine(18, 100, line3, 18);
}

bool wlWriteRmw(size_t addr, const uint8_t *src, size_t len) {
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
  Text.loadFont("Oxta14",LittleFS);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0xff36,0x0000);
  Text.drawString("PROJECT MOON",185,60);
  Text.setTextWrap(true,true);

  xTaskCreate(task_LogoFadeInAndMove, "LogoFadeMove", 20480, NULL, 1, NULL);

  message = csv.getTextById(1);
  Text.unloadFont();
  Text.loadFont("simhei15", LittleFS);
  Text.setTextColor(0x07ff, TFT_BLACK);

  delay(8000);
  
  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
  /*
  mixer.playBG("/BG.wav");
  mixer.playInsert("/BGstart.wav");
  showGlitchEffectUTF8(message ? message : "CSV message missing");
  mixer.stopBG();
  mixer.playInsert("/BGend.wav");
  */
  firstFlag = true;
  appInitialized = true;
  Serial.println("[APP] project initialized");
  return true;
}

void processAppLoop() {
  if(RUNSTATE == 0)
  {
    generateUniqueRandomNumbers(1,csv.size(),csv.size(),csvArray);
    RUNSTATE = 1;
  }
  if(RUNSTATE == 1)  
  {
    Key_loop();
    uint8_t key = get_Keycode();

    if (key == 2 || firstFlag) {
      csvCount++;
      if(firstFlag)
      {
        firstFlag = false;
      }
      if (csvCount > csv.size())
      {
        csvCount = 0;
        RUNSTATE = 0;
        return;
      }
      
      message = csv.getTextById(csvArray[csvCount]);
      if (!message) 
      {
        String messageFallback = "CSV id not found: ";
        messageFallback += String(csvArray[csvCount]);
        message = messageFallback.c_str();
      }
      mixer.playBG("/BG.wav");
      mixer.playInsert("/BGstart.wav");
      showGlitchEffectUTF8(message);
      mixer.stopBG();
      mixer.playInsert("/BGend.wav");
      //tft.fillRect(30,150,30,30,0x0000);
      //tft.drawNumber(csvCount,30,160);
      //tft.drawNumber(csvArray[csvCount],30,180);
    
    }
  }
  
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] project + USB MSC + FAT CSV");
  ledcSetup(0, 40000, 8);
  ledcAttachPin(14, 0);
  ledcWrite(0, 0);
  msc.vendorID("ESP32");
  msc.productID("S3_FAT_MSC");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(false);

  if (!openRawBackend()) {
    Serial.println("[BOOT] raw FAT backend failed");
    while (true) delay(1000);
  }
  if (!msc.begin(sectorCount, static_cast<uint16_t>(mscBlockSize))) {
    Serial.println("[BOOT] MSC begin failed");
    while (true) delay(1000);
  }
  closeRawBackend();

  USB.onEvent(onUsbEvent);
  USB.begin();
  Serial.println("[BOOT] USB initialized");

  const uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    if (usbHostActive) break;
    delay(10);
  }

  if (usbHostActive) {
    Serial.println("[BOOT] USB detected -> USB mode");
    enterUsbMode();
  } else {
    Serial.println("[BOOT] USB not detected -> APP mode");
    if (enterAppMode()) {
      initProjectResources();
    }
  }
  usbHostActivePrev = usbHostActive;
}

void loop() {
  if (usbHostActive != usbHostActivePrev) {
    if (usbHostActive) {
      Serial.println("[AUTO] USB plugged -> USB mode");
      enterUsbMode();
    } else {
      Serial.println("[AUTO] USB unplugged -> restart app");
      Serial.println("[LOG] USB disconnected, restarting for clean APP state");
      delay(120);
      esp_restart();
    }
    usbHostActivePrev = usbHostActive;
  }

  if (!usbModeActive && appInitialized) {
    processAppLoop();
  }

  delay(20);
}

void showGlitchEffectUTF8(const char* text) {
  String chars[32];
  int charCount = 0;
  int keycode = 255;
  bool rollbackEnabled = gEnableReprint;
  bool forceFinishNow = false;
  bool keyLatch = false;
  // UTF-8 分割
  for (int i = 0; text[i] != '\0' && charCount < 32;) {
    uint8_t c = (uint8_t)text[i];
    int charLen = 1;

    if ((c & 0x80) == 0x00) charLen = 1;         // ASCII
    else if ((c & 0xE0) == 0xC0) charLen = 2;    // 2-byte UTF-8
    else if ((c & 0xF0) == 0xE0) charLen = 3;    // 3-byte UTF-8
    else if ((c & 0xF8) == 0xF0) charLen = 4;    // 4-byte UTF-8

    chars[charCount] = "";
    for (int j = 0; j < charLen; j++) {
      chars[charCount] += text[i + j];
    }

    i += charLen;
    charCount++;
  }

  auto mutateCharNearBoundary = [&](const String& src) -> String {
    int n = src.length();
    if (n <= 0) return src;

    uint8_t b[4] = {0, 0, 0, 0};
    for (int k = 0; k < n && k < 4; ++k) b[k] = (uint8_t)src[k];

    // ASCII: perturb code point directly.
    if (n == 1 && (b[0] & 0x80) == 0) {
      int delta = random(1, 16);
      if (random(2) == 0) delta = -delta;
      int v = (int)b[0] + delta;
      while (v < 33) v += 94;
      while (v > 126) v -= 94;
      String out;
      out += (char)v;
      return out;
    }

    // UTF-8 multi-byte: perturb continuation bytes, keep valid UTF-8 framing.
    for (int k = 1; k < n && k < 4; ++k) {
      if ((b[k] & 0xC0) == 0x80) {
        int delta = random(1, 16);
        if (random(2) == 0) delta = -delta;
        int v = (int)b[k] + delta;
        if (v < 0x80) v = 0x80 + (0x80 - v);
        if (v > 0xBF) v = 0xBF - (v - 0xBF);
        if (v < 0x80) v = 0x80;
        if (v > 0xBF) v = 0xBF;
        b[k] = (uint8_t)v;
      }
    }

    char outBuf[5] = {0};
    for (int k = 0; k < n && k < 4; ++k) outBuf[k] = (char)b[k];
    return String(outBuf);
  };

  bool wrongActive[32] = {false};
  String wrongChars[32];
  int rollbackCooldown = 0;

  auto tryActivateWrong = [&](int j, int wrongProb) {
    if (j < 0 || j >= charCount) return;
    if (wrongActive[j]) return;
    if (random(100) >= wrongProb) return;

    String candidate = chars[j];
    for (int t = 0; t < 6; ++t) {
      candidate = mutateCharNearBoundary(chars[j]);
      if (candidate != chars[j] && candidate != wrongChars[j]) break;
    }
    if (candidate != chars[j]) {
      wrongChars[j] = candidate;
      wrongActive[j] = true;
    }
  };

  int i = 0;
  while (i < charCount + 5) {
    int steps = 2 + random(3);  // 每个字符跳 2~4 次

    for (int s = 0; s < steps; s++) {
      String display = "";

      for (int j = 0; j < charCount; j++) {
        if (j < i) {
          int dist = i - j;
          if (dist < 5) {
            int wrongProb = (dist < 3) ? gWrongProb3 : gWrongProb5;
            bool isUtf8 = chars[j].length() > 1;
            if (i < charCount) {
              tryActivateWrong(j, wrongProb);
            } else if (isUtf8) {
              // tail flush phase: only UTF-8 perturbation
              tryActivateWrong(j, wrongProb);
            }
            display += wrongActive[j] ? wrongChars[j] : chars[j];
          } else {
            wrongActive[j] = false;  // left error zone
            display += chars[j];
          }
        } else if (j == i) {
          display += chars[j];   // 当前字符直接显示
        } else {
          char junk = junkChars[random(strlen(junkChars))];
          display += junk;       // 后面用 ASCII 乱码代替
        }
      }

      Text.fillRect(0, 60, tft.width(), 20, TFT_BLACK);
      Text.pushImage(160 - 60, 0, 120, 120, (uint16_t*)Index_B);
      Text.drawString(display, 160, 70);
      Text.pushSprite(0, 150, 0, 60, 320, 20);
      delay(10);
      Key_loop(); // 处理按键，保持系统响应
      keycode = get_Keycode();
      if (keycode == 2 && !keyLatch) {
        keyLatch = true;
        if (!gEnableReprint) {
          forceFinishNow = true;     // reprint disabled in config: one press to finish
        } else if (rollbackEnabled) {
          rollbackEnabled = false;   // first press: disable rollback
        } else {
          forceFinishNow = true;     // second press: show full text and exit
        }
      }
      if (keycode != 2) {
        keyLatch = false;
      }
      if (forceFinishNow) break;
    }
    if (forceFinishNow) break;

    // 固定当前字符后的正式显示
    String display = "";
    for (int j = 0; j < charCount; j++) {
      if (j <= i) {
        const int dist = i - j;
        if (dist < 5) {
          int wrongProb = (dist < 3) ? gWrongProb3 : gWrongProb5;
          bool isUtf8 = chars[j].length() > 1;
          if (i < charCount) {
            tryActivateWrong(j, wrongProb);
          } else if (isUtf8) {
            // tail flush phase: only UTF-8 perturbation
            tryActivateWrong(j, wrongProb);
          }
          display += wrongActive[j] ? wrongChars[j] : chars[j];
        } else {
          wrongActive[j] = false;  // left error zone
          display += chars[j];
        }
      } else {
        char junk = junkChars[random(strlen(junkChars))];
        display += junk; 
      }
    }

	if(random(1,100) <= 30 + Sound_count)
	{
		Sound_count = 0;
		mixer.playInsert("/BB2.wav");
	}
	else
	{
		Sound_count += 5;
	}
	
    Text.fillRect(0, 60, tft.width(), 20, TFT_BLACK);
    Text.pushImage(160 - 60, 0, 120, 120, (uint16_t*)Index_B);
    Text.drawString(display, 160, 70);
    Text.pushSprite(0, 150, 0, 60, 320, 20);
    delay(20);
    Key_loop();
    keycode = get_Keycode();
    if (keycode == 2 && !keyLatch) {
      keyLatch = true;
      if (!gEnableReprint) {
        forceFinishNow = true;
      } else if (rollbackEnabled) {
        rollbackEnabled = false;
      } else {
        forceFinishNow = true;
      }
    }
    if (keycode != 2) {
      keyLatch = false;
    }
    if (forceFinishNow) break;

    // If the last decoded 5 chars are all wrong at once, rollback decode progress by 5.
    if (i >= charCount) {
      i++;
      continue;
    }

    if (rollbackCooldown > 0) {
      rollbackCooldown--;
      i++;
      continue;
    }

    if (rollbackEnabled && i >= 4) {
      bool allWrong = true;
      for (int j = i - 4; j <= i; ++j) {
        if (j < 0 || j >= charCount || !wrongActive[j]) {
          allWrong = false;
          break;
        }
      }
      if (allWrong) {
        i -= 5;
        if (i < 0) i = 0;
        rollbackCooldown = 5;
        // clear nearby latched wrong states to avoid immediate re-trigger loops
        int clearL = i - 2;
        if (clearL < 0) clearL = 0;
        int clearR = i + 6;
        if (clearR >= charCount) clearR = charCount - 1;
        for (int j = clearL; j <= clearR; ++j) {
          wrongActive[j] = false;
          wrongChars[j] = "";
        }
      } else {
        i++;
      }
    } else {
      i++;
    }
  }

  // Ensure the very last frame is fully corrected.
  String finalDisplay = "";
  for (int j = 0; j < charCount; ++j) {
    finalDisplay += chars[j];
  }
  Text.fillRect(0, 60, tft.width(), 20, TFT_BLACK);
  Text.pushImage(160 - 60, 0, 120, 120, (uint16_t*)Index_B);
  Text.drawString(finalDisplay, 160, 70);
  Text.pushSprite(0, 150, 0, 60, 320, 20);
}

void task_LogoFadeInAndMove(void *pvParameters)
{
    uint16_t LogoTemp[8100] = {0x0000};
    
    const uint16_t* pLogo =nullptr;
    pLogo = (uint16_t*)Logo_Moon_B;
    // 淡入階段
    tft.pushImage(160-45, 150-45, 90, 90, (uint16_t*)Logo_Moon_B);
    for(uint8_t N = 0; N < 48; N++)
    {
        ledcWrite(0,N*5 );
        delay(30);
    }
    delay(500);

    uint8_t steps = 80; // 原来的 dx 从0到78，每次+2，总共约40步
    float progress;
    float eased;
    int dx;
    int d1;
    for(uint8_t i = 0; i <= steps; i++)
    {
        // 计算进度 0.0 ~ 1.0
        progress = (float)i / steps;

        // 使用 sin 缓动函数实现 ease-in-out
        // 公式: eased = 0.5 * (1 - cos(pi * progress))
        eased = 0.5 * (1 - cos(progress * 3.1415926));

        // 根据缓动值计算 dx
        dx = (int)(eased * 85); // 最终目标 0 ~ 80
        d1 = (int)(eased * 160); 
        // 绘制图片
        Text.pushSprite(160+40-dx,145,100,50,d1,20);
        tft.pushImage(160-45-dx, 150-45, 90, 90, (uint16_t*)Logo_Moon_B);
        delay(30);
    }
    delay(2000);
    for(uint8_t N = 0; N < 48; N++)
    {
        ledcWrite(0,(48-(N+1))*5 );
        delay(30);
    }
    tft.fillRect(0,50,320,140,0x0000);
    delay(50);
    ledcWrite(0,255);
    tft.pushImage(160-60, 150-60, 120, 120, (uint16_t*)Index_B);
	//tft.fillScreen(TFT_WHITE);
    vTaskDelete(NULL);
}

void generateUniqueRandomNumbers(int low, int high, int count, int* result) 
{
    if (!result) return;
    if (low > high) return;
    if (count <= 0) return;

    const int range = high - low + 1;
    int need = count;
    if (need > range) need = range;


    if (range > 64) {
  
        return;
    }

    bool used[64] = { false };

    int generated = 0;
    while (generated < need) {
        int r = random(low, high + 1);
        int idx = r - low;          
        if (!used[idx]) {
            used[idx] = true;
            result[generated++] = r;
        }

    }

}
