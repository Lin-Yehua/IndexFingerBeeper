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

struct AppLoopImageEvent
{
  uint16_t width = 0;
  uint16_t height = 0;
  int16_t centerX = 160;
  int16_t centerY = 155;
};

struct AppLoopInputInbox
{
  static constexpr size_t kKeyDepth = 4;
  static constexpr size_t kTextDepth = 8;

  uint8_t keyQueue[kKeyDepth] = {0};
  size_t keyHead = 0;
  size_t keyCount = 0;

  String webTextQueue[kTextDepth];
  size_t webTextHead = 0;
  size_t webTextCount = 0;

  bool hasImmediateText = false;
  String immediateText;

  bool hasImage = false;
  AppLoopImageEvent image;

  bool scheduleDue = false;
};

static AppLoopInputInbox gAppLoopInbox;

static void inboxPushKey(uint8_t key)
{
  if (key == 255)
    return;

  if (gAppLoopInbox.keyCount >= AppLoopInputInbox::kKeyDepth)
  {
    gAppLoopInbox.keyHead = (gAppLoopInbox.keyHead + 1) % AppLoopInputInbox::kKeyDepth;
    --gAppLoopInbox.keyCount;
  }
  const size_t tail = (gAppLoopInbox.keyHead + gAppLoopInbox.keyCount) % AppLoopInputInbox::kKeyDepth;
  gAppLoopInbox.keyQueue[tail] = key;
  ++gAppLoopInbox.keyCount;
}

static bool inboxPopKey(uint8_t &outKey)
{
  if (gAppLoopInbox.keyCount == 0)
    return false;
  outKey = gAppLoopInbox.keyQueue[gAppLoopInbox.keyHead];
  gAppLoopInbox.keyHead = (gAppLoopInbox.keyHead + 1) % AppLoopInputInbox::kKeyDepth;
  --gAppLoopInbox.keyCount;
  return true;
}

static void inboxPushWebText(const char *text)
{
  if (!text)
    return;

  if (gAppLoopInbox.webTextCount >= AppLoopInputInbox::kTextDepth)
  {
    gAppLoopInbox.webTextQueue[gAppLoopInbox.webTextHead] = "";
    gAppLoopInbox.webTextHead = (gAppLoopInbox.webTextHead + 1) % AppLoopInputInbox::kTextDepth;
    --gAppLoopInbox.webTextCount;
  }
  const size_t tail = (gAppLoopInbox.webTextHead + gAppLoopInbox.webTextCount) % AppLoopInputInbox::kTextDepth;
  gAppLoopInbox.webTextQueue[tail] = text;
  ++gAppLoopInbox.webTextCount;
}

static bool inboxPopWebText(String &outText)
{
  if (gAppLoopInbox.webTextCount == 0)
    return false;
  outText = gAppLoopInbox.webTextQueue[gAppLoopInbox.webTextHead];
  gAppLoopInbox.webTextQueue[gAppLoopInbox.webTextHead] = "";
  gAppLoopInbox.webTextHead = (gAppLoopInbox.webTextHead + 1) % AppLoopInputInbox::kTextDepth;
  --gAppLoopInbox.webTextCount;
  return true;
}

static bool inboxHasWebText()
{
  return gAppLoopInbox.webTextCount > 0;
}

static void requestMenuFeature();

class PromptScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_PROMPT; }

  void setText(const char *text)
  {
    _text = text ? text : "";
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    (void)ctx;
    playMessageWithGlitch(_text.c_str());
  }

private:
  String _text;
};

class MenuScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_MENU; }

  void setItem(const char *text, uint8_t index, uint8_t count)
  {
    _text = text ? text : "";
    _index = index;
    _count = count;
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    (void)ctx;
    String line = _text;
    line += " ";
    line += String(static_cast<unsigned int>(_index + 1));
    line += "/";
    line += String(static_cast<unsigned int>(_count));
    playMessageWithGlitch(line.c_str());
  }

private:
  String _text;
  uint8_t _index = 0;
  uint8_t _count = 0;
};

class ImageScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_IMAGE; }

  void setImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY)
  {
    _pixels = pixels;
    _width = width;
    _height = height;
    _centerX = centerX;
    _centerY = centerY;
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    (void)ctx;
    showWebInterruptImage(_pixels, _width, _height, _centerX, _centerY);
  }

private:
  const uint16_t *_pixels = nullptr;
  uint16_t _width = 0;
  uint16_t _height = 0;
  int16_t _centerX = 160;
  int16_t _centerY = 155;
};

