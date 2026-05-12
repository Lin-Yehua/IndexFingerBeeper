/*
 * 文件说明: 无线门户聚合编译单元。
 * 文件功能: 引入 wireless_portal 私有实现片段，并管理 Web/DNS 任务和无线门户生命周期。
 *
 * 函数表:
 * - webServerTask: FreeRTOS 任务入口或任务控制函数。
 * - wirelessPortalStart: 模块内部辅助函数。
 * - wirelessPortalStartEspNowOnly: 模块内部辅助函数。
 * - wirelessPortalStop: 模块内部辅助函数。
 * - wirelessPortalPopMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingMessage: 模块内部辅助函数。
 * - wirelessPortalPopImmediateMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingImmediateMessage: 模块内部辅助函数。
 * - wirelessPortalPopHostMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingHostMessage: 模块内部辅助函数。
 * - wirelessPortalPushMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalPushImmediateMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalPushHostMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalConsumeCsvReloadRequest: 模块内部辅助函数。
 * - wirelessPortalConsumeScheduleReloadRequest: 模块内部辅助函数。
 * - wirelessPortalInstantRefreshNoKeyEnabled: 模块内部辅助函数。
 * - wirelessPortalTakePendingImage: 模块内部辅助函数。
 */
#include "WirelessPortal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <FFat.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

#include "AppGlobals.h"
#include "DeviceUuid.h"
#include "Ds1302Rtc.h"
#include "EspNowMessage.h"
#include "UsbAppMode.h"

