/*
 * 文件说明: APP 主循环控制器私有实现。
 * 文件功能: 拆分主循环中的中断仲裁、网页中断、Host/日程中断和各 APP 模式运行路径。
 *
 * 函数表:
 * - readLoopKey: 读取、获取或消费对应数据。
 * - wakeByKey: 模块内部辅助函数。
 * - csvTotal: 模块内部辅助函数。
 * - startCsvRound: 模块内部辅助函数。
 * - playCsvAtCursor: 播放对应消息或音频流程。
 * - playLocalText: 播放对应消息或音频流程。
 * - consumePlayTrigger: 模块内部辅助函数。
 * - resetIrqOnMode: 停止、释放或清理对应状态。
 * - preemptByHost: 模块内部辅助函数。
 * - startImmediate: 模块内部辅助函数。
 * - startImage: 模块内部辅助函数。
 * - finishImmediate: 模块内部辅助函数。
 * - finishImage: 模块内部辅助函数。
 * - ensureImageBuf: 初始化或确保对应资源可用。
 * - reloadCsvIfNeeded: 模块内部辅助函数。
 * - hostIrqAllowed: 模块内部辅助函数。
 * - webIrqAllowed: 模块内部辅助函数。
 * - handleScheduleIrq: 处理对应业务流程或事件分支。
 * - handleHostIrq: 处理对应业务流程或事件分支。
 * - handleWebIrq: 处理对应业务流程或事件分支。
 * - handleApMode: 处理对应业务流程或事件分支。
 * - handleStaConnected: 处理对应业务流程或事件分支。
 * - handleStaConnecting: 处理对应业务流程或事件分支。
 * - handleStaMode: 处理对应业务流程或事件分支。
 * - handleStaOnlyMode: 处理对应业务流程或事件分支。
 * - handleCurrentMode: 处理对应业务流程或事件分支。
 * - processAppLoop: 执行 APP 主循环调度。
 */
struct LoopIrqState
{
  bool webActive = false;
  bool webKeyLatch = false;
  bool imageActive = false;
  bool imageKeyLatch = false;
  bool imagePreemptedWeb = false;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
  int16_t imageCenterX = 160;
  int16_t imageCenterY = 155;
  bool immediateActive = false;
  bool immediateKeyLatch = false;
  bool immediatePreemptedWeb = false;
};

// 读取主循环按键；必要时消费上层中断传递下来的虚拟短按。
static uint8_t readLoopKey(bool &syntheticKeyPress)
{
  if (syntheticKeyPress)
  {
    syntheticKeyPress = false;
    return 2;
  }

  Key_loop();
  return get_Keycode();
}

// 短按/长按先尝试唤醒背光，避免误触发业务动作。
static bool wakeByKey(uint8_t key)
{
  return (key == 2 || key == 3) && wakeBacklightByKeyIfNeeded();
}

// 返回本轮可访问的 CSV 数量，并限制到缓存容量内。
static int csvTotal()
{
  int total = csv.size();
  if (total > kCsvArrayCapacity)
  {
    total = kCsvArrayCapacity;
  }
  return total;
}

// 建立一轮不重复随机 CSV 序列。
static void startCsvRound(int total)
{
  if (RUNSTATE != 0)
    return;

  generateUniqueRandomNumbers(1, csv.size(), total, csvArray);
  csvCount = 0;
  RUNSTATE = 1;
}

// 播放当前 CSV 游标指向的文本，游标耗尽时自动开新一轮。
static void playCsvAtCursor(int total)
{
  if (csvCount >= total)
  {
    generateUniqueRandomNumbers(1, csv.size(), total, csvArray);
    csvCount = 0;
    RUNSTATE = 1;
  }

  const int currentCsvId = csvArray[csvCount];
  csvCount++;

  String localMessage;
  const char *csvMessage = csv.getTextById(currentCsvId);
  if (csvMessage)
  {
    localMessage = csvMessage;
  }
  else
  {
    localMessage = "CSV id not found: ";
    localMessage += String(currentCsvId);
  }
  message = localMessage.c_str();
  playMessageWithGlitch(message);
}

// 根据 CSV 状态播放本地文本；空表时使用固定兜底文案。
static void playLocalText(int total)
{
  if (total <= 0)
  {
    message = kCsvEmptyFallbackMessage;
    playMessageWithGlitch(message);
    return;
  }

  startCsvRound(total);
  playCsvAtCursor(total);
}

