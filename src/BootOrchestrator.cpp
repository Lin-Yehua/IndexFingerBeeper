/*
 * 文件说明: 预启动总控实现文件。
 * 文件功能: 编排分区日志、USB MSC 初始化、开机动画、USB/APP 分流和深睡快恢复。
 *
 * 函数表:
 * - logAppPartitionLine: 模块内部辅助函数。
 * - logBootPartitionInfo: 模块内部辅助函数。
 * - initUsbMsc: 初始化或确保对应资源可用。
 * - beginUsbDevice: 初始化或确保对应资源可用。
 * - waitUsbHostActive: 模块内部辅助函数。
 * - enterInitialApp: 模块内部辅助函数。
 * - routeFastResume: 模块内部辅助函数。
 * - routeColdBoot: 模块内部辅助函数。
 * - routeBootPath: 选择冷启动、USB 模式或深睡快恢复启动路径。
 */
#include "BootOrchestrator.h"
#include "AppGlobals.h"
#include "Ds1302Rtc.h"
#include "UsbAppMode.h"
#include "esp_system.h"
#include <Arduino.h>
#include <USB.h>
#include <esp_ota_ops.h>

namespace
{

// 打印当前运行分区和下次启动分区，便于 OTA/回滚问题定位。
void logAppPartitionLine(const char *tag, const esp_partition_t *part)
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

// 汇总启动分区状态。
void logBootPartitionInfo()
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

// 配置 USB MSC 回调与设备描述，供电脑识别 FAT 分区。
void initUsbMsc()
{
  msc.vendorID("ESP32");
  msc.productID("S3_FAT_MSC");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(false);
}

// 初始化 USB 枚举前所需的原始 FAT 后端。
bool beginUsbDevice()
{
  if (!openRawBackend())
  {
    Serial.println("[BOOT] raw FAT backend failed");
    return false;
  }
  if (!msc.begin(sectorCount, static_cast<uint16_t>(mscBlockSize)))
  {
    closeRawBackend();
    Serial.println("[BOOT] MSC begin failed");
    return false;
  }
  closeRawBackend();

  USB.onEvent(onUsbEvent);
  USB.begin();
  Serial.println("[BOOT] USB initialized");
  return true;
}

// USB 事件有轻微延迟，启动后短暂等待主机接入信号。
bool waitUsbHostActive()
{
  const uint32_t t0 = millis();
  while (millis() - t0 < 300)
  {
    if (usbHostActive)
      return true;
    delay(10);
  }
  return usbHostActive;
}

// 进入 APP 模式并执行更新、资源加载与启动模式恢复。
void enterInitialApp(bool restoreSleepSnapshot)
{
  Serial.println("[BOOT] USB not detected -> APP mode");
  if (!enterAppMode())
    return;

  applyPendingFatUpdatesFromUpdateDir();
  if (!initProjectResources())
    return;

  if (restoreSleepSnapshot)
  {
    if (!appRestoreFromDeepSleepSnapshot())
    {
      applyStartupModeFromSettingIni();
    }
    return;
  }

  applyStartupModeFromSettingIni();
}

// 深睡按键/定时唤醒时走快恢复，避开完整 USB 枚举等待链路。
void routeFastResume()
{
  Serial.println("[BOOT] deep-sleep key wake -> fast resume");
  rtc.begin();
  usbHostActive = false;
  usbHostActivePrev = false;
  usbModeActive = false;
  Serial.println("[BOOT] partition check (fast-resume path)");
  logBootPartitionInfo();
  enterInitialApp(true);
}

// 常规冷启动：USB 初始化、开机动画、USB/APP 分流。
void routeColdBoot()
{
  initUsbMsc();
  rtc.begin();

  if (!beginUsbDevice())
  {
    while (true)
      delay(1000);
  }

  runBootAnimationTaskStart();
  runBootAnimationTaskWait();
  Serial.println("[BOOT] partition check (post-animation)");
  logBootPartitionInfo();

  const bool forceAppUpdateBoot = appConsumeForceAppUpdateBoot();
  if (forceAppUpdateBoot)
  {
    Serial.println("[UPDATE] force APP boot after USB eject");
  }
  else
  {
    (void)waitUsbHostActive();
  }

  if (!forceAppUpdateBoot && usbHostActive)
  {
    Serial.println("[BOOT] USB detected -> USB mode");
    enterUsbMode();
    return;
  }

  enterInitialApp(false);
}

} // namespace

void routeBootPath()
{
  logBootPartitionInfo();

  // Enable audio output path early so boot animation BGM can be heard.
  pinMode(42, OUTPUT);
  digitalWrite(42, HIGH);
  (void)appHandleRtcMaintenanceWakeIfNeeded();

  if (appShouldFastResumeFromDeepSleep())
  {
    routeFastResume();
  }
  else
  {
    routeColdBoot();
  }

  usbHostActivePrev = usbHostActive;
}
