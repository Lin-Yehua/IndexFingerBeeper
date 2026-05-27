#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

constexpr size_t kAppMessageTextMaxLen = 240;
constexpr size_t kAppMessageDefaultQueueDepth = 32;

enum class AppMsgType : uint8_t
{
  KeyPressed = 0,
  UsbPlugged,
  UsbUnplugged,

  TextPushReceived,
  ImmediateTextReceived,
  ImageReceived,

  WirelessSettingsChanged,
  WifiConnectRequested,
  WifiConnected,
  WifiDisconnected,

  ScheduleDue,
  BatteryLow,

  MenuOpenRequested,
  FeatureSelected,
  FeatureFinished,

  UiNavigate,
  UiBack,

  PlayText,
  PlayMusic,
  ShowImage,
  EnterSleep,
};

enum class AppMsgPriority : uint8_t
{
  Low = 0,
  Normal,
  High,
  Interrupt,
  Critical,
};

enum class AppMsgSource : uint8_t
{
  Unknown = 0,
  Key,
  WirelessPortal,
  StaCloud,
  Schedule,
  Battery,
  Usb,
  Ui,
  Feature,
};

struct AppMessage
{
  AppMsgType type = AppMsgType::KeyPressed;
  AppMsgPriority priority = AppMsgPriority::Normal;
  AppMsgSource source = AppMsgSource::Unknown;
  uint32_t timestampMs = 0;
  uint16_t correlationId = 0;
  uint16_t targetFeature = 0;

  union
  {
    uint8_t keyCode;
    uint16_t routeId;
    uint16_t imageId;
    uint16_t settingId;
    uint16_t commandId;
    struct
    {
      uint16_t width;
      uint16_t height;
      int16_t centerX;
      int16_t centerY;
    } image;
  } data = {};

  char text[kAppMessageTextMaxLen + 1] = {0};
};

class AppMessageBus
{
public:
  bool begin(size_t depth = kAppMessageDefaultQueueDepth);
  bool publish(const AppMessage &msg, uint32_t timeoutMs = 0);
  bool poll(AppMessage &outMsg, uint32_t timeoutMs = 0);
  size_t pending() const;
  bool ready() const;

private:
  QueueHandle_t _queue = nullptr;
};

extern AppMessageBus gAppMessageBus;

void appMessageCopyText(AppMessage &msg, const char *text);
const char *appMessageTypeName(AppMsgType type);