// 判断本轮是否由短按或启动首屏触发播放。
static bool consumePlayTrigger(uint8_t key)
{
  if (!(key == 2 || firstFlag))
    return false;

  if (firstFlag)
  {
    firstFlag = false;
  }
  return true;
}

// 模式切换时清空中断态，避免旧模式的网页/图片中断串到新模式。
static void resetIrqOnMode(LoopIrqState &irq)
{
  static AppLoopMode lastLoopMode = APP_MODE_AP_STA;
  static bool lastLoopModeInitialized = false;
  if (!lastLoopModeInitialized)
  {
    lastLoopMode = gAppLoopMode;
    lastLoopModeInitialized = true;
    return;
  }

  if (lastLoopMode != gAppLoopMode)
  {
    irq = LoopIrqState{};
    clearScheduleInterruptQueue();
    lastLoopMode = gAppLoopMode;
  }
}

// Host/日程中断进入时，打断较低优先级的网页类中断。
static void preemptByHost(LoopIrqState &irq)
{
  if (irq.imageActive)
  {
    irq.imageActive = false;
    irq.imageKeyLatch = false;
    if (irq.imagePreemptedWeb)
    {
      irq.webActive = true;
      irq.webKeyLatch = false;
    }
    irq.imagePreemptedWeb = false;
    tft.fillScreen(0x0000);
    Serial.println("[WEB] image interrupt preempted by host");
  }
  if (irq.immediateActive)
  {
    irq.immediateKeyLatch = false;
    Serial.println("[ESPNOW] preempt immediate interrupt");
  }
  if (irq.webActive)
  {
    Serial.println("[ESPNOW] preempt web interrupt");
  }
}

// 启动网页立即消息中断，必要时保留可恢复的普通网页中断。
static void startImmediate(LoopIrqState &irq)
{
  if (!irq.immediateActive)
  {
    const bool hadImageInterrupt = irq.imageActive;
    const bool hadImagePreemptedWebInterrupt = irq.imagePreemptedWeb;
    const bool hadWebInterrupt = irq.webActive;

    if (hadImageInterrupt)
    {
      irq.imageActive = false;
      irq.imageKeyLatch = false;
      irq.imagePreemptedWeb = false;
      tft.fillScreen(0x0000);
      Serial.println("[WEB] image interrupt preempted by immediate");
    }
    if (hadWebInterrupt)
    {
      irq.webActive = false;
      irq.webKeyLatch = false;
      Serial.println("[WEB] web interrupt preempted by immediate");
    }

    irq.immediateActive = true;
    irq.immediateKeyLatch = false;
    // Image and immediate are same-layer interrupts: do not preserve each other.
    // But keep lower-layer web resume chain when either one had preempted web.
    irq.immediatePreemptedWeb = hadWebInterrupt || hadImagePreemptedWebInterrupt;
    Serial.println("[WEB] immediate interrupt started");
  }
  else
  {
    irq.immediateKeyLatch = false;
    Serial.println("[WEB] immediate interrupt updated");
  }
}

// 启动网页图片中断，并记录被抢占的普通网页中断状态。
static void startImage(LoopIrqState &irq, uint16_t imageW, uint16_t imageH, int16_t centerX, int16_t centerY)
{
  notifyBacklightActivity();
  const bool hadImmediateInterrupt = irq.immediateActive;
  const bool hadImmediatePreemptedWeb = irq.immediatePreemptedWeb;
  const bool hadWebInterrupt = irq.webActive;
  const bool keepPreemptedWeb = irq.imageActive && irq.imagePreemptedWeb;

  if (hadImmediateInterrupt)
  {
    irq.immediateActive = false;
    irq.immediateKeyLatch = false;
    Serial.println("[WEB] immediate interrupt preempted by image");
  }
  if (hadWebInterrupt)
  {
    irq.webActive = false;
    irq.webKeyLatch = false;
  }

  showWebInterruptImage(gWebImageScratch, imageW, imageH, centerX, centerY);
  irq.imageActive = true;
  irq.imageKeyLatch = false;
  // Image and immediate are same-layer interrupts: do not preserve each other.
  irq.imagePreemptedWeb = hadWebInterrupt || hadImmediatePreemptedWeb || keepPreemptedWeb;
  irq.imageWidth = imageW;
  irq.imageHeight = imageH;
  irq.imageCenterX = centerX;
  irq.imageCenterY = centerY;
  Serial.printf("[WEB] image interrupt started %ux%u\n", static_cast<unsigned int>(imageW),
                static_cast<unsigned int>(imageH));
}

