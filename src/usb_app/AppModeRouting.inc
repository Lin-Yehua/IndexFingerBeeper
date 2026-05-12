/*
 * 文件说明: APP 模式路由私有实现。
 * 文件功能: 封装 APP 模式切换、启动模式读取和模式进入回调调度。
 *
 * 函数表:
 * - playStaMessage: 播放对应消息或音频流程。
 * - playAPMessage: 播放对应消息或音频流程。
 * - playStaOnlyMessage: 播放对应消息或音频流程。
 * - beginStaConnectAttempt: 初始化或确保对应资源可用。
 * - modeToIndex: 模块内部辅助函数。
 * - switchAppMode: 应用配置或切换运行状态。
 * - dispatchModeEnterIfNeeded: 模块内部辅助函数。
 * - setAppModeEnterCallback: 保存、写入或更新对应数据。
 * - setAppModeInitCallback: 保存、写入或更新对应数据。
 * - getAppLoopMode: 读取、获取或消费对应数据。
 * - applyStartupModeFromSettingIni: 应用配置或切换运行状态。
 */
static void playStaMessage(const String &text)
{
  playMessageWithGlitch(text.c_str());
}
static void playAPMessage(const String &text)
{
  playMessageWithGlitch(text.c_str());
}
static void playStaOnlyMessage(const String &text)
{
  playMessageWithGlitch(text.c_str());
}
static void beginStaConnectAttempt()
{
  if (!gStaNetSsid.length())
  {
    gStaOnlinePhase = StaOnlinePhase::kFailWaitShort;
    playStaMessage(kStaMissingCfgMsg);
    return;
  }

  String msg = kStaConnectingPrefix;
  msg += gStaNetSsid;
  if (gStaRetryCount > 0)
  {
    msg += kStaRetryPrefix;
    msg += String(gStaRetryCount);
  }
  playStaMessage(msg);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(20);
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  WiFi.begin(gStaNetSsid.c_str(), gStaNetPassword.c_str());

  gStaAttemptStartMs = millis();
  gStaOnlinePhase = StaOnlinePhase::kConnecting;
}

static uint8_t modeToIndex(AppLoopMode mode)
{
  switch (mode)
  {
  case APP_MODE_AP_STA:
    return 0;
  case APP_MODE_STA_ONLINE:
    return 1;
  case APP_MODE_STA_ONLY:
    return 2;
  default:
    return 0;
  }
}

static void switchAppMode(AppLoopMode mode)
{
  if (gAppLoopMode == mode)
    return;
  gAppLoopMode = mode;
  gAppModeEnterPending = true;
  if (!persistAppModeToSettingIni(mode))
  {
    Serial.println("[MODE] save Mode to /setting.ini failed");
  }
  else
  {
    Serial.printf("[MODE] switched -> %s\n", appModeToIniValue(mode));
  }
}

static void dispatchModeEnterIfNeeded()
{
  if (!gAppModeEnterPending)
    return;
  gAppModeEnterPending = false;
  AppModeEnterCallback callback = gAppModeInitCallbacks[modeToIndex(gAppLoopMode)];
  if (!callback)
  {
    callback = gAppModeEnterCallback; // Fallback generic callback.
  }
  if (callback)
  {
    callback(gAppLoopMode);
  }
}

void setAppModeEnterCallback(AppModeEnterCallback callback)
{
  gAppModeEnterCallback = callback;
  if (callback)
  {
    // Ensure current mode triggers once after callback registration.
    gAppModeEnterPending = true;
  }
}

void setAppModeInitCallback(AppLoopMode mode, AppModeEnterCallback callback)
{
  gAppModeInitCallbacks[modeToIndex(mode)] = callback;
  if (callback && mode == gAppLoopMode)
  {
    // Ensure current mode triggers once after per-mode callback registration.
    gAppModeEnterPending = true;
  }
}

AppLoopMode getAppLoopMode()
{
  return gAppLoopMode;
}

void applyStartupModeFromSettingIni()
{
  AppLoopMode startupMode = gAppLoopMode;
  if (loadAppModeFromSettingIni(startupMode))
  {
    gAppLoopMode = startupMode;
    Serial.printf("[BOOT] startup Mode=%s\n", appModeToIniValue(gAppLoopMode));
  }
  else
  {
    Serial.printf("[BOOT] startup Mode fallback=%s\n", appModeToIniValue(gAppLoopMode));
  }
  gAppModeEnterPending = true;
  dispatchModeEnterIfNeeded();
}
