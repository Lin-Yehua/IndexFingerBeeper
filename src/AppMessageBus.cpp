#include "AppMessageBus.h"

#include <string.h>

AppMessageBus gAppMessageBus;

bool AppMessageBus::begin(size_t depth)
{
  if (ready())
  {
    return true;
  }
  if (depth == 0)
  {
    depth = kAppMessageDefaultQueueDepth;
  }

  for (size_t i = 0; i < kPriorityCount; ++i)
  {
    _queues[i] = xQueueCreate(depth, sizeof(AppMessage));
    if (!_queues[i])
    {
      Serial.printf("[APPBUS] queue create failed index=%u\n", static_cast<unsigned int>(i));
      return false;
    }
  }

  Serial.printf("[APPBUS] ready priorities=%u depth=%u item=%u\n", static_cast<unsigned int>(kPriorityCount),
                static_cast<unsigned int>(depth), static_cast<unsigned int>(sizeof(AppMessage)));
  return true;
}

size_t AppMessageBus::priorityIndex(AppMsgPriority priority)
{
  const size_t raw = static_cast<size_t>(priority);
  if (raw >= kPriorityCount)
  {
    return static_cast<size_t>(AppMsgPriority::Normal);
  }
  return raw;
}

bool AppMessageBus::publish(const AppMessage &msg, uint32_t timeoutMs)
{
  if (!ready() && !begin())
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
  QueueHandle_t queue = _queues[priorityIndex(copy.priority)];
  return queue && xQueueSend(queue, &copy, waitTicks) == pdTRUE;
}

bool AppMessageBus::poll(AppMessage &outMsg, uint32_t timeoutMs)
{
  if (!ready())
  {
    return false;
  }
  const TickType_t waitTicks = timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs);

  for (int index = static_cast<int>(kPriorityCount) - 1; index >= 0; --index)
  {
    const TickType_t wait = (index == 0) ? waitTicks : 0;
    QueueHandle_t queue = _queues[index];
    if (queue && xQueueReceive(queue, &outMsg, wait) == pdTRUE)
    {
      return true;
    }
  }
  return false;
}

size_t AppMessageBus::pending() const
{
  if (!ready())
  {
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < kPriorityCount; ++i)
  {
    if (_queues[i])
    {
      total += static_cast<size_t>(uxQueueMessagesWaiting(_queues[i]));
    }
  }
  return total;
}

bool AppMessageBus::ready() const
{
  for (size_t i = 0; i < kPriorityCount; ++i)
  {
    if (!_queues[i])
    {
      return false;
    }
  }
  return true;
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
