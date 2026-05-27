/* Interrupt controller for Host/Web/Schedule preemption. Included by UsbAppMode.cpp. */
class AppInterruptController
{
public:
  void syncMode();
  bool service();
  bool takeSyntheticKeyPress();

private:
  LoopIrqState _irq;
  bool _syntheticKeyPress = false;

  bool handleScheduleIrq();
  bool handleHostIrq();
  bool handleWebIrq();
};

bool AppInterruptController::handleScheduleIrq()
{
  if (gScheduleInterruptPendingStart && gScheduleInterruptCount > 0 && inboxConsumeScheduleDue())
  {
    preemptByHost(_irq);
    gScheduleInterruptPendingStart = false;
    gScheduleInterruptActive = true;
    gScheduleInterruptKeyLatch = false;
    gScheduleInterruptQueue[0].reminderCount = 0;
    playMessageWithGlitch(gScheduleInterruptQueue[0].text);
    gScheduleInterruptLastPlayMs = millis();
    Serial.println("[SCHEDULE] interrupt queue started");
    return true;
  }

  if (!(gScheduleInterruptActive && gScheduleInterruptCount > 0))
    return false;

  String hostBroadcastMessage;
  if (wirelessPortalPopHostMessage(hostBroadcastMessage))
  {
    if (insertHostMessageIntoScheduleInterruptQueueFront(hostBroadcastMessage))
    {
      Serial.println("[SCHEDULE] host message inserted at queue front");
    }
    else
    {
      Serial.println("[SCHEDULE] host message dropped (queue full)");
    }
  }

  const uint8_t key = readLoopKey(_syntheticKeyPress);
  if (key == 2 && !gScheduleInterruptKeyLatch)
  {
    gScheduleInterruptKeyLatch = true;
    if (wakeBacklightByKeyIfNeeded())
    {
      return true;
    }

    if (gScheduleInterruptCount > 1)
    {
      for (size_t i = 1; i < gScheduleInterruptCount; ++i)
      {
        gScheduleInterruptQueue[i - 1] = gScheduleInterruptQueue[i];
      }
      --gScheduleInterruptCount;
      gScheduleInterruptQueue[0].reminderCount = 0;
      playMessageWithGlitch(gScheduleInterruptQueue[0].text);
      gScheduleInterruptLastPlayMs = millis();
      Serial.println("[SCHEDULE] interrupt next message");
      return true;
    }

    clearScheduleInterruptQueue();
    _syntheticKeyPress = true;
    Serial.println("[SCHEDULE] interrupt queue finished");
  }
  if (key != 2)
  {
    gScheduleInterruptKeyLatch = false;
  }

  if (!(gScheduleInterruptActive && gScheduleInterruptCount > 0))
    return false;

  ScheduleInterruptQueueItem &current = gScheduleInterruptQueue[0];
  if (!current.hostMessage && current.intervalSec > 0 && current.reminderTimes > current.reminderCount)
  {
    const uint32_t intervalMs = static_cast<uint32_t>(current.intervalSec) * 1000UL;
    if (intervalMs > 0 && (millis() - gScheduleInterruptLastPlayMs) >= intervalMs)
    {
      ++current.reminderCount;
      playMessageWithGlitch(current.text);
      gScheduleInterruptLastPlayMs = millis();
      Serial.printf("[SCHEDULE] reminder replay %u/%u\n", static_cast<unsigned int>(current.reminderCount),
                    static_cast<unsigned int>(current.reminderTimes));
      return true;
    }
  }
  return true;
}

// 处理 Host 广播和日程提醒中断。
bool AppInterruptController::handleHostIrq()
{
  if (!hostIrqAllowed())
    return false;

  if (handleScheduleIrq())
    return true;

  String hostBroadcastMessage;
  if (wirelessPortalPopHostMessage(hostBroadcastMessage))
  {
    preemptByHost(_irq);
    playMessageWithGlitch(hostBroadcastMessage.c_str());
    return true;
  }
  return false;
}

// 处理网页普通、立即和图片三类中断。
bool AppInterruptController::handleWebIrq()
{
  if (!webIrqAllowed())
    return false;

  String immediateMessage;
  if (inboxPopImmediateText(immediateMessage))
  {
    startImmediate(_irq);
    playMessageWithGlitch(immediateMessage.c_str());
    _irq.immediateActive = false;
    _irq.immediateKeyLatch = false;
    _irq.immediatePreemptedWeb = false;
    return true;
  }

  // Image interrupt and immediate interrupt are peer-level:
  // image polling must happen before "immediate active" wait branch.
  AppLoopImageEvent image;
  if (inboxPopImage(image))
  {
    startImage(_irq, image.width, image.height, image.centerX, image.centerY);
    return true;
  }

  if (_irq.immediateActive)
  {
    const uint8_t key = readLoopKey(_syntheticKeyPress);
    if (key == 2 && !_irq.immediateKeyLatch)
    {
      _irq.immediateKeyLatch = true;
      if (wakeBacklightByKeyIfNeeded())
      {
        return true;
      }
      finishImmediate(_irq, _syntheticKeyPress);
    }
    if (key != 2)
    {
      _irq.immediateKeyLatch = false;
    }
    if (_irq.immediateActive)
    {
      return true;
    }
  }

  if (_irq.imageActive)
  {
    const uint8_t key = readLoopKey(_syntheticKeyPress);
    if (key == 2 && !_irq.imageKeyLatch)
    {
      _irq.imageKeyLatch = true;
      if (wakeBacklightByKeyIfNeeded())
      {
        // Backlight wake is always effective and does not end image interrupt.
        return true;
      }
      finishImage(_irq, _syntheticKeyPress);
    }
    else
    {
      if (key != 2)
      {
        _irq.imageKeyLatch = false;
      }
      return true;
    }
  }

  if (!_irq.webActive && inboxHasWebText())
  {
    _irq.webActive = true;
    _irq.webKeyLatch = false;
    Serial.println("[WEB] interrupt started");
  }

  if (_irq.webActive)
  {
    String queuedMessage;
    if (inboxPopWebText(queuedMessage))
    {
      playMessageWithGlitch(queuedMessage.c_str());
      Serial.println(inboxHasWebText() ? "[WEB] interrupt next message" : "[WEB] interrupt displayed");
      return true;
    }
    _irq.webActive = false;
    _irq.webKeyLatch = false;
    _syntheticKeyPress = true;
    Serial.println("[WEB] interrupt finished");
    return true;
  }
  return false;
}

// 正常 AP 模式：按键切联网，短按播放本地 CSV/恢复提示。
void AppInterruptController::syncMode()
{
  resetIrqOnMode(_irq);
}

bool AppInterruptController::service()
{
  if (handleHostIrq())
    return true;
  if (handleWebIrq())
    return true;
  return false;
}

bool AppInterruptController::takeSyntheticKeyPress()
{
  const bool value = _syntheticKeyPress;
  _syntheticKeyPress = false;
  return value;
}

static AppInterruptController gAppInterruptController;

// STA 连接成功后的断线复位和按键取云端队列消息。
