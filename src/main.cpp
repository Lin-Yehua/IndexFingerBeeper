#include <Arduino.h>
#include <USB.h>
#include "esp_system.h"
#include "AppGlobals.h"
#include "UsbAppMode.h"

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
