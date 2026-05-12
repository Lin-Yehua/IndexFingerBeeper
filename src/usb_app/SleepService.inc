/*
 * 文件说明: 深睡与快恢复私有实现。
 * 文件功能: 保存/读取睡眠快照，设置 RTC 唤醒上下文，并执行深睡维护唤醒流程。
 *
 * 函数表:
 * - chooseRtcRefillStepSec: 计算、判断或转换对应结果。
 * - appendSnapshotMessage: 保存、写入或更新对应数据。
 * - writeFixedMessage: 保存、写入或更新对应数据。
 * - readFixedMessage: 读取、获取或消费对应数据。
 * - restorePortalQueuesFromCapture: 模块内部辅助函数。
 * - saveSleepSnapshotToFat: 保存、写入或更新对应数据。
 * - loadSleepSnapshotFromFat: 读取、获取或消费对应数据。
 * - decodeSnapshotMode: 解析、规范化或格式化对应内容。
 * - applySleepSnapshot: 应用配置或切换运行状态。
 * - markSleepRtcContextForSleep: 模块内部辅助函数。
 * - waitWakeKeyReleaseBeforeSleep: 模块内部辅助函数。
 * - enterDeepSleepNow: 模块内部辅助函数。
 * - handleRtcMaintenanceWake: 处理对应业务流程或事件分支。
 */
uint32_t chooseRtcRefillStepSec(uint64_t diffSec)
{
  if (diffSec >= 1800ULL)
    return 1800U;
  if (diffSec >= 600ULL)
    return 600U;
  if (diffSec >= 300ULL)
    return 300U;
  if (diffSec >= 60ULL)
    return 60U;
  return 10U;
}

void appendSnapshotMessage(std::vector<String> &out, const String &raw, size_t cap)
{
  if (out.size() >= cap)
    return;
  String normalized = raw;
  normalized.replace("\r", " ");
  normalized.replace("\n", " ");
  normalized.trim();
  if (!normalized.length())
    return;
  if (normalized.length() > kSleepTextMaxLen)
  {
    normalized.remove(kSleepTextMaxLen);
  }
  out.push_back(normalized);
}

bool writeFixedMessage(fs::File &f, const String &msg)
{
  char fixed[kSleepTextMaxLen + 1] = {0};
  sanitizeMessageForSnapshot(msg, fixed);
  return writeExact(f, fixed, sizeof(fixed));
}

bool readFixedMessage(fs::File &f, String &out)
{
  char fixed[kSleepTextMaxLen + 1] = {0};
  if (!readExact(f, fixed, sizeof(fixed)))
    return false;
  fixed[kSleepTextMaxLen] = '\0';
  out = String(fixed);
  out.trim();
  return true;
}

void restorePortalQueuesFromCapture(const std::vector<String> &regularQueue, bool hasImmediate,
                                    const String &immediateMessage, const std::vector<String> &hostQueue)
{
  for (const String &msg : regularQueue)
  {
    (void)wirelessPortalPushMessageForRestore(msg);
  }
  if (hasImmediate)
  {
    (void)wirelessPortalPushImmediateMessageForRestore(immediateMessage);
  }
  for (const String &msg : hostQueue)
  {
    (void)wirelessPortalPushHostMessageForRestore(msg);
  }
}

