/* Text push mode controllers. Included by UsbAppMode.cpp. */
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
// 省电模式：不启网页 AP，支持本地 CSV 播放和空闲深睡。
// 分发当前 APP 模式；未知模式交回调用方做兜底复位。
class ITextPushModeController
{
public:
  virtual AppLoopMode mode() const = 0;
  virtual bool tick(bool &syntheticKeyPress) = 0;
};

class ApStaTextModeController final : public ITextPushModeController
{
public:
  AppLoopMode mode() const override { return APP_MODE_AP_STA; }
  bool tick(bool &syntheticKeyPress) override
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
};

class StaOnlineTextModeController final : public ITextPushModeController
{
public:
  AppLoopMode mode() const override { return APP_MODE_STA_ONLINE; }
  bool tick(bool &syntheticKeyPress) override
  {
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return true;

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
};

class StaOnlyTextModeController final : public ITextPushModeController
{
public:
  AppLoopMode mode() const override { return APP_MODE_STA_ONLY; }
  bool tick(bool &syntheticKeyPress) override
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
};

static ApStaTextModeController gApStaTextMode;
static StaOnlineTextModeController gStaOnlineTextMode;
static StaOnlyTextModeController gStaOnlyTextMode;

static ITextPushModeController *textModeControllerFor(AppLoopMode mode);

static bool runCurrentTextPushMode(bool &syntheticKeyPress)
{
  ITextPushModeController *controller = textModeControllerFor(gAppLoopMode);
  if (!controller)
    return false;
  return controller->tick(syntheticKeyPress);
}

static ITextPushModeController *textModeControllerFor(AppLoopMode mode)
{
  switch (mode)
  {
  case APP_MODE_AP_STA:
    return &gApStaTextMode;
  case APP_MODE_STA_ONLINE:
    return &gStaOnlineTextMode;
  case APP_MODE_STA_ONLY:
    return &gStaOnlyTextMode;
  default:
    return nullptr;
  }
}