class UiManager
{
public:
  void setActiveFeature(uint16_t featureId)
  {
    _store.activeFeature = featureId;
  }

  uint16_t currentRoute() const
  {
    return _store.currentRoute;
  }

  void showPrompt(const char *text)
  {
    _prompt.setText(text);
    show(&_prompt);
  }

  void showMenuItem(const char *text, uint8_t index, uint8_t count)
  {
    _menu.setItem(text, index, count);
    show(&_menu);
  }

  void showImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY)
  {
    _image.setImage(pixels, width, height, centerX, centerY);
    show(&_image);
  }

private:
  AppStore _store;
  PromptScreen _prompt;
  MenuScreen _menu;
  ImageScreen _image;
  Screen *_current = nullptr;

  void show(Screen *screen)
  {
    if (!screen)
      return;
    if (_current && _current != screen)
    {
      _current->exit();
    }
    _current = screen;
    _store.currentRoute = screen->route();
    UiContext ctx;
    ctx.bus = &gAppMessageBus;
    screen->enter(_store);
    screen->render(_store, ctx);
  }
};

static UiManager gUiManager;

// 读取主循环按键；必要时消费上层中断传递下来的虚拟短按。
static uint8_t readLoopKey(bool &syntheticKeyPress)
{
  if (syntheticKeyPress)
  {
    syntheticKeyPress = false;
    return 2;
  }

  uint8_t key = 255;
  if (inboxPopKey(key))
  {
    return key;
  }
  return 255;
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

  gUiManager.showImage(gWebImageScratch, imageW, imageH, centerX, centerY);
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
static bool handleScheduleIrq(LoopIrqState &irq, bool &syntheticKeyPress)
{
  if (gAppLoopInbox.scheduleDue && gScheduleInterruptPendingStart && gScheduleInterruptCount > 0)
  {
    gAppLoopInbox.scheduleDue = false;
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

  const uint8_t key = readLoopKey(syntheticKeyPress);
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

  if (gAppLoopInbox.hasImmediateText)
  {
    const String immediateMessage = gAppLoopInbox.immediateText;
    gAppLoopInbox.immediateText = "";
    gAppLoopInbox.hasImmediateText = false;
    startImmediate(irq);
    playMessageWithGlitch(immediateMessage.c_str());
    irq.immediateActive = false;
    irq.immediateKeyLatch = false;
    irq.immediatePreemptedWeb = false;
    return true;
  }

  // Image interrupt and immediate interrupt are peer-level:
  // image polling must happen before "immediate active" wait branch.
  if (gAppLoopInbox.hasImage)
  {
    const AppLoopImageEvent image = gAppLoopInbox.image;
    gAppLoopInbox.hasImage = false;
    startImage(irq, image.width, image.height, image.centerX, image.centerY);
    return true;
  }

  if (irq.immediateActive)
  {
    const uint8_t key = readLoopKey(syntheticKeyPress);
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
    const uint8_t key = readLoopKey(syntheticKeyPress);
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

  if (!irq.webActive && inboxHasWebText())
  {
    irq.webActive = true;
    irq.webKeyLatch = false;
    Serial.println("[WEB] interrupt started");
  }

  if (irq.webActive)
  {
    String queuedMessage;
    if (inboxPopWebText(queuedMessage))
    {
      playMessageWithGlitch(queuedMessage.c_str());
      Serial.println(inboxHasWebText() ? "[WEB] interrupt next message" : "[WEB] interrupt displayed");
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
    requestMenuFeature();
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
  const uint32_t elapsed = millis() - gStaAttemptStartMs;
  if (status == WL_CONNECTED)
  {
    String okMsg = kStaConnectOkPrefix;
    okMsg += WiFi.localIP().toString();
    String ntpDetail;
    const bool ntpOk = syncDs1302FromStaNtp(ntpDetail);
    if (ntpOk)
    {
      Serial.printf("[STA] NTP sync -> DS1302 ok: %s\n", ntpDetail.c_str());
    }
    else
    {
      Serial.printf("[STA] NTP sync skipped/failed: %s\n", ntpDetail.c_str());
    }

    String uuid;
    const bool uuidReady = deviceUuidRead(uuid) && uuid.length();
    if (!uuidReady)
    {
      Serial.println("[STA] UUID unavailable; wait for NTP before cloud fetch");
      if (elapsed < kStaAttemptTimeoutMs)
      {
        return true;
      }
    }
    else
    {
      clearStaMessageQueue();
      gStaLastQueueEmptyHintMs = 0;
      gStaNextFetchAllowedMs = 0;
      Serial.printf("[STA] device UUID ready: %s\n", uuid.c_str());
      gStaOnlinePhase = StaOnlinePhase::kConnected;
      playStaMessage(okMsg);
      return true;
    }
  }

  if (elapsed < kStaAttemptTimeoutMs)
  {
    return true;
  }

  if (status == WL_CONNECTED)
  {
    WiFi.disconnect(false, false);
    delay(20);
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
    requestMenuFeature();
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
    requestMenuFeature();
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

static AppContext makeAppContext()
{
  AppContext ctx;
  ctx.bus = &gAppMessageBus;
  ctx.nowMs = millis();
  return ctx;
}

static void publishFeatureEvent(AppMsgType type, uint16_t featureId)
{
  AppMessage msg;
  msg.type = type;
  msg.priority = AppMsgPriority::Normal;
  msg.source = AppMsgSource::Feature;
  msg.targetFeature = featureId;
  msg.data.commandId = featureId;
  (void)gAppMessageBus.publish(msg);
}

class TextPushFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_TEXT_PUSH; }
  const char *title() const override { return "Text Push"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    gUiManager.setActiveFeature(id());
    Serial.println("[FEATURE] enter Text Push");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    (void)ctx;
    bool syntheticKeyPress = false;
    if (!handleCurrentMode(syntheticKeyPress))
    {
      switchAppMode(APP_MODE_AP_STA);
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }
};

class CoinFlipFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_COIN_FLIP; }
  const char *title() const override { return "Coin Flip"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    gUiManager.setActiveFeature(id());
    gUiManager.showPrompt(u8"翻硬币 | 短按开始 | 长按菜单");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    (void)ctx;
    bool syntheticKeyPress = false;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 3)
    {
      requestMenuFeature();
      return;
    }
    if (key == 2)
    {
      const bool heads = (esp_random() & 1U) != 0;
      gUiManager.showPrompt(heads ? u8"正面" : u8"反面");
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }
};

class MusicFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_MUSIC; }
  const char *title() const override { return "Music"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    gUiManager.setActiveFeature(id());
    gUiManager.showPrompt(u8"音乐功能 | 短按提示 | 长按菜单");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    (void)ctx;
    bool syntheticKeyPress = false;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 3)
    {
      requestMenuFeature();
      return;
    }
    if (key == 2)
    {
      gUiManager.showPrompt(u8"音乐模块已预留，下一步接播放列表");
    }
  }

  bool canInterrupt() const override { return false; }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }
};

class MenuFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_MENU; }
  const char *title() const override { return "Menu"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    gUiManager.setActiveFeature(id());
    _selected = 0;
    showCurrentItem();
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    (void)ctx;
    bool syntheticKeyPress = false;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 2)
    {
      _selected = (_selected + 1) % kItemCount;
      showCurrentItem();
      return;
    }
    if (key == 3)
    {
      selectCurrentItem();
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }

private:
  enum MenuItem : uint8_t
  {
    kTextPush = 0,
    kNextTextMode,
    kCoinFlip,
    kMusic,
    kItemCount,
  };

  uint8_t _selected = 0;

  const char *itemTitle() const
  {
    switch (_selected)
    {
    case kTextPush:
      return u8"菜单: 文本推送";
    case kNextTextMode:
      return u8"菜单: 切换文本模式";
    case kCoinFlip:
      return u8"菜单: 翻硬币";
    case kMusic:
      return u8"菜单: 音乐";
    }
    return u8"菜单";
  }

  void showCurrentItem() const
  {
    gUiManager.showMenuItem(itemTitle(), _selected, kItemCount);
  }

  void selectCurrentItem();
};

static TextPushFeature gTextPushFeature;
static MenuFeature gMenuFeature;
static CoinFlipFeature gCoinFlipFeature;
static MusicFeature gMusicFeature;
static IAppFeature *gActiveFeature = &gTextPushFeature;
static bool gFeaturesBegun = false;

static IAppFeature *featureById(uint16_t featureId)
{
  switch (featureId)
  {
  case APP_FEATURE_TEXT_PUSH:
    return &gTextPushFeature;
  case APP_FEATURE_MENU:
    return &gMenuFeature;
  case APP_FEATURE_COIN_FLIP:
    return &gCoinFlipFeature;
  case APP_FEATURE_MUSIC:
    return &gMusicFeature;
  default:
    return nullptr;
  }
}

static void ensureFeaturesBegun()
{
  if (gFeaturesBegun)
    return;
  AppContext ctx = makeAppContext();
  (void)gTextPushFeature.begin(ctx);
  (void)gMenuFeature.begin(ctx);
  (void)gCoinFlipFeature.begin(ctx);
  (void)gMusicFeature.begin(ctx);
  gFeaturesBegun = true;
  gActiveFeature->enter(ctx);
}