// 结束立即消息中断，并把同一次短按传递给后续业务流。
static void finishImmediate(LoopIrqState &irq, bool &syntheticKeyPress)
{
  irq.immediateActive = false;
  irq.immediateKeyLatch = false;
  const bool canResumeWeb = irq.immediatePreemptedWeb && wirelessPortalHasPendingMessage();
  if (canResumeWeb)
  {
    irq.webActive = true;
    irq.webKeyLatch = false;
    Serial.println("[WEB] immediate interrupt resume web");
  }
  else
  {
    if (irq.immediatePreemptedWeb)
    {
      irq.webActive = false;
      irq.webKeyLatch = false;
      Serial.println("[WEB] immediate interrupt finished (no pending web message)");
    }
    else
    {
      Serial.println("[WEB] immediate interrupt finished");
    }
  }
  syntheticKeyPress = true;
  irq.immediatePreemptedWeb = false;
}

// 结束图片中断，必要时恢复普通网页中断。
static void finishImage(LoopIrqState &irq, bool &syntheticKeyPress)
{
  irq.imageActive = false;
  irq.imageKeyLatch = false;
  if (irq.imagePreemptedWeb)
  {
    irq.webActive = true;
    irq.webKeyLatch = false;
  }
  irq.imagePreemptedWeb = false;
  syntheticKeyPress = true;
  tft.fillScreen(0x0000);
  Serial.println("[WEB] image interrupt finished");
}

// 确保网页图片中断使用的像素缓存已经分配。
static void ensureImageBuf()
{
  const size_t wantedPixels = static_cast<size_t>(kWebImageMaxWidth) * static_cast<size_t>(kWebImageMaxHeight);
  if (gWebImageScratch)
    return;

  if (!ensureWebImageScratch(wantedPixels))
  {
    static uint32_t lastAllocLogMs = 0;
    const uint32_t now = millis();
    if (now - lastAllocLogMs > 5000U)
    {
      lastAllocLogMs = now;
      Serial.println("[WEB] image scratch alloc failed");
    }
  }
}

// 处理网页保存 data.csv 后的热重载请求。
static void reloadCsvIfNeeded(bool instantRefreshNoKey)
{
  if (!wirelessPortalConsumeCsvReloadRequest())
    return;

  notifyBacklightActivity();
  if (csv.load(FFat, "/data.csv"))
  {
    csvCount = 0;
    RUNSTATE = 0;
    if (csv.size() <= 0)
    {
      Serial.println("[WEB] /data.csv is empty, fallback message enabled");
    }
    if (instantRefreshNoKey)
    {
      firstFlag = false;
      playLocalText(csvTotal());
    }
    Serial.println("[WEB] /data.csv reloaded");
  }
  else
  {
    Serial.println("[WEB] /data.csv reload failed");
  }
}

// 当前模式是否允许 Host/日程类高优先级中断。
static bool hostIrqAllowed()
{
  return (gAppLoopMode == APP_MODE_AP_STA) || (gAppLoopMode == APP_MODE_STA_ONLY) ||
         (gAppLoopMode == APP_MODE_STA_ONLINE && gStaOnlinePhase == StaOnlinePhase::kConnected);
}

// 当前模式是否允许网页普通/立即/图片中断。
static bool webIrqAllowed()
{
  return (gAppLoopMode == APP_MODE_AP_STA) ||
         (gAppLoopMode == APP_MODE_STA_ONLINE && gStaOnlinePhase == StaOnlinePhase::kConnected);
}

