#include "AppMessageBus.h"

#include <string.h>

AppMessageBus gAppMessageBus;

bool AppMessageBus::begin(size_t depth)
{
  if (_queue)
  {
    return true;
  }
  if (depth == 0)
  {
    depth = kAppMessageDefaultQueueDepth;
  }
  _queue = xQueueCreate(depth, sizeof(AppMessage));
  if (!_queue)
  {
    Serial.println("[APPBUS] queue create failed");
    return false;
  }
  Serial.printf("[APPBUS] ready depth=%u item=%u\n", static_cast<unsigned int>(depth),
                static_cast<unsigned int>(sizeof(AppMessage)));
  return true;
}

bool AppMessageBus::publish(const AppMessage &msg, uint32_t timeoutMs)
{
  if (!_queue && !begin())
  {
    return false;
  }

  AppMessage copy = msg;
  if (copy.timestampMs == 0)
  {
    copy.timestampMs = millis();
  }
  copy.text[kAppMessageTextMaxLen] = '\0';

  const TickType_t waitTicks = timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xQueueSend(_queue, &copy, waitTicks) == pdTRUE;
}

bool AppMessageBus::poll(AppMessage &outMsg, uint32_t timeoutMs)
{
  if (!_queue)
  {
    return false;
  }
  const TickType_t waitTicks = timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xQueueReceive(_queue, &outMsg, waitTicks) == pdTRUE;
}

size_t AppMessageBus::pending() const
{
  if (!_queue)
  {
    return 0;
  }
  return static_cast<size_t>(uxQueueMessagesWaiting(_queue));
}

bool AppMessageBus::ready() const
{
  return _queue != nullptr;
}

void appMessageCopyText(AppMessage &msg, const char *text)
{
  if (!text)
  {
    msg.text[0] = '\0';
    return;
  }
  strncpy(msg.text, text, kAppMessageTextMaxLen);
  msg.text[kAppMessageTextMaxLen] = '\0';
}

const char *appMessageTypeName(AppMsgType type)
{
  switch (type)
  {
  case AppMsgType::KeyPressed:
    return "KeyPressed";
  case AppMsgType::UsbPlugged:
    return "UsbPlugged";
  case AppMsgType::UsbUnplugged:
    return "UsbUnplugged";
  case AppMsgType::TextPushReceived:
    return "TextPushReceived";
  case AppMsgType::ImmediateTextReceived:
    return "ImmediateTextReceived";
  case AppMsgType::ImageReceived:
    return "ImageReceived";
  case AppMsgType::WirelessSettingsChanged:
    return "WirelessSettingsChanged";
  case AppMsgType::WifiConnectRequested:
    return "WifiConnectRequested";
  case AppMsgType::WifiConnected:
    return "WifiConnected";
  case AppMsgType::WifiDisconnected:
    return "WifiDisconnected";
  case AppMsgType::ScheduleDue:
    return "ScheduleDue";
  case AppMsgType::BatteryLow:
    return "BatteryLow";
  case AppMsgType::MenuOpenRequested:
    return "MenuOpenRequested";
  case AppMsgType::FeatureSelected:
    return "FeatureSelected";
  case AppMsgType::FeatureFinished:
    return "FeatureFinished";
  case AppMsgType::UiNavigate:
    return "UiNavigate";
  case AppMsgType::UiBack:
    return "UiBack";
  case AppMsgType::PlayText:
    return "PlayText";
  case AppMsgType::PlayMusic:
    return "PlayMusic";
  case AppMsgType::ShowImage:
    return "ShowImage";
  case AppMsgType::EnterSleep:
    return "EnterSleep";
  }
  return "Unknown";
}
