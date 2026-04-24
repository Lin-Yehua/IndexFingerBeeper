#include <Arduino.h>
#include <TFT_eSPI.h>
#include <USB.h>
#include "driver/i2s.h"
#include "esp_timer.h"
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

constexpr int kGpio2AdcMinMilliVolts = 2400;
constexpr int kGpio2AdcMaxPeakToPeakMilliVolts = 180;
constexpr int kGpio2AdcMaxAbsDeviationMilliVolts = 90;
constexpr uint16_t kGpio2AdcSampleCount = 40;
constexpr uint16_t kGpio2AdcSampleIntervalMs = 20;

constexpr uint16_t kDs1302DriftObserveSeconds = 40;
constexpr int64_t kDs1302MaxAbsDriftMsPerDay = 45000;  // 45s/day

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

bool isLeapYear(uint16_t year) {
  if ((year % 4U) != 0U) return false;
  if ((year % 100U) != 0U) return true;
  return (year % 400U) == 0U;
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

bool ds1302DateTimeToSecondsSince2000(const Ds1302DateTime &dt, int64_t &outSeconds) {
  if (!ds1302IsValidDateTime(dt)) return false;

  int64_t days = 0;
  for (uint16_t year = 2000; year < dt.year; ++year) {
    days += isLeapYear(year) ? 366 : 365;
  }
  for (uint8_t month = 1; month < dt.month; ++month) {
    days += daysInMonth(dt.year, month);
  }
  days += static_cast<int64_t>(dt.day) - 1;

  outSeconds =
      days * 86400LL +
      static_cast<int64_t>(dt.hour) * 3600LL +
      static_cast<int64_t>(dt.minute) * 60LL +
      static_cast<int64_t>(dt.second);
  return true;
}

bool isDateTimeSecondChanged(const Ds1302DateTime &a, const Ds1302DateTime &b) {
  return a.year != b.year ||
         a.month != b.month ||
         a.day != b.day ||
         a.hour != b.hour ||
         a.minute != b.minute ||
         a.second != b.second;
}

bool waitForRtcSecondTick(Ds1302DateTime &tickDt, uint64_t &tickUs, uint32_t timeoutMs) {
  Ds1302DateTime prev = {};
  if (!rtc.readDateTime(prev) || !ds1302IsValidDateTime(prev)) {
    return false;
  }

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    Ds1302DateTime now = {};
    if (rtc.readDateTime(now) && ds1302IsValidDateTime(now)) {
      if (isDateTimeSecondChanged(now, prev)) {
        tickDt = now;
        tickUs = static_cast<uint64_t>(esp_timer_get_time());
        return true;
      }
      prev = now;
    }
    delay(2);
  }

  return false;
}