// 维护日程中断队列，包括插队、按键切换和重复提醒。
static bool handleScheduleIrq(LoopIrqState &irq, bool &syntheticKeyPress)
{
  if (gScheduleInterruptPendingStart && gScheduleInterruptCount > 0)
  {
    preemptByHost(irq);
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

  Key_loop();
  const uint8_t key = get_Keycode();
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
    syntheticKeyPress = true;
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
static bool handleHostIrq(LoopIrqState &irq, bool &syntheticKeyPress)
{
  if (!hostIrqAllowed())
    return false;

  if (handleScheduleIrq(irq, syntheticKeyPress))
    return true;

  String hostBroadcastMessage;
  if (wirelessPortalPopHostMessage(hostBroadcastMessage))
  {
    preemptByHost(irq);
    playMessageWithGlitch(hostBroadcastMessage.c_str());
    return true;
  }
  return false;
}

// 处理网页普通、立即和图片三类中断。
static bool handleWebIrq(LoopIrqState &irq, bool &syntheticKeyPress)
{
  if (!webIrqAllowed())
    return false;

  String immediateMessage;
  if (wirelessPortalPopImmediateMessage(immediateMessage))
  {
    startImmediate(irq);
    playMessageWithGlitch(immediateMessage.c_str());
    irq.immediateActive = false;
    irq.immediateKeyLatch = false;
    irq.immediatePreemptedWeb = false;
    return true;
  }

  // Image interrupt and immediate interrupt are peer-level:
  // image polling must happen before "immediate active" wait branch.
  if (gWebImageScratch)
  {
    uint16_t imageW = 0;
    uint16_t imageH = 0;
    int16_t centerX = 160;
    int16_t centerY = 155;
    size_t pixelCount = 0;
    if (wirelessPortalTakePendingImage(gWebImageScratch, gWebImageScratchPixels, imageW, imageH, centerX, centerY,
                                       pixelCount))
    {
      (void)pixelCount;
      startImage(irq, imageW, imageH, centerX, centerY);
      return true;
    }
  }

  if (irq.immediateActive)
  {
    Key_loop();
    const uint8_t key = get_Keycode();
    if (key == 2 && !irq.immediateKeyLatch)
    {
      irq.immediateKeyLatch = true;
      if (wakeBacklightByKeyIfNeeded())
      {
        return true;
      }
      finishImmediate(irq, syntheticKeyPress);
    }
    if (key != 2)
    {
      irq.immediateKeyLatch = false;
    }
    if (irq.immediateActive)
    {
      return true;
    }
  }

  if (irq.imageActive)
  {
    Key_loop();
    const uint8_t key = get_Keycode();
    if (key == 2 && !irq.imageKeyLatch)
    {
      irq.imageKeyLatch = true;
      if (wakeBacklightByKeyIfNeeded())
      {
        // Backlight wake is always effective and does not end image interrupt.
        return true;
      }
      finishImage(irq, syntheticKeyPress);
    }
    else
    {
      if (key != 2)
      {
        irq.imageKeyLatch = false;
      }
      return true;
    }
  }

  if (!irq.webActive && wirelessPortalHasPendingMessage())
  {
    irq.webActive = true;
    irq.webKeyLatch = false;
    Serial.println("[WEB] interrupt started");
  }

  if (irq.webActive)
  {
    String queuedMessage;
    if (wirelessPortalPopMessage(queuedMessage))
    {
      playMessageWithGlitch(queuedMessage.c_str());
      Serial.println(wirelessPortalHasPendingMessage() ? "[WEB] interrupt next message" : "[WEB] interrupt displayed");
      return true;
    }
    irq.webActive = false;
    irq.webKeyLatch = false;
    syntheticKeyPress = true;
    Serial.println("[WEB] interrupt finished");
    return true;
  }
  return false;
}

// 正常 AP 模式：按键切联网，短按播放本地 CSV/恢复提示。
static bool handleApMode(bool &syntheticKeyPress)
{
  const int total = csvTotal();
  if (total > 0)
  {
    startCsvRound(total);
  }

  const uint8_t key = readLoopKey(syntheticKeyPress);
  if (wakeByKey(key))
    return true;

  if (key == 3)
  {
    switchAppMode(APP_MODE_STA_ONLINE);
    return true;
  }
  if (playPostRecoveryManualMessageByKey(key))
    return true;
  if (hasPostRecoveryManualMessage())
    return true;

  if (consumePlayTrigger(key))
  {
    playLocalText(total);
  }
  return true;
}

// STA 连接成功后的断线复位和按键取云端队列消息。
static bool handleStaConnected(uint8_t key)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    gStaOnlinePhase = StaOnlinePhase::kDisconnectedWaitShort;
    gStaRetryCount = 0;
    gStaAttemptStartMs = 0;
    gStaLastQueueEmptyHintMs = 0;
    gStaNextFetchAllowedMs = 0;
    gStaNtpSyncedThisSession = false;
    gStaNtpLastAttemptMs = 0;
    clearStaMessageQueue();
    playStaMessage(kStaDisconnectedMsg);
    return true;
  }

  if (key == 2)
  {
    String nextMessage;
    if (!popStaMessageQueue(nextMessage))
    {
      const uint32_t nowMs = millis();
      if ((nowMs - gStaLastQueueEmptyHintMs) < kStaQueueEmptyHintCooldownMs)
      {
        return true;
      }
      gStaLastQueueEmptyHintMs = nowMs;
      playStaMessage(kStaCloudQueueEmptyMsg);
      return true;
    }
    gStaLastQueueEmptyHintMs = 0;
    playStaMessage(nextMessage);
  }
  return true;
}

