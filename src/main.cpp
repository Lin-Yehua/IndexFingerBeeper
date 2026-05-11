#include "AppGlobals.h"
#include "Ds1302Rtc.h"
#include "UsbAppMode.h"
#include "WirelessPortal.h"
#include "esp_system.h"
#include <Arduino.h>
#include <USB.h>
#include <esp_ota_ops.h>

static void logAppPartitionLine(const char *tag, const esp_partition_t *part)
{
  if (!part)
  {
    Serial.printf("[BOOT] %s partition: <null>\n", tag);
    return;
  }

  const bool isOta =
      (part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN) && (part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX);
  if (isOta)
  {
    const int otaSlot = static_cast<int>(part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    Serial.printf("[BOOT] %s partition: label=%s type=ota_%d addr=0x%06lX size=0x%06lX\n", tag, part->label, otaSlot,
                  static_cast<unsigned long>(part->address), static_cast<unsigned long>(part->size));
  }
  else
  {
    Serial.printf("[BOOT] %s partition: label=%s subtype=0x%02X addr=0x%06lX size=0x%06lX\n", tag, part->label,
                  static_cast<unsigned>(part->subtype), static_cast<unsigned long>(part->address),
                  static_cast<unsigned long>(part->size));
    Serial.printf("[BOOT] warning: %s is not an OTA slot in dual-OTA layout\n", tag);
  }
}

static void logBootPartitionInfo()
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *configuredBoot = esp_ota_get_boot_partition();
  logAppPartitionLine("running", running);
  logAppPartitionLine("configured boot", configuredBoot);
  if (running && configuredBoot && running != configuredBoot)
  {
    Serial.println("[BOOT] warning: running partition != configured boot partition");
  }
}

void setup()
{
  Serial.begin(115200);
  // delay(300);
  Serial.println("\n[BOOT] project + USB MSC + FAT CSV");
  logBootPartitionInfo();
  setAppModeInitCallback(APP_MODE_AP_STA, onApStaInit);
  setAppModeInitCallback(APP_MODE_STA_ONLINE, onStaOnlineInit);
  setAppModeInitCallback(APP_MODE_STA_ONLY, onStaOnlyInit);

  ledcSetup(0, 40000, 8);
  ledcAttachPin(14, 0);
  ledcWrite(0, 0);
  // Enable audio output path early so boot animation BGM can be heard.
  pinMode(42, OUTPUT);
  digitalWrite(42, HIGH);
  (void)appHandleRtcMaintenanceWakeIfNeeded();

  const bool fastResume = appShouldFastResumeFromDeepSleep();
  if (fastResume)
  {
    Serial.println("[BOOT] deep-sleep key wake -> fast resume");
    rtc.begin();
    usbHostActive = false;
    usbHostActivePrev = false;
    usbModeActive = false;
    Serial.println("[BOOT] partition check (fast-resume path)");
    logBootPartitionInfo();
    if (enterAppMode())
    {
      applyPendingFatUpdatesFromUpdateDir();
      if (initProjectResources())
      {
        if (!appRestoreFromDeepSleepSnapshot())
        {
          applyStartupModeFromSettingIni();
        }
      }
    }
  }
  else
  {
    msc.vendorID("ESP32");
    msc.productID("S3_FAT_MSC");
    msc.productRevision("1.0");
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.onStartStop(onStartStop);
    msc.mediaPresent(false);
    rtc.begin();

    if (!openRawBackend())
    {
      Serial.println("[BOOT] raw FAT backend failed");
      while (true)
        delay(1000);
    }
    if (!msc.begin(sectorCount, static_cast<uint16_t>(mscBlockSize)))
    {
      Serial.println("[BOOT] MSC begin failed");
      while (true)
        delay(1000);
    }
    closeRawBackend();

    USB.onEvent(onUsbEvent);
    USB.begin();
    Serial.println("[BOOT] USB initialized");

    runBootAnimationTaskStart();
    runBootAnimationTaskWait();
    Serial.println("[BOOT] partition check (post-animation)");
    logBootPartitionInfo();

    const bool forceAppUpdateBoot = appConsumeForceAppUpdateBoot();
    if (!forceAppUpdateBoot)
    {
      const uint32_t t0 = millis();
      while (millis() - t0 < 300)
      {
        if (usbHostActive)
          break;
        delay(10);
      }
    }
    else
    {
      Serial.println("[UPDATE] force APP boot after USB eject");
    }

    if (!forceAppUpdateBoot && usbHostActive)
    {
      Serial.println("[BOOT] USB detected -> USB mode");
      enterUsbMode();
    }
    else
    {
      Serial.println("[BOOT] USB not detected -> APP mode");
      if (enterAppMode())
      {
        applyPendingFatUpdatesFromUpdateDir();
        if (initProjectResources())
        {
          applyStartupModeFromSettingIni();
        }
      }
    }
  }
  usbHostActivePrev = usbHostActive;
}

void loop()
{
  if (appConsumeUpdateRebootRequest())
  {
    Serial.println("[UPDATE] rebooting to enter APP update flow");
    delay(120);
    esp_restart();
  }
  // 检测USB模式
  if (usbHostActive != usbHostActivePrev)
  {
    if (usbHostActive)
    {
      Serial.println("[AUTO] USB plugged -> USB mode");
      wirelessPortalStop();
      enterUsbMode();
    }
    else
    {
      Serial.println("[AUTO] USB unplugged -> restart app");
      Serial.println("[LOG] USB disconnected, restarting for clean APP state");
      delay(120);
      esp_restart();
    }
    usbHostActivePrev = usbHostActive;
  }
  // 真正的mainLoop
  if (!usbModeActive && appInitialized)
  {
    processAppLoop();
  }

  delay(20);
}