bool saveSleepSnapshotToFat()
{
  if (!fatMounted)
  {
    Serial.println("[SLEEP] FAT not mounted, skip snapshot");
    return false;
  }

  SleepSnapshotHeader header;
  header.magic = kSleepFileMagic;
  header.version = kSleepFileVersion;
  header.appMode = static_cast<uint8_t>(gAppLoopMode);
  header.runState = RUNSTATE;
  header.firstFlag = firstFlag ? 1U : 0U;
  header.staPhase = static_cast<uint8_t>(gStaOnlinePhase);
  header.csvCount = csvCount;
  header.staRetryCount = gStaRetryCount;
  header.backlightTimeSec = gBacklightTimeSec;
  sanitizeMessageForSnapshot(String(gLastDisplayedText), header.lastDisplayed);

  std::vector<String> staQueue;
  staQueue.reserve(kSleepStaQueueMax);
  if (ensureStaQueueMutex() && xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) == pdTRUE)
  {
    const size_t count = std::min(gStaMsgQueueSize, kSleepStaQueueMax);
    for (size_t i = 0; i < count; ++i)
    {
      const size_t idx = (gStaMsgQueueHead + i) % kStaPrefetchDepth;
      appendSnapshotMessage(staQueue, gStaMsgQueue[idx], kSleepStaQueueMax);
    }
    xSemaphoreGive(gStaMsgQueueMutex);
  }

  std::vector<String> regularQueue;
  std::vector<String> hostQueue;
  regularQueue.reserve(kSleepPortalQueueMax);
  hostQueue.reserve(kSleepPortalQueueMax);
  String tmp;
  while (regularQueue.size() < kSleepPortalQueueMax && wirelessPortalPopMessage(tmp))
  {
    appendSnapshotMessage(regularQueue, tmp, kSleepPortalQueueMax);
  }
  while (hostQueue.size() < kSleepPortalQueueMax && wirelessPortalPopHostMessage(tmp))
  {
    appendSnapshotMessage(hostQueue, tmp, kSleepPortalQueueMax);
  }
  String immediateMessage;
  const bool hasImmediate = wirelessPortalPopImmediateMessage(immediateMessage);
  if (hasImmediate)
  {
    sanitizeMessageForSnapshot(immediateMessage, header.immediateMessage);
    header.hasImmediate = 1U;
  }

  header.staQueueCount = static_cast<uint8_t>(staQueue.size());
  header.regularQueueCount = static_cast<uint8_t>(regularQueue.size());
  header.hostQueueCount = static_cast<uint8_t>(hostQueue.size());

  if (!fatFsTakeWriteMutex(3000))
  {
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] snapshot write lock failed");
    return false;
  }

  (void)FFat.remove(kSleepSnapshotTmpPath);
  fs::File f = FFat.open(kSleepSnapshotTmpPath, "w");
  if (!f)
  {
    fatFsGiveWriteMutex();
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] open snapshot file failed");
    return false;
  }

  bool ok = writeExact(f, &header, sizeof(header));
  if (ok)
  {
    ok = writeExact(f, csvArray, sizeof(csvArray));
  }
  if (ok)
  {
    for (const String &msg : staQueue)
    {
      if (!writeFixedMessage(f, msg))
      {
        ok = false;
        break;
      }
    }
  }
  if (ok)
  {
    for (const String &msg : regularQueue)
    {
      if (!writeFixedMessage(f, msg))
      {
        ok = false;
        break;
      }
    }
  }
  if (ok)
  {
    for (const String &msg : hostQueue)
    {
      if (!writeFixedMessage(f, msg))
      {
        ok = false;
        break;
      }
    }
  }
  f.flush();
  f.close();

  if (!ok)
  {
    (void)FFat.remove(kSleepSnapshotTmpPath);
    fatFsGiveWriteMutex();
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] snapshot write failed");
    return false;
  }

  if (FFat.exists(kSleepSnapshotPath) && !FFat.remove(kSleepSnapshotPath))
  {
    (void)FFat.remove(kSleepSnapshotTmpPath);
    fatFsGiveWriteMutex();
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] old snapshot remove failed");
    return false;
  }
  if (!FFat.rename(kSleepSnapshotTmpPath, kSleepSnapshotPath))
  {
    (void)FFat.remove(kSleepSnapshotTmpPath);
    fatFsGiveWriteMutex();
    restorePortalQueuesFromCapture(regularQueue, hasImmediate, immediateMessage, hostQueue);
    Serial.println("[SLEEP] snapshot rename failed");
    return false;
  }
  fatFsGiveWriteMutex();

  Serial.printf("[SLEEP] snapshot saved mode=%u staQ=%u webQ=%u hostQ=%u\n", static_cast<unsigned int>(header.appMode),
                static_cast<unsigned int>(header.staQueueCount), static_cast<unsigned int>(header.regularQueueCount),
                static_cast<unsigned int>(header.hostQueueCount));
  return true;
}