// STA 连接中阶段：成功后校时，超时则按次数重试。
static bool handleStaConnecting()
{
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED)
  {
    gStaOnlinePhase = StaOnlinePhase::kConnected;
    clearStaMessageQueue();
    gStaLastQueueEmptyHintMs = 0;
    gStaNextFetchAllowedMs = 0;
    String okMsg = kStaConnectOkPrefix;
    okMsg += WiFi.localIP().toString();
    String ntpDetail;
    if (syncDs1302FromStaNtp(ntpDetail))
    {
      Serial.printf("[STA] NTP sync -> DS1302 ok: %s\n", ntpDetail.c_str());
    }
    else
    {
      Serial.printf("[STA] NTP sync skipped/failed: %s\n", ntpDetail.c_str());
    }
    playStaMessage(okMsg);
    return true;
  }

  const uint32_t elapsed = millis() - gStaAttemptStartMs;
  if (elapsed < kStaAttemptTimeoutMs)
  {
    return true;
  }

  gStaRetryCount++;
  if (gStaRetryCount >= kStaMaxRetryCount)
  {
    gStaOnlinePhase = StaOnlinePhase::kFailWaitShort;
    playStaMessage(kStaConnectFailMsg);
    return true;
  }

  beginStaConnectAttempt();
  return true;
}

// 联网模式：短按连接/取云端消息，长按切到省电模式。
static bool handleStaMode(bool &syntheticKeyPress)
{
  const uint8_t key = readLoopKey(syntheticKeyPress);
  if (wakeByKey(key))
    return true;

  // Long press always switches mode, including while connecting.
  if (key == 3)
  {
    switchAppMode(APP_MODE_STA_ONLY);
    return true;
  }

  switch (gStaOnlinePhase)
  {
  case StaOnlinePhase::kPromptWaitShort:
    if (key == 2)
    {
      beginStaConnectAttempt();
    }
    return true;

  case StaOnlinePhase::kConnecting:
    return handleStaConnecting();

  case StaOnlinePhase::kFailWaitShort:
    if (key == 2)
    {
      switchAppMode(APP_MODE_STA_ONLY);
    }
    return true;

  case StaOnlinePhase::kConnected:
    return handleStaConnected(key);

  case StaOnlinePhase::kDisconnectedWaitShort:
    if (key == 2)
    {
      beginStaConnectAttempt();
    }
    return true;
  }
  return true;
}

// 省电模式：不启网页 AP，支持本地 CSV 播放和空闲深睡。
static bool handleStaOnlyMode(bool &syntheticKeyPress)
{
  const int total = csvTotal();
  const uint8_t key = readLoopKey(syntheticKeyPress);
  if (wakeByKey(key))
    return true;

  if (key == 3)
  {
    switchAppMode(APP_MODE_AP_STA);
    return true;
  }

  if (staOnlySleepTimeoutReached())
  {
    enterStaOnlyDeepSleep();
    return true;
  }
  if (playPostRecoveryManualMessageByKey(key))
    return true;
  if (hasPostRecoveryManualMessage())
    return true;

  if (consumePlayTrigger(key))
  {
    playLocalText(total);
  }
  return true;
}

// 分发当前 APP 模式；未知模式交回调用方做兜底复位。
static bool handleCurrentMode(bool &syntheticKeyPress)
{
  if (gAppLoopMode == APP_MODE_AP_STA)
  {
    return handleApMode(syntheticKeyPress);
  }
  if (gAppLoopMode == APP_MODE_STA_ONLINE)
  {
    return handleStaMode(syntheticKeyPress);
  }
  if (gAppLoopMode == APP_MODE_STA_ONLY)
  {
    return handleStaOnlyMode(syntheticKeyPress);
  }
  return false;
}

void processAppLoop()
{
  serviceBatteryMonitor(false);

  static LoopIrqState irq;
  bool syntheticKeyPress = false;
  const bool instantRefreshNoKey = wirelessPortalInstantRefreshNoKeyEnabled();

  dispatchModeEnterIfNeeded();
  resetIrqOnMode(irq);
  ensureImageBuf();
  reloadCsvIfNeeded(instantRefreshNoKey);
  serviceScheduleInterruptByRtcMinute();

  if (handleHostIrq(irq, syntheticKeyPress))
    return;
  if (handleWebIrq(irq, syntheticKeyPress))
    return;
  if (handleCurrentMode(syntheticKeyPress))
    return;

  switchAppMode(APP_MODE_AP_STA);
}