static void enterFeatureById(uint16_t featureId)
{
  ensureFeaturesBegun();
  IAppFeature *next = featureById(featureId);
  if (!next || next == gActiveFeature)
    return;
  AppContext ctx = makeAppContext();
  gActiveFeature->exit(ctx);
  gActiveFeature = next;
  gActiveFeature->enter(ctx);
  publishFeatureEvent(AppMsgType::FeatureSelected, featureId);
}

static void requestMenuFeature()
{
  enterFeatureById(APP_FEATURE_MENU);
}

static AppLoopMode nextTextPushMode()
{
  switch (gAppLoopMode)
  {
  case APP_MODE_AP_STA:
    return APP_MODE_STA_ONLINE;
  case APP_MODE_STA_ONLINE:
    return APP_MODE_STA_ONLY;
  case APP_MODE_STA_ONLY:
  default:
    return APP_MODE_AP_STA;
  }
}

void MenuFeature::selectCurrentItem()
{
  switch (_selected)
  {
  case kTextPush:
    enterFeatureById(APP_FEATURE_TEXT_PUSH);
    break;
  case kNextTextMode:
    switchAppMode(nextTextPushMode());
    enterFeatureById(APP_FEATURE_TEXT_PUSH);
    break;
  case kCoinFlip:
    enterFeatureById(APP_FEATURE_COIN_FLIP);
    break;
  case kMusic:
    enterFeatureById(APP_FEATURE_MUSIC);
    break;
  }
}

static void tickActiveFeature()
{
  ensureFeaturesBegun();
  AppContext ctx = makeAppContext();
  gActiveFeature->tick(ctx);
}

static void serviceAppMessageBus()
{
  if (!gAppMessageBus.begin())
  {
    return;
  }

  AppMessage msg;
  uint8_t drained = 0;
  while (drained < 8 && gAppMessageBus.poll(msg))
  {
    ++drained;
    switch (msg.type)
    {
    case AppMsgType::KeyPressed:
      inboxPushKey(msg.data.keyCode);
      break;
    case AppMsgType::TextPushReceived:
      inboxPushWebText(msg.text);
      break;
    case AppMsgType::ImmediateTextReceived:
      gAppLoopInbox.immediateText = msg.text;
      gAppLoopInbox.hasImmediateText = true;
      break;
    case AppMsgType::ImageReceived:
      gAppLoopInbox.image.width = msg.data.image.width;
      gAppLoopInbox.image.height = msg.data.image.height;
      gAppLoopInbox.image.centerX = msg.data.image.centerX;
      gAppLoopInbox.image.centerY = msg.data.image.centerY;
      gAppLoopInbox.hasImage = true;
      break;
    case AppMsgType::ScheduleDue:
      gAppLoopInbox.scheduleDue = true;
      break;
    case AppMsgType::MenuOpenRequested:
      requestMenuFeature();
      break;
    case AppMsgType::FeatureSelected:
      if (msg.source != AppMsgSource::Feature)
      {
        enterFeatureById(msg.targetFeature ? msg.targetFeature : msg.data.commandId);
      }
      break;
    case AppMsgType::FeatureFinished:
      enterFeatureById(APP_FEATURE_TEXT_PUSH);
      break;
    default:
      Serial.printf("[APPBUS] unhandled type=%s source=%u priority=%u target=%u\n", appMessageTypeName(msg.type),
                    static_cast<unsigned int>(msg.source), static_cast<unsigned int>(msg.priority),
                    static_cast<unsigned int>(msg.targetFeature));
      break;
    }
  }
}

void processAppLoop()
{
  serviceBatteryMonitor(false);
  appCheckLowBatterySleepIfNeeded(false, "app-loop");
  serviceLowBatteryWarning();

  static LoopIrqState irq;
  bool syntheticKeyPress = false;
  const bool instantRefreshNoKey = wirelessPortalInstantRefreshNoKeyEnabled();

  dispatchModeEnterIfNeeded();
  resetIrqOnMode(irq);
  ensureImageBuf();
  reloadCsvIfNeeded(instantRefreshNoKey);
  serviceScheduleInterruptByRtcMinute();
  pumpAppInputSources();
  serviceAppMessageBus();

  if (handleHostIrq(irq, syntheticKeyPress))
    return;
  if (handleWebIrq(irq, syntheticKeyPress))
    return;
  tickActiveFeature();
}