bool loadSleepSnapshotFromFat(SleepSnapshotData &outData)
{
  if (!fatMounted)
    return false;
  fs::File f = FFat.open(kSleepSnapshotPath, FILE_READ);
  if (!f)
    return false;

  SleepSnapshotHeader header;
  if (!readExact(f, &header, sizeof(header)))
  {
    f.close();
    return false;
  }
  if (header.magic != kSleepFileMagic || header.version != kSleepFileVersion)
  {
    f.close();
    return false;
  }
  if (header.staQueueCount > kSleepStaQueueMax || header.regularQueueCount > kSleepPortalQueueMax ||
      header.hostQueueCount > kSleepPortalQueueMax)
  {
    f.close();
    return false;
  }
  if (!readExact(f, csvArray, sizeof(csvArray)))
  {
    f.close();
    return false;
  }

  outData.header = header;
  outData.staQueue.clear();
  outData.regularQueue.clear();
  outData.hostQueue.clear();
  outData.staQueue.reserve(header.staQueueCount);
  outData.regularQueue.reserve(header.regularQueueCount);
  outData.hostQueue.reserve(header.hostQueueCount);

  String item;
  for (uint8_t i = 0; i < header.staQueueCount; ++i)
  {
    if (!readFixedMessage(f, item))
    {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.staQueue, item, kSleepStaQueueMax);
  }
  for (uint8_t i = 0; i < header.regularQueueCount; ++i)
  {
    if (!readFixedMessage(f, item))
    {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.regularQueue, item, kSleepPortalQueueMax);
  }
  for (uint8_t i = 0; i < header.hostQueueCount; ++i)
  {
    if (!readFixedMessage(f, item))
    {
      f.close();
      return false;
    }
    appendSnapshotMessage(outData.hostQueue, item, kSleepPortalQueueMax);
  }

  f.close();
  return true;
}

AppLoopMode decodeSnapshotMode(uint8_t rawMode)
{
  switch (rawMode)
  {
  case APP_MODE_AP_STA:
  case APP_MODE_STA_ONLINE:
  case APP_MODE_STA_ONLY:
    return static_cast<AppLoopMode>(rawMode);
  default:
    return APP_MODE_AP_STA;
  }
}

StaOnlinePhase decodeSnapshotStaPhase(uint8_t rawPhase)
{
  switch (rawPhase)
  {
  case static_cast<uint8_t>(StaOnlinePhase::kPromptWaitShort):
  case static_cast<uint8_t>(StaOnlinePhase::kConnecting):
  case static_cast<uint8_t>(StaOnlinePhase::kFailWaitShort):
  case static_cast<uint8_t>(StaOnlinePhase::kConnected):
  case static_cast<uint8_t>(StaOnlinePhase::kDisconnectedWaitShort):
    return static_cast<StaOnlinePhase>(rawPhase);
  default:
    return StaOnlinePhase::kPromptWaitShort;
  }
}

