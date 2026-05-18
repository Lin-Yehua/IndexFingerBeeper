/*
 * 文件说明: Arduino 主入口文件。
 * 文件功能: 注册 APP 模式回调，初始化基础引脚，并在 loop 中分发 USB/APP 运行逻辑。
 *
 * 函数表:
 * - setup: Arduino 启动入口，完成系统初始化。
 * - loop: Arduino 主循环，周期处理运行任务。
 */
#include "AppGlobals.h"
#include "BootOrchestrator.h"
#include "UsbAppMode.h"
#include "WirelessPortal.h"
#include "esp_system.h"
#include <Arduino.h>

void setup()
{
  Serial.begin(115200);
  // delay(300);
  ledcSetup(0, 40000, 8);
  ledcAttachPin(14, 0);
  ledcWrite(0, 0);
  appCheckLowBatterySleepIfNeeded(true, "early-boot");

  Serial.println("\n[BOOT] project + USB MSC + FAT CSV");
  setAppModeInitCallback(APP_MODE_AP_STA, onApStaInit);
  setAppModeInitCallback(APP_MODE_STA_ONLINE, onStaOnlineInit);
  setAppModeInitCallback(APP_MODE_STA_ONLY, onStaOnlyInit);

  routeBootPath();
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