int readGpio2MilliVolts() {
  int mv = analogReadMilliVolts(kKeyPin);
  if (mv > 0) return mv;

  const int raw = analogRead(kKeyPin);
  if (raw < 0) return -1;
  return (raw * 3300) / 4095;
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

  Ds1302DateTime backup = {};
  const bool backupOk = rtc.readDateTime(backup) && ds1302IsValidDateTime(backup);
  auto appendRestoreStatus = [&](String &text) {
    if (!backupOk) return;
    if (rtc.writeDateTime(backup)) {
      text += " | restored";
    } else {
      text += " | restore failed";
    }
  };

  Ds1302DateTime writeDt = {};
  writeDt.year = 2026;
  writeDt.month = 4;
  writeDt.day = 24;
  writeDt.hour = 12;
  writeDt.minute = 0;
  writeDt.second = 0;

  if (!rtc.writeDateTime(writeDt)) {
    detail = "write failed";
    appendRestoreStatus(detail);
    return false;
  }

  delay(30);
  Ds1302DateTime verifyDt = {};
  if (!rtc.readDateTime(verifyDt) || !ds1302IsValidDateTime(verifyDt)) {
    detail = "verify read invalid";
    appendRestoreStatus(detail);
    return false;
  }

  int64_t writeSec = 0;
  int64_t verifySec = 0;
  if (!ds1302DateTimeToSecondsSince2000(writeDt, writeSec) ||
      !ds1302DateTimeToSecondsSince2000(verifyDt, verifySec)) {
    detail = "verify convert failed";
    appendRestoreStatus(detail);
    return false;
  }
  if (llabs(verifySec - writeSec) > 1LL) {
    char buf[128] = {0};
    snprintf(buf,
             sizeof(buf),
             "verify mismatch set=%02u:%02u:%02u read=%02u:%02u:%02u",
             static_cast<unsigned int>(writeDt.hour),
             static_cast<unsigned int>(writeDt.minute),
             static_cast<unsigned int>(writeDt.second),
             static_cast<unsigned int>(verifyDt.hour),
             static_cast<unsigned int>(verifyDt.minute),
             static_cast<unsigned int>(verifyDt.second));
    detail = buf;
    appendRestoreStatus(detail);
    return false;
  }

  Ds1302DateTime startDt = {};
  uint64_t startUs = 0;
  if (!waitForRtcSecondTick(startDt, startUs, 3000U)) {
    detail = "tick start timeout";
    appendRestoreStatus(detail);
    return false;
  }

  Ds1302DateTime endDt = startDt;
  uint64_t endUs = startUs;
  for (uint16_t i = 0; i < kDs1302DriftObserveSeconds; ++i) {
    if (!waitForRtcSecondTick(endDt, endUs, 2500U)) {
      char buf[64] = {0};
      snprintf(buf, sizeof(buf), "tick timeout at %u/%u", static_cast<unsigned int>(i), static_cast<unsigned int>(kDs1302DriftObserveSeconds));
      detail = buf;
      appendRestoreStatus(detail);
      return false;
    }
  }

  int64_t startSec = 0;
  int64_t endSec = 0;
  if (!ds1302DateTimeToSecondsSince2000(startDt, startSec) ||
      !ds1302DateTimeToSecondsSince2000(endDt, endSec)) {
    detail = "elapsed convert failed";
    appendRestoreStatus(detail);
    return false;
  }
  const int64_t rtcElapsedSec = endSec - startSec;
  if (rtcElapsedSec <= 0) {
    detail = "elapsed <= 0";
    appendRestoreStatus(detail);
    return false;
  }

  const uint64_t mcuElapsedUs = endUs - startUs;
  if (mcuElapsedUs == 0U) {
    detail = "elapsed us=0";
    appendRestoreStatus(detail);
    return false;
  }

  const int64_t rtcElapsedUs = rtcElapsedSec * 1000000LL;
  const int64_t driftUs = rtcElapsedUs - static_cast<int64_t>(mcuElapsedUs);
  const int64_t driftPpm = (driftUs * 1000000LL) / static_cast<int64_t>(mcuElapsedUs);
  const int64_t driftMsPerDay = (driftUs * 86400000LL) / static_cast<int64_t>(mcuElapsedUs);
  const bool driftOk = llabs(driftMsPerDay) <= kDs1302MaxAbsDriftMsPerDay;
  char buf[192] = {0};

  if (rtcElapsedSec < static_cast<int64_t>(kDs1302DriftObserveSeconds - 1) ||
      rtcElapsedSec > static_cast<int64_t>(kDs1302DriftObserveSeconds + 1)) {
    snprintf(buf,
             sizeof(buf),
             "rtc elapsed out of range=%llds expect~%us",
             static_cast<long long>(rtcElapsedSec),
             static_cast<unsigned int>(kDs1302DriftObserveSeconds));
    detail = buf;
    appendRestoreStatus(detail);
    return false;
  }

  if (!driftOk) {
    snprintf(buf,
             sizeof(buf),
             "drift too large rtc=%llds mcu=%llums day=%lldms ppm=%lld",
             static_cast<long long>(rtcElapsedSec),
             static_cast<unsigned long long>(mcuElapsedUs / 1000ULL),
             static_cast<long long>(driftMsPerDay),
             static_cast<long long>(driftPpm));
    detail = buf;
    appendRestoreStatus(detail);
    return false;
  }

  snprintf(buf,
           sizeof(buf),
           "drift ok rtc=%llds mcu=%llums day=%lldms ppm=%lld",
           static_cast<long long>(rtcElapsedSec),
           static_cast<unsigned long long>(mcuElapsedUs / 1000ULL),
           static_cast<long long>(driftMsPerDay),
           static_cast<long long>(driftPpm));
  detail = buf;
  appendRestoreStatus(detail);
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

bool testGpio2Adc(String &detail) {
  pinMode(kKeyPin, INPUT);
#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
  analogSetPinAttenuation(kKeyPin, ADC_11db);
#endif

  int samples[kGpio2AdcSampleCount] = {0};
  int minMv = 5000;
  int maxMv = 0;
  int64_t sumMv = 0;

  for (uint16_t i = 0; i < kGpio2AdcSampleCount; ++i) {
    const int mv = readGpio2MilliVolts();
    if (mv <= 0) {
      detail = "adc read failed";
      return false;
    }
    samples[i] = mv;
    if (mv < minMv) minMv = mv;
    if (mv > maxMv) maxMv = mv;
    sumMv += mv;

    if (i + 1 < kGpio2AdcSampleCount) {
      delay(kGpio2AdcSampleIntervalMs);
    }
  }

  const int avgMv = static_cast<int>((sumMv + (kGpio2AdcSampleCount / 2)) / kGpio2AdcSampleCount);
  int maxAbsDevMv = 0;
  for (uint16_t i = 0; i < kGpio2AdcSampleCount; ++i) {
    const int dev = abs(samples[i] - avgMv);
    if (dev > maxAbsDevMv) maxAbsDevMv = dev;
  }
  const int ppMv = maxMv - minMv;

  char buf[160] = {0};
  snprintf(buf,
           sizeof(buf),
           "avg=%dmV min=%dmV max=%dmV pp=%dmV dev=%dmV",
           avgMv,
           minMv,
           maxMv,
           ppMv,
           maxAbsDevMv);
  detail = buf;

  if (avgMv < kGpio2AdcMinMilliVolts) {
    detail += " (<2400mV)";
    return false;
  }
  if (ppMv > kGpio2AdcMaxPeakToPeakMilliVolts || maxAbsDevMv > kGpio2AdcMaxAbsDeviationMilliVolts) {
    detail += " (unstable)";
    return false;
  }

  return true;
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

  logBoth("Hardware map: BL14 KEY2/ADC2 AMP42 I2S(40,39,41) RTC(47,48,45)", TFT_CYAN);

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

  ok = testGpio2Adc(detail);
  reportResult("GPIO2 ADC", ok, detail);
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

  delay(20);
}