bool applySleepSnapshot(const SleepSnapshotData &snapshot, bool replayLastDisplayed)
{
  gAppLoopMode = decodeSnapshotMode(snapshot.header.appMode);
  gAppModeEnterPending = false;

  RUNSTATE = snapshot.header.runState;
  firstFlag = snapshot.header.firstFlag != 0;
  csvCount = snapshot.header.csvCount;
  if (csvCount < 0)
    csvCount = 0;
  if (csvCount > kCsvArrayCapacity)
    csvCount = kCsvArrayCapacity;

  gStaOnlinePhase = decodeSnapshotStaPhase(snapshot.header.staPhase);
  gStaRetryCount = snapshot.header.staRetryCount;
  gStaAttemptStartMs = millis();
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;

  wirelessPortalStop();
  bool portalOk = false;
  if (gAppLoopMode == APP_MODE_STA_ONLY)
  {
    WiFi.disconnect(true, false);
    portalOk = wirelessPortalStartEspNowOnly();
  }
  else
  {
    portalOk = wirelessPortalStart();
  }
  if (!portalOk)
  {
    Serial.println("[SLEEP] restore portal start failed");
  }

  clearStaMessageQueue();
  for (const String &msg : snapshot.staQueue)
  {
    (void)pushStaMessageQueue(msg);
  }

  for (const String &msg : snapshot.regularQueue)
  {
    (void)wirelessPortalPushMessageForRestore(msg);
  }
  if (snapshot.header.hasImmediate)
  {
    (void)wirelessPortalPushImmediateMessageForRestore(String(snapshot.header.immediateMessage));
  }
  for (const String &msg : snapshot.hostQueue)
  {
    (void)wirelessPortalPushHostMessageForRestore(msg);
  }

  if (gAppLoopMode == APP_MODE_STA_ONLINE)
  {
    (void)loadStaCredentialsFromSettingIni(gStaNetSsid, gStaNetPassword, gStaNetApi);
    ensureStaFetcherTaskStarted();
    if (gStaOnlinePhase == StaOnlinePhase::kConnecting && gStaNetSsid.length())
    {
      WiFi.mode(WIFI_STA);
      WiFi.begin(gStaNetSsid.c_str(), gStaNetPassword.c_str());
    }
  }
  else
  {
    gStaNetSsid = "";
    gStaNetPassword = "";
    gStaNetApi = kStaCloudApiUrlDefault;
  }

  gBacklightTimeSec = snapshot.header.backlightTimeSec;
  setBacklightTimeSeconds(gBacklightTimeSec);
  notifyBacklightActivity();

  rememberLastDisplayedText(snapshot.header.lastDisplayed);
  if (replayLastDisplayed && snapshot.header.lastDisplayed[0])
  {
    playMessageWithGlitch(snapshot.header.lastDisplayed);
  }

  return true;
}

void markSleepRtcContextForSleep()
{
  gSleepRtcCtx.magic = kSleepRtcCtxMagic;
  gSleepRtcCtx.version = kSleepRtcCtxVersion;
  gSleepRtcCtx.snapshotValid = 1;
  gSleepRtcCtx.appMode = static_cast<uint8_t>(gAppLoopMode);
  gSleepRtcCtx.nextWakeSec = kRtcWakeDefaultSec;
  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.reminderIntervalSec = 0;
  gSleepRtcCtx.reminderTimes = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';

  uint64_t nowUnix = 0;
  if (!readRtcUnix(nowUnix))
  {
    gSleepRtcCtx.expectedUnix = 0;
    return;
  }
  gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(kRtcWakeDefaultSec);
}

void waitWakeKeyReleaseBeforeSleep()
{
  pinMode(static_cast<uint8_t>(kWakeKeyGpio), INPUT_PULLUP);
  const uint32_t started = millis();
  while (digitalRead(static_cast<uint8_t>(kWakeKeyGpio)) == LOW)
  {
    if ((millis() - started) > 2500U)
      break;
    delay(10);
  }
}

[[noreturn]] void enterDeepSleepNow(uint32_t wakeSec)
{
  if (wakeSec == 0)
    wakeSec = kRtcWakeDefaultSec;
  if (wakeSec > kRtcWakeMaxSec)
    wakeSec = kRtcWakeMaxSec;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(kWakeKeyGpio, 0);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(wakeSec) * 1000000ULL);
  Serial.printf("[SLEEP] enter deep sleep ext0(gpio=%d) timer=%us\n", static_cast<int>(kWakeKeyGpio),
                static_cast<unsigned int>(wakeSec));
  delay(20);
  esp_deep_sleep_start();
  while (true)
  {
    delay(1000);
  }
}

