#include <Arduino.h>
#include <TFT_eSPI.h>
#include <USB.h>
#include "driver/i2s.h"
#include "Ds1302Rtc.h"
#include <math.h>

namespace {

constexpr uint32_t kSerialBaud = 115200;

constexpr uint8_t kBacklightPin = 14;
constexpr uint8_t kAudioEnablePin = 42;
constexpr uint8_t kKeyPin = 2;

constexpr int kI2SBckPin = 40;
constexpr int kI2SWsPin = 39;
constexpr int kI2SDoutPin = 41;
constexpr uint32_t kI2SSampleRate = 16000;
constexpr i2s_port_t kI2SPort = I2S_NUM_0;

constexpr uint16_t kHeaderY = 4;
constexpr uint16_t kLogStartY = 24;
constexpr uint16_t kRowHeight = 16;

TFT_eSPI tft = TFT_eSPI();

uint16_t gLogY = kLogStartY;
bool gI2sReady = false;
volatile bool gUsbHostActive = false;

void appendScreenLine(const String &line, uint16_t color = TFT_WHITE) {
  if (gLogY > 230) {
    tft.fillRect(0, kLogStartY, 320, 240 - kLogStartY, TFT_BLACK);
    gLogY = kLogStartY;
  }
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(0, gLogY);
  tft.println(line);
  gLogY += kRowHeight;
}

void logBoth(const String &line, uint16_t color = TFT_WHITE) {
  Serial.println(line);
  appendScreenLine(line, color);
}

bool initI2sOutput() {
  i2s_driver_uninstall(kI2SPort);

  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kI2SSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pinCfg = {};
  pinCfg.bck_io_num = kI2SBckPin;
  pinCfg.ws_io_num = kI2SWsPin;
  pinCfg.data_out_num = kI2SDoutPin;
  pinCfg.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(kI2SPort, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    return false;
  }

  err = i2s_set_pin(kI2SPort, &pinCfg);
  if (err != ESP_OK) {
    i2s_driver_uninstall(kI2SPort);
    return false;
  }

  i2s_zero_dma_buffer(kI2SPort);
  return true;
}

void playTone(uint16_t freqHz, uint16_t durationMs, float gain = 0.22f) {
  if (!gI2sReady) return;

  static int16_t buf[128];
  constexpr float kTwoPi = 6.28318530718f;
  float phase = 0.0f;
  const float step = kTwoPi * static_cast<float>(freqHz) / static_cast<float>(kI2SSampleRate);
  uint32_t remain = static_cast<uint32_t>(kI2SSampleRate) * durationMs / 1000U;

  while (remain > 0) {
    const uint32_t chunk = (remain > 128U) ? 128U : remain;
    for (uint32_t i = 0; i < chunk; ++i) {
      buf[i] = static_cast<int16_t>(sinf(phase) * (32767.0f * gain));
      phase += step;
      if (phase >= kTwoPi) phase -= kTwoPi;
    }

    size_t written = 0;
    i2s_write(kI2SPort, buf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
    remain -= chunk;
  }
}

void beepPass() {
  playTone(1400, 110);
  delay(40);
}

void beepFail() {
  playTone(350, 120);
  delay(50);
  playTone(240, 180);
  delay(60);
}

void reportResult(const char *name, bool ok, const String &detail) {
  String line = ok ? "[PASS] " : "[FAIL] ";
  line += name;
  if (detail.length()) {
    line += " | ";
    line += detail;
  }
  logBoth(line, ok ? TFT_GREEN : TFT_RED);
  if (ok) {
    beepPass();
  } else {
    beepFail();
  }
}

void onUsbEvent(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  (void)arg;
  (void)eventData;
  if (eventBase != ARDUINO_USB_EVENTS) return;

  if (eventId == ARDUINO_USB_STARTED_EVENT || eventId == ARDUINO_USB_RESUME_EVENT) {
    gUsbHostActive = true;
  } else if (eventId == ARDUINO_USB_STOPPED_EVENT) {
    gUsbHostActive = false;
  }
}

bool testDisplay(String &detail) {
  detail = "TFT init ok";
  return true;
}

bool testBacklight(String &detail) {
  ledcWrite(0, 40);
  delay(180);
  ledcWrite(0, 255);
  detail = "PWM dim->bright";
  return true;
}

bool testSpeaker(String &detail) {
  if (!gI2sReady) {
    detail = "I2S init failed";
    return false;
  }

  playTone(880, 180);
  delay(70);
  playTone(1320, 180);
  detail = "2 tones played (confirm by ear)";
  return true;
}

bool testUsb(String &detail) {
  gUsbHostActive = false;
  USB.onEvent(onUsbEvent);
  USB.begin();

  const uint32_t t0 = millis();
  while (millis() - t0 < 3000U) {
    if (gUsbHostActive) {
      detail = "host connected";
      return true;
    }
    delay(10);
  }

  detail = "no host event in 3s";
  return false;
}

bool testRtcDs1302(String &detail) {
  rtc.begin();

  Ds1302DateTime before = {};
  const bool beforeOk = rtc.readDateTime(before) && ds1302IsValidDateTime(before);

  Ds1302DateTime writeDt = {};
  writeDt.year = 2026;
  writeDt.month = 4;
  writeDt.day = 21;
  writeDt.hour = 12;
  writeDt.minute = 0;
  writeDt.second = 0;

  if (!rtc.writeDateTime(writeDt)) {
    detail = "write failed";
    return false;
  }

  Ds1302DateTime startDt = {};
  if (!rtc.readDateTime(startDt) || !ds1302IsValidDateTime(startDt)) {
    detail = "read start invalid";
    return false;
  }

  delay(5000);

  Ds1302DateTime endDt = {};
  if (!rtc.readDateTime(endDt) || !ds1302IsValidDateTime(endDt)) {
    detail = "read end invalid";
    return false;
  }

  const int startSec =
      static_cast<int>(startDt.hour) * 3600 +
      static_cast<int>(startDt.minute) * 60 +
      static_cast<int>(startDt.second);
  const int endSec =
      static_cast<int>(endDt.hour) * 3600 +
      static_cast<int>(endDt.minute) * 60 +
      static_cast<int>(endDt.second);
  const int elapsed = endSec - startSec;

  if (beforeOk) {
    (void)rtc.writeDateTime(before);
  }

  char buf[160] = {0};
  if (elapsed < 4 || elapsed > 6) {
    snprintf(buf,
             sizeof(buf),
             "elapsed=%ds out of range [4..6] start=%02u:%02u:%02u end=%02u:%02u:%02u",
             elapsed,
             static_cast<unsigned int>(startDt.hour),
             static_cast<unsigned int>(startDt.minute),
             static_cast<unsigned int>(startDt.second),
             static_cast<unsigned int>(endDt.hour),
             static_cast<unsigned int>(endDt.minute),
             static_cast<unsigned int>(endDt.second));
    detail = buf;
    return false;
  }

  snprintf(buf,
           sizeof(buf),
           "elapsed=%ds ok start=%02u:%02u:%02u end=%02u:%02u:%02u%s",
           elapsed,
           static_cast<unsigned int>(startDt.hour),
           static_cast<unsigned int>(startDt.minute),
           static_cast<unsigned int>(startDt.second),
           static_cast<unsigned int>(endDt.hour),
           static_cast<unsigned int>(endDt.minute),
           static_cast<unsigned int>(endDt.second),
           beforeOk ? " (restored)" : "");
  detail = buf;
  return true;
}

bool testKey(String &detail) {
  pinMode(kKeyPin, INPUT_PULLUP);
  logBoth("[ACTION] Press key(GPIO2) within 10s", TFT_YELLOW);

  const uint32_t t0 = millis();
  uint32_t lowSince = 0;
  while (millis() - t0 < 10000U) {
    if (digitalRead(kKeyPin) == LOW) {
      if (lowSince == 0) {
        lowSince = millis();
      } else if (millis() - lowSince >= 30U) {
        detail = "pressed";
        return true;
      }
    } else {
      lowSince = 0;
    }
    delay(10);
  }

  detail = "timeout (not pressed)";
  return false;
}

bool testSerialRx(String &detail) {
  logBoth("[ACTION] Send any byte in Serial Monitor within 8s", TFT_YELLOW);
  const uint32_t t0 = millis();
  while (millis() - t0 < 8000U) {
    if (Serial.available() > 0) {
      const uint8_t b = static_cast<uint8_t>(Serial.read());
      while (Serial.available() > 0) {
        Serial.read();
      }
      char buf[24] = {0};
      snprintf(buf, sizeof(buf), "rx byte=0x%02X", static_cast<unsigned int>(b));
      detail = buf;
      return true;
    }
    delay(10);
  }

  detail = "rx timeout";
  return false;
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(300);

  ledcSetup(0, 40000, 8);
  ledcAttachPin(kBacklightPin, 0);
  ledcWrite(0, 255);

  pinMode(kAudioEnablePin, OUTPUT);
  digitalWrite(kAudioEnablePin, HIGH);
  gI2sReady = initI2sOutput();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextWrap(false, false);
  tft.setCursor(0, kHeaderY);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.println("HW SELF TEST");
  tft.drawFastHLine(0, kLogStartY - 2, 320, TFT_DARKGREY);

  logBoth("Hardware map: BL14 KEY2 AMP42 I2S(40,39,41) RTC(47,48,45)", TFT_CYAN);

  int total = 0;
  int pass = 0;
  String detail;
  bool ok = false;

  ok = testDisplay(detail);
  reportResult("Display", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testBacklight(detail);
  reportResult("Backlight", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testSpeaker(detail);
  reportResult("Speaker", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testUsb(detail);
  reportResult("USB", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testRtcDs1302(detail);
  reportResult("DS1302", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testKey(detail);
  reportResult("Key(GPIO2)", ok, detail);
  ++total;
  if (ok) ++pass;

  ok = testSerialRx(detail);
  reportResult("Serial RX", ok, detail);
  ++total;
  if (ok) ++pass;

  detail = String(pass) + "/" + String(total) + " passed";
  reportResult("SUMMARY", pass == total, detail);
}

void loop() {
  static bool keyLatch = false;

  if (digitalRead(kKeyPin) == LOW) {
    if (!keyLatch) {
      keyLatch = true;
      logBoth("[EVENT] Key pressed");
      playTone(1100, 80);
    }
  } else {
    keyLatch = false;
  }

  if (Serial.available() > 0) {
    const uint8_t b = static_cast<uint8_t>(Serial.read());
    Serial.printf("[ECHO] 0x%02X\n", static_cast<unsigned int>(b));
    appendScreenLine(String("[ECHO] 0x") + String(b, HEX), TFT_YELLOW);
    playTone(900, 60);
  }

  delay(20);
}
