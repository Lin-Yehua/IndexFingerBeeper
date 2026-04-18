#include <Arduino.h>
#include <USB.h>
#include "esp_system.h"
#include "AppGlobals.h"
#include "UsbAppMode.h"
#include "WirelessPortal.h"

void setup() {
  Serial.begin(115200);
  //delay(300);
  Serial.println("\n[BOOT] project + USB MSC + FAT CSV");
  setAppModeInitCallback(APP_MODE_AP_STA,onApStaInit);
  setAppModeInitCallback(APP_MODE_STA_ONLINE,onStaOnlineInit);
  setAppModeInitCallback(APP_MODE_STA_ONLY,onStaOnlyInit);

  ledcSetup(0, 40000, 8);
  ledcAttachPin(14, 0);
  ledcWrite(0, 0);
  const bool fastResume = appShouldFastResumeFromDeepSleep();
  if (fastResume) {
    Serial.println("[BOOT] deep-sleep key wake -> fast resume");
    usbHostActive = false;
    usbHostActivePrev = false;
    usbModeActive = false;
    if (enterAppMode()) {
      if (initProjectResources()) {
        if (!appRestoreFromDeepSleepSnapshot()) {
          applyStartupModeFromSettingIni();
        }
      }
    }
  } else {
    runBootAnimationTaskStart();
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

    runBootAnimationTaskWait();

    const uint32_t t0 = millis();
    while (millis() - t0 < 300) {
      if (usbHostActive) break;
      delay(10);
    }

    if (usbHostActive) {
      Serial.println("[BOOT] USB detected -> USB mode");
      enterUsbMode();
    } else {
      Serial.println("[BOOT] USB not detected -> APP mode");
      if (enterAppMode()) {
        if (initProjectResources()) {
          applyStartupModeFromSettingIni();
        }
      }
    }
  }
  usbHostActivePrev = usbHostActive;
}

void loop() 
{
  //检测USB模式
  if (usbHostActive != usbHostActivePrev) {
    if (usbHostActive) {
      Serial.println("[AUTO] USB plugged -> USB mode");
      wirelessPortalStop();
      enterUsbMode();
    } else {
      Serial.println("[AUTO] USB unplugged -> restart app");
      Serial.println("[LOG] USB disconnected, restarting for clean APP state");
      delay(120);
      esp_restart();
    }
    usbHostActivePrev = usbHostActive;
  }
  //真正的mainLoop
  if (!usbModeActive && appInitialized) {
    processAppLoop();
  }

  delay(20);
}