bool handleRtcMaintenanceWake()
{
  ledcWrite(0, 0);
  rtc.begin();

  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.reminderIntervalSec = 0;
  gSleepRtcCtx.reminderTimes = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';

  uint32_t nextWakeSec = kRtcWakeDefaultSec;
  uint64_t nowUnix = 0;
  const bool rtcOk = readRtcUnix(nowUnix);

  if (!rtcOk)
  {
    nextWakeSec = 10U;
    gSleepRtcCtx.expectedUnix = 0;
    Serial.println("[SLEEP] RTC read invalid, no external time, quick retry");
  }
  else
  {
    ReminderSchedule &schedule = scheduleScratchBuffer();
    uint32_t nextReminderDelta = 0;
    char nextReminderMessage[kSleepTextMaxLen + 1] = {0};
    uint16_t nextReminderIntervalSec = 0;
    uint16_t nextReminderTimes = 0;
    const bool hasSchedule = loadReminderSchedule(schedule);
    if (hasSchedule && computeNextReminderDelta(schedule, nowUnix, nextReminderDelta, nextReminderMessage,
                                                &nextReminderIntervalSec, &nextReminderTimes))
    {
      if (nextReminderDelta <= kReminderTriggerWindowSec)
      {
        gSleepRtcCtx.pendingReminder = 1;
        gSleepRtcCtx.reminderIntervalSec = nextReminderIntervalSec;
        gSleepRtcCtx.reminderTimes = nextReminderTimes;
        memcpy(gSleepRtcCtx.reminderMessage, nextReminderMessage, sizeof(gSleepRtcCtx.reminderMessage));
        gSleepRtcCtx.reminderMessage[sizeof(gSleepRtcCtx.reminderMessage) - 1] = '\0';
        gSleepRtcCtx.expectedUnix = nowUnix;
        gSleepRtcCtx.nextWakeSec = 0;
        Serial.printf("[SLEEP] reminder due now (delta=%u sec, interval=%u, times=%u), continue boot\n",
                      static_cast<unsigned int>(nextReminderDelta), static_cast<unsigned int>(nextReminderIntervalSec),
                      static_cast<unsigned int>(nextReminderTimes));
        return true;
      }

      const uint32_t tierSec = chooseRtcRefillStepSec(nextReminderDelta);
      nextWakeSec = std::min(nextReminderDelta, tierSec);
      if (nextWakeSec == 0)
      {
        nextWakeSec = 1;
      }
      nextWakeSec = std::min(nextWakeSec, kRtcWakeMaxSec);
      gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(nextWakeSec);
      Serial.printf("[SLEEP] schedule pending delta=%u sec, tier=%u sec, next=%u sec\n",
                    static_cast<unsigned int>(nextReminderDelta), static_cast<unsigned int>(tierSec),
                    static_cast<unsigned int>(nextWakeSec));
    }
    else
    {
      const uint64_t targetUnix = gSleepRtcCtx.expectedUnix;
      const uint64_t diffSec = (targetUnix == 0ULL) ? 0ULL : absDiffU64(nowUnix, targetUnix);
      if (targetUnix != 0ULL && diffSec <= kRtcMismatchToleranceSec)
      {
        nextWakeSec = kRtcWakeDefaultSec;
      }
      else
      {
        const uint32_t refillStep = chooseRtcRefillStepSec(diffSec == 0ULL ? kRtcWakeDefaultSec : diffSec);
        nextWakeSec = std::min(refillStep, kRtcWakeMaxSec);
      }
      gSleepRtcCtx.expectedUnix = nowUnix + static_cast<uint64_t>(nextWakeSec);
      Serial.printf("[SLEEP] no schedule, heartbeat next=%u sec\n", static_cast<unsigned int>(nextWakeSec));
    }
  }

  gSleepRtcCtx.magic = kSleepRtcCtxMagic;
  gSleepRtcCtx.version = kSleepRtcCtxVersion;
  gSleepRtcCtx.snapshotValid = 1;
  gSleepRtcCtx.nextWakeSec = nextWakeSec;
  enterDeepSleepNow(nextWakeSec);
  return false;
}