namespace
{

constexpr char kDefaultApSsid[] = u8"魔法哔哔机";
constexpr char kDefaultApPassword[] = "12345678";
constexpr uint8_t kDefaultApChannel = 1;
constexpr uint8_t kApChannelMin = 1;
constexpr uint8_t kApChannelMax = 13;
constexpr size_t kApSsidMaxLen = 33;
constexpr size_t kApPasswordMaxLen = 65;
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kDnsPort = 53;
constexpr BaseType_t kWebTaskCore = 0;
constexpr UBaseType_t kWebTaskPriority = 1;
constexpr uint32_t kWebTaskStack = 12288;
constexpr size_t kTextMaxLen = 240;
constexpr size_t kQueueDepth = 128;
constexpr size_t kImageUploadByteLimit = 1024 * 1024;
constexpr uint16_t kImageMaxWidth = 320;
constexpr uint16_t kImageMaxHeight = 240;
constexpr uint16_t kImageDefaultWidth = 320;
constexpr uint16_t kImageDefaultHeight = 140;
constexpr int16_t kImageDefaultCenterX = 160;
constexpr int16_t kImageDefaultCenterY = 155;
constexpr char kPrefsNs[] = "wireless";
constexpr char kPrefsInstantRefreshNoKey[] = "instant_no_key";

struct WebQueuedMessage
{
  char text[kTextMaxLen + 1];
};

struct PendingImageFrame
{
  uint16_t *pixels = nullptr;
  size_t pixelCapacity = 0;
  size_t pixelCount = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  int16_t centerX = kImageDefaultCenterX;
  int16_t centerY = kImageDefaultCenterY;
  bool ready = false;
};

struct ImageUploadState
{
  uint8_t *bytes = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  bool failed = false;
  char error[96] = {0};
};

// WirelessPortal 的私有实现按网页配置、消息服务和路由注册拆分。
#include "wireless_portal/StorageConfig.inc"

#include "wireless_portal/MessagingServices.inc"

#include "wireless_portal/Routes.inc"

void webServerTask(void *param)
{
  (void)param;
  gWebServer = new PortalWebServer(kHttpPort);
  gDnsServer = new DNSServer();
  if (!gWebServer || !gDnsServer)
  {
    if (gDnsServer)
    {
      delete gDnsServer;
      gDnsServer = nullptr;
    }
    if (gWebServer)
    {
      delete gWebServer;
      gWebServer = nullptr;
    }
    gWebTaskRunning = false;
    gWebTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  gDnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  gDnsServer->start(kDnsPort, "*", WiFi.softAPIP());

  registerRoutes();
  gWebServer->begin();
  Serial.printf("[WEB] HTTP ready: %s\n", apRootUrl().c_str());

  while (gWebTaskRunning)
  {
    gDnsServer->processNextRequest();
    gWebServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(8));
  }

  gWebServer->stop();
  gDnsServer->stop();
  delete gWebServer;
  delete gDnsServer;
  gWebServer = nullptr;
  gDnsServer = nullptr;
  gWebTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

} // namespace

bool wirelessPortalStart()
{
  if (gPortalStarted)
    return true;

  if (!gImageMutex)
  {
    gImageMutex = xSemaphoreCreateMutex();
    if (!gImageMutex)
    {
      Serial.println("[WEB] image mutex create failed");
      return false;
    }
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    resetImageUploadStateLocked();
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  loadApCredentialsFromSettingIni();
  if (!gEnableAp && !gEnableEspNow)
  {
    Serial.println("[WEB] EnableAP=false and EnableESPNOW=false, wireless off");
    WiFi.mode(WIFI_OFF);
    return true;
  }

  if (gEnableAp)
  {
    if (!LittleFS.exists("/index.html") || !LittleFS.exists("/style.css") || !LittleFS.exists("/app.js"))
    {
      Serial.println("[WEB] missing web files in LittleFS (/index.html /style.css /app.js)");
      Serial.println("[WEB] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
      return false;
    }

    if (!gMessageQueue)
    {
      gMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
      if (!gMessageQueue)
      {
        Serial.println("[WEB] regular queue create failed");
        return false;
      }
    }
  }
  if (gEnableEspNow)
  {
    if (!gHostMessageQueue)
    {
      gHostMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
      if (!gHostMessageQueue)
      {
        Serial.println("[ESPNOW] host queue create failed");
        return false;
      }
    }
  }

  WiFi.mode(gEnableAp ? WIFI_AP_STA : WIFI_STA);
  if (!gEnableAp && gEnableEspNow)
  {
    delay(10);
    if (!forceStaChannel(gApChannel))
    {
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableAp)
  {
    bool apOk = false;
    if (gApPassword[0] == '\0')
    {
      apOk = WiFi.softAP(gApSsid, nullptr, gApChannel);
    }
    else
    {
      apOk = WiFi.softAP(gApSsid, gApPassword, gApChannel);
    }
    if (!apOk)
    {
      Serial.println("[WEB] softAP start failed");
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableEspNow)
  {
    if (!initEspNowReceiver())
    {
      if (gEnableAp)
        WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  if (gEnableAp)
  {
    gWebTaskRunning = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(webServerTask, "WebPortal", kWebTaskStack, nullptr, kWebTaskPriority,
                                                  &gWebTaskHandle, kWebTaskCore);

    if (ok != pdPASS)
    {
      gWebTaskRunning = false;
      deinitEspNowReceiver();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[WEB] task create failed");
      return false;
    }
  }

  gPortalStarted = true;
  Serial.printf("[WEB] wireless started EnableAP=%d EnableESPNOW=%d SSID=%s PASS=%s AP_IP=%s CH=%d HostMAC=%s\n",
                gEnableAp ? 1 : 0, gEnableEspNow ? 1 : 0, gEnableAp ? gApSsid : "<AP-OFF>",
                gEnableAp ? (gApPassword[0] ? gApPassword : "<OPEN>") : "<AP-OFF>",
                gEnableAp ? WiFi.softAPIP().toString().c_str() : "<AP-OFF>", WiFi.channel(),
                gHostMacFilterEnabled ? gHostMacFilterText : "<ANY>");
  return true;
}

bool wirelessPortalStartEspNowOnly()
{
  if (gPortalStarted)
    return true;

  if (!gImageMutex)
  {
    gImageMutex = xSemaphoreCreateMutex();
    if (!gImageMutex)
    {
      Serial.println("[ESPNOW] image mutex create failed");
      return false;
    }
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    resetImageUploadStateLocked();
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  loadApCredentialsFromSettingIni();
  gEnableAp = false;
  gEnableEspNow = true;

  if (!gHostMessageQueue)
  {
    gHostMessageQueue = xQueueCreate(kQueueDepth, sizeof(WebQueuedMessage));
    if (!gHostMessageQueue)
    {
      Serial.println("[ESPNOW] host queue create failed");
      return false;
    }
  }

  WiFi.mode(WIFI_STA);
  delay(10);
  if (!forceStaChannel(gApChannel))
  {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  if (!initEspNowReceiver())
  {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  gPortalStarted = true;
  Serial.printf("[ESPNOW] STA-only receiver started CH=%d HostMAC=%s\n", WiFi.channel(),
                gHostMacFilterEnabled ? gHostMacFilterText : "<ANY>");
  return true;
}

void wirelessPortalStop()
{
  if (!gPortalStarted)
    return;

  gWebTaskRunning = false;
  for (int i = 0; i < 120 && gWebTaskHandle != nullptr; ++i)
  {
    delay(5);
  }

  if (gWebTaskHandle != nullptr)
  {
    vTaskDelete(gWebTaskHandle);
    gWebTaskHandle = nullptr;
  }

  if (gWebServer)
  {
    gWebServer->stop();
    delete gWebServer;
    gWebServer = nullptr;
  }

  if (gDnsServer)
  {
    gDnsServer->stop();
    delete gDnsServer;
    gDnsServer = nullptr;
  }

  deinitEspNowReceiver();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  if (gMessageQueue)
  {
    WebQueuedMessage drop = {};
    while (xQueueReceive(gMessageQueue, &drop, 0) == pdTRUE)
    {
    }
  }
  portENTER_CRITICAL(&gImmediateMessageMux);
  gImmediateMessage.text[0] = '\0';
  gImmediateMessage.pending = false;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  if (gHostMessageQueue)
  {
    WebQueuedMessage drop = {};
    while (xQueueReceive(gHostMessageQueue, &drop, 0) == pdTRUE)
    {
    }
  }

  if (gImageMutex && xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(200)) == pdTRUE)
  {
    resetImageUploadStateLocked();
    if (gPendingImage.pixels)
    {
      free(gPendingImage.pixels);
    }
    gPendingImage.pixels = nullptr;
    gPendingImage.pixelCapacity = 0;
    gPendingImage.pixelCount = 0;
    gPendingImage.width = 0;
    gPendingImage.height = 0;
    gPendingImage.centerX = kImageDefaultCenterX;
    gPendingImage.centerY = kImageDefaultCenterY;
    gPendingImage.ready = false;
    xSemaphoreGive(gImageMutex);
  }

  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = false;
  gScheduleReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);

  gPortalStarted = false;
  Serial.println("[WEB] AP stopped");
}

bool wirelessPortalPopMessage(String &outMessage)
{
  outMessage = "";
  if (!gMessageQueue)
    return false;

  WebQueuedMessage msg = {};
  if (xQueueReceive(gMessageQueue, &msg, 0) != pdTRUE)
    return false;

  outMessage = msg.text;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingMessage()
{
  if (!gMessageQueue)
    return false;
  return uxQueueMessagesWaiting(gMessageQueue) > 0;
}

bool wirelessPortalPopImmediateMessage(String &outMessage)
{
  outMessage = "";
  char local[kTextMaxLen + 1] = {0};
  bool has = false;
  portENTER_CRITICAL(&gImmediateMessageMux);
  if (gImmediateMessage.pending)
  {
    memcpy(local, gImmediateMessage.text, sizeof(local));
    local[sizeof(local) - 1] = '\0';
    gImmediateMessage.pending = false;
    has = true;
  }
  portEXIT_CRITICAL(&gImmediateMessageMux);
  if (!has)
    return false;

  outMessage = local;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingImmediateMessage()
{
  return immediatePendingCount() > 0;
}

bool wirelessPortalPopHostMessage(String &outMessage)
{
  outMessage = "";
  if (!gHostMessageQueue)
    return false;

  WebQueuedMessage msg = {};
  if (xQueueReceive(gHostMessageQueue, &msg, 0) != pdTRUE)
    return false;

  outMessage = msg.text;
  return outMessage.length() > 0;
}

bool wirelessPortalHasPendingHostMessage()
{
  if (!gHostMessageQueue)
    return false;
  return uxQueueMessagesWaiting(gHostMessageQueue) > 0;
}

bool wirelessPortalPushMessageForRestore(const String &text)
{
  String error;
  return enqueueRegularMessage(text, error);
}

bool wirelessPortalPushImmediateMessageForRestore(const String &text)
{
  String error;
  return enqueueImmediateMessage(text, error);
}

bool wirelessPortalPushHostMessageForRestore(const String &text)
{
  if (!gHostMessageQueue)
    return false;

  String normalized = text;
  normalized.trim();
  if (!normalized.length())
    return false;
  if (normalized.length() > kTextMaxLen)
  {
    normalized.remove(kTextMaxLen);
  }

  WebQueuedMessage msg = {};
  normalized.toCharArray(msg.text, sizeof(msg.text));
  return xQueueSend(gHostMessageQueue, &msg, 0) == pdTRUE;
}

bool wirelessPortalConsumeCsvReloadRequest()
{
  return takeCsvReloadRequested();
}

bool wirelessPortalConsumeScheduleReloadRequest()
{
  return takeScheduleReloadRequested();
}

bool wirelessPortalInstantRefreshNoKeyEnabled()
{
  return gInstantRefreshNoKey;
}

bool wirelessPortalTakePendingImage(uint16_t *outPixels, size_t outCapacityPixels, uint16_t &outWidth,
                                    uint16_t &outHeight, int16_t &outCenterX, int16_t &outCenterY,
                                    size_t &outPixelCount)
{
  outWidth = 0;
  outHeight = 0;
  outCenterX = kImageDefaultCenterX;
  outCenterY = kImageDefaultCenterY;
  outPixelCount = 0;

  if (!outPixels || outCapacityPixels == 0 || !gImageMutex)
    return false;
  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return false;

  if (!gPendingImage.ready || !gPendingImage.pixels || gPendingImage.pixelCount == 0)
  {
    xSemaphoreGive(gImageMutex);
    return false;
  }
  if (gPendingImage.pixelCount > outCapacityPixels)
  {
    xSemaphoreGive(gImageMutex);
    return false;
  }

  memcpy(outPixels, gPendingImage.pixels, gPendingImage.pixelCount * sizeof(uint16_t));
  outWidth = gPendingImage.width;
  outHeight = gPendingImage.height;
  outCenterX = gPendingImage.centerX;
  outCenterY = gPendingImage.centerY;
  outPixelCount = gPendingImage.pixelCount;
  gPendingImage.ready = false;

  xSemaphoreGive(gImageMutex);
  return true;
}
