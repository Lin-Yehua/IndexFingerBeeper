/* Shared app-loop services: text playback, portal polling, and mode gates. Included by UsbAppMode.cpp. */
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

  appUi().showImage(gWebImageScratch, imageW, imageH, centerX, centerY);
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
  const bool canResumeWeb = irq.immediatePreemptedWeb && (inboxHasWebText() || wirelessPortalHasPendingMessage());
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

static bool webIrqAllowed();

static bool publishAppInputEvent(const AppMessage &msg)
{
  if (gAppMessageBus.publish(msg))
  {
    return true;
  }
  else
  {
    Serial.printf("[APPBUS] drop input type=%s\n", appMessageTypeName(msg.type));
    return false;
  }
}

static void pumpKeyInputEvent()
{
  Key_loop();
  const uint8_t key = get_Keycode();
  if (key == 255)
    return;

  AppMessage msg;
  msg.type = AppMsgType::KeyPressed;
  msg.priority = (key == 3) ? AppMsgPriority::High : AppMsgPriority::Normal;
  msg.source = AppMsgSource::Key;
  msg.data.keyCode = key;
  publishAppInputEvent(msg);
}

static void pumpWebInputEvents()
{
  if (!webIrqAllowed())
    return;

  String immediateMessage;
  if (wirelessPortalPopImmediateMessage(immediateMessage))
  {
    AppMessage msg;
    msg.type = AppMsgType::ImmediateTextReceived;
    msg.priority = AppMsgPriority::Interrupt;
    msg.source = AppMsgSource::WirelessPortal;
    appMessageCopyText(msg, immediateMessage.c_str());
    publishAppInputEvent(msg);
  }

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
      AppMessage msg;
      msg.type = AppMsgType::ImageReceived;
      msg.priority = AppMsgPriority::Interrupt;
      msg.source = AppMsgSource::WirelessPortal;
      msg.data.image.width = imageW;
      msg.data.image.height = imageH;
      msg.data.image.centerX = centerX;
      msg.data.image.centerY = centerY;
      publishAppInputEvent(msg);
    }
  }

  if (inboxHasWebText())
    return;

  String queuedMessage;
  if (wirelessPortalPopMessage(queuedMessage))
  {
    AppMessage msg;
    msg.type = AppMsgType::TextPushReceived;
    msg.priority = AppMsgPriority::Normal;
    msg.source = AppMsgSource::WirelessPortal;
    appMessageCopyText(msg, queuedMessage.c_str());
    publishAppInputEvent(msg);
  }
}

static void pumpScheduleInputEvent()
{
  static bool duePublished = false;
  if (!hostIrqAllowed() || !gScheduleInterruptPendingStart || gScheduleInterruptCount == 0)
  {
    duePublished = false;
    return;
  }
  if (duePublished)
    return;

  AppMessage msg;
  msg.type = AppMsgType::ScheduleDue;
  msg.priority = AppMsgPriority::Interrupt;
  msg.source = AppMsgSource::Schedule;
  duePublished = publishAppInputEvent(msg);
}

static void pumpAppInputSources()
{
  pumpKeyInputEvent();
  pumpWebInputEvents();
  pumpScheduleInputEvent();
}

// 当前模式是否允许网页普通/立即/图片中断。
static bool webIrqAllowed()
{
  return (gAppLoopMode == APP_MODE_AP_STA) ||
         (gAppLoopMode == APP_MODE_STA_ONLINE && gStaOnlinePhase == StaOnlinePhase::kConnected);
}

// 维护日程中断队列，包括插队、按键切换和重复提醒。
