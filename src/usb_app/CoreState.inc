/*
 * 文件说明: APP 模式核心状态私有实现。
 * 文件功能: 集中定义 UsbAppMode 内部常量、运行状态、队列状态和跨模块共享的小工具。
 *
 * 函数表:
 * - ensureStaQueueMutex: 初始化或确保对应资源可用。
 * - clearStaMessageQueue: 停止、释放或清理对应状态。
 * - pushStaMessageQueue: 保存、写入或更新对应数据。
 * - loadStaCredentialsFromSettingIni: 读取、获取或消费对应数据。
 * - ensureStaFetcherTaskStarted: 初始化或确保对应资源可用。
 * - beginStaConnectAttempt: 初始化或确保对应资源可用。
 * - refreshTodayReminderSlotsFromRtc: 模块内部辅助函数。
 * - parseAppModeFromIniValue: 解析、规范化或格式化对应内容。
 * - loadAppModeFromSettingIni: 读取、获取或消费对应数据。
 * - persistAppModeToSettingIni: 保存、写入或更新对应数据。
 * - playMessageWithGlitch: 播放对应消息或音频流程。
 * - waitWakeKeyReleaseBeforeSleep: 模块内部辅助函数。
 * - saveSleepSnapshotToFat: 保存、写入或更新对应数据。
 * - markSleepRtcContextForSleep: 模块内部辅助函数。
 * - enterDeepSleepNow: 模块内部辅助函数。
 * - handleRtcMaintenanceWake: 处理对应业务流程或事件分支。
 * - sanitizeMessageForSnapshot: 模块内部辅助函数。
 * - rememberLastDisplayedText: 模块内部辅助函数。
 * - reminderRepeatPriority: 模块内部辅助函数。
 * - normalizeScheduleQueueMessage: 解析、规范化或格式化对应内容。
 * - clearScheduleInterruptQueue: 停止、释放或清理对应状态。
 * - appendScheduleInterruptQueueItem: 保存、写入或更新对应数据。
 * - enqueueScheduleInterruptMessage: 保存、写入或更新对应数据。
 * - insertHostMessageIntoScheduleInterruptQueueFront: 保存、写入或更新对应数据。
 * - popScheduleInterruptMessage: 读取、获取或消费对应数据。
 * - writeExact: 保存、写入或更新对应数据。
 * - readExact: 读取、获取或消费对应数据。
 * - clearSleepRtcContext: 停止、释放或清理对应状态。
 * - hasValidSleepRtcContext: 计算、判断或转换对应结果。
 * - clampUnitFloat: 计算、判断或转换对应结果。
 * - backlightDutyBrightFromLevel: 模块内部辅助函数。
 * - backlightDutyDimFromBright: 模块内部辅助函数。
 * - applyBacklightState: 应用配置或切换运行状态。
 * - wakeBacklightByKeyIfNeeded: 模块内部辅助函数。
 * - backlightCloseDelayMs: 模块内部辅助函数。
 * - staOnlySleepAfterOffDelayMs: 模块内部辅助函数。
 * - staOnlySleepTimeoutReached: 模块内部辅助函数。
 * - enterStaOnlyDeepSleep: 模块内部辅助函数。
 * - backlightTask: FreeRTOS 任务入口或任务控制函数。
 * - ensureBacklightTaskStarted: 初始化或确保对应资源可用。
 * - clampBatteryVoltage: 计算、判断或转换对应结果。
 * - batteryPercentFromVoltage: 模块内部辅助函数。
 * - ensureBatteryPinConfigured: 初始化或确保对应资源可用。
 * - sampleBatteryVoltage: 模块内部辅助函数。
 * - serviceBatteryMonitor: 周期服务函数，维护对应后台状态。
 * - appGetBatteryStatus: 模块内部辅助函数。
 * - consumeStartupPromptSkip: 模块内部辅助函数。
 * - absDiffU64: 计算、判断或转换对应结果。
 * - daysFromCivil: 计算、判断或转换对应结果。
 * - appModeToIniValue: 模块内部辅助函数。
 * - return: 模块内部辅助函数。
 */
static bool ensureStaQueueMutex();
static void clearStaMessageQueue();
static bool pushStaMessageQueue(const String &message, bool prioritizeBottleMessage = false);
static bool loadStaCredentialsFromSettingIni(String &outSsid, String &outPassword, String &outNet);
static void ensureStaFetcherTaskStarted();
static void beginStaConnectAttempt();
static bool refreshTodayReminderSlotsFromRtc(bool forceReload, bool markPastTriggered);
static bool parseAppModeFromIniValue(String value, AppLoopMode &outMode);
static const char *appModeToIniValue(AppLoopMode mode);
static bool loadAppModeFromSettingIni(AppLoopMode &outMode);
static bool persistAppModeToSettingIni(AppLoopMode mode);
static void playMessageWithGlitch(const char *text);
void waitWakeKeyReleaseBeforeSleep();
bool saveSleepSnapshotToFat();
void markSleepRtcContextForSleep();
[[noreturn]] void enterDeepSleepNow(uint32_t wakeSec);
bool handleRtcMaintenanceWake();

namespace
{

constexpr uint8_t kBacklightDutyOff = 0;
constexpr int kDefaultBacklightCloseTimeSec = 20;
constexpr int kDefaultSleepTimeMin = 1;
constexpr uint32_t kBacklightTaskTickMs = 100UL;
constexpr uint32_t kBootAnimPollMs = 10UL;
constexpr uint32_t kBootAnimMaxWaitMs = 12000UL;
constexpr gpio_num_t kWakeKeyGpio = GPIO_NUM_2;
constexpr uint32_t kRtcWakeDefaultSec = 60U;
constexpr uint32_t kRtcWakeMaxSec = 30U * 60U;
constexpr uint32_t kRtcMismatchToleranceSec = 2U;
constexpr uint32_t kReminderTriggerWindowSec = 59U;
constexpr size_t kSleepTextMaxLen = 240;
constexpr size_t kSleepPortalQueueMax = 128;
constexpr size_t kSleepStaQueueMax = 20;
constexpr size_t kMaxReminderTimes = 128;
constexpr size_t kScheduleInterruptQueueMax = 64;
constexpr char kSleepSnapshotPath[] = "/sleep_state.bin";
constexpr char kSleepSnapshotTmpPath[] = "/sleep_state.tmp";
constexpr char kUpdateDirPath[] = "/Update";
constexpr char kUpdateDirPathLower[] = "/update";
constexpr char kUpdateFirmwarePath[] = "/Update/firmware.bin";
constexpr char kUpdateAppPath[] = "/Update/app.bin";
constexpr char kUpdateLittleFsPath[] = "/Update/littlefs.bin";
constexpr char kUpdateFsPath[] = "/Update/fs.bin";
constexpr char kUpdateSpiffsPath[] = "/Update/spiffs.bin";
constexpr char kLittleFsBackupDirPath[] = "/Backup";
constexpr const char *kCsvEmptyFallbackMessage = u8"痛苦啊，你是我的唯一...";
constexpr const char *kFatRecoveryNoticeLine1 = u8"你的设备出现了一些问题";
constexpr const char *kFatRecoveryNoticeLine2 = u8"不必担心,我们已经为你恢复到了初始状态";
constexpr const char *kFatRecoveryNoticeLine3 = u8"尽管如此,我们还是建议你去联系你的部门主管";
constexpr const char *kFatRecoveryFailedLine = u8"我们遇到了不可逆转的熔毁，请联系部门主管";
constexpr const char *kDefaultReminderMessage = u8"这个时候你似乎有什么事要干";
constexpr uint32_t kSleepFileMagic = 0x53534E50UL; // "SSNP"
constexpr uint16_t kSleepFileVersion = 1;
constexpr uint32_t kSleepRtcCtxMagic = 0x54534654UL; // "TSFT"
constexpr uint16_t kSleepRtcCtxVersion = 2;

struct SleepRtcContext
{
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t snapshotValid = 0;
  uint8_t appMode = 0;
  uint32_t nextWakeSec = 0;
  uint8_t pendingReminder = 0;
  uint16_t reminderIntervalSec = 0;
  uint16_t reminderTimes = 0;
  uint64_t expectedUnix = 0;
  char reminderMessage[kSleepTextMaxLen + 1] = {0};
};

struct SleepSnapshotHeader
{
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t appMode = 0;
  uint8_t runState = 0;
  uint8_t firstFlag = 0;
  uint8_t staPhase = 0;
  int32_t csvCount = 0;
  uint8_t staRetryCount = 0;
  uint8_t staQueueCount = 0;
  uint8_t regularQueueCount = 0;
  uint8_t hostQueueCount = 0;
  uint8_t hasImmediate = 0;
  int32_t backlightTimeSec = -1;
  char lastDisplayed[kSleepTextMaxLen + 1] = {0};
  char immediateMessage[kSleepTextMaxLen + 1] = {0};
};

RTC_DATA_ATTR SleepRtcContext gSleepRtcCtx;
RTC_DATA_ATTR uint32_t gForceAppUpdateBootTag = 0;
constexpr uint32_t kForceAppUpdateBootMagic = 0x55504454UL; // "UPDT"

enum BacklightState : uint8_t
{
  kBacklightBright = 0,
  kBacklightDim = 1,
  kBacklightOff = 2,
};

TaskHandle_t gBacklightTaskHandle = nullptr;
portMUX_TYPE gBacklightMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t gBacklightLastActivityMs = 0;
BacklightState gBacklightState = kBacklightBright;
TaskHandle_t gBootAnimWaiter = nullptr;
bool gBootAnimRunning = false;
bool gBootAnimUsbDetected = false;
bool gBootAnimErrorHintSuppressed = false;
bool gSimheiFontPreloaded = false;
bool gSettingsPreloadedAtBoot = false;
volatile bool gUpdateRebootRequested = false;
char gLastDisplayedText[kSleepTextMaxLen + 1] = {0};

struct ScheduleInterruptQueueItem
{
  char text[kSleepTextMaxLen + 1] = {0};
  uint16_t intervalSec = 0;
  uint16_t reminderTimes = 0;
  uint16_t reminderCount = 0;
  bool hostMessage = false;
};

struct DailyReminderSlot
{
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t priority = 0;
  uint16_t number = 0;
  uint16_t scheduleIndex = 0;
  bool triggered = false;
};

struct FatRecoveryTarget
{
  const char *name;
  const char *fatPath;
  const char *backupPath;
  bool isDirectory;
};

constexpr FatRecoveryTarget kFatRecoveryTargets[] = {
    {"data.csv", "/data.csv", "/Backup/data.csv", false},
    // Project uses setting.ini (not setting.csv).
    {"setting.ini", "/setting.ini", "/Backup/setting.ini", false},
    {"schedule.csv", "/schedule.csv", "/Backup/schedule.csv", false},
    {"sound", "/sound", "/Backup/sound", true},
};

ScheduleInterruptQueueItem gScheduleInterruptQueue[kScheduleInterruptQueueMax];
size_t gScheduleInterruptCount = 0;
bool gScheduleInterruptActive = false;
bool gScheduleInterruptKeyLatch = false;
bool gScheduleInterruptPendingStart = false;
uint32_t gScheduleInterruptLastPlayMs = 0;
DailyReminderSlot gTodayReminderSlots[kMaxReminderTimes];
size_t gTodayReminderCount = 0;
uint32_t gTodayReminderDateKey = 0;
uint16_t gTodayReminderLastCheckedMinute = 0xFFFFU;
char gPostRecoveryManualMessage[kSleepTextMaxLen + 1] = {0};

void sanitizeMessageForSnapshot(const String &in, char out[kSleepTextMaxLen + 1])
{
  if (!out)
    return;
  String normalized = in;
  normalized.replace("\r", " ");
  normalized.replace("\n", " ");
  normalized.trim();
  if (normalized.length() > kSleepTextMaxLen)
  {
    normalized.remove(kSleepTextMaxLen);
  }
  normalized.toCharArray(out, kSleepTextMaxLen + 1);
  out[kSleepTextMaxLen] = '\0';
}

void rememberLastDisplayedText(const char *text)
{
  if (!text)
    return;
  String normalized = text;
  sanitizeMessageForSnapshot(normalized, gLastDisplayedText);
}

uint8_t reminderRepeatPriority(bool repeatDay, bool repeatMonth, bool repeatWeek, bool repeatYear)
{
  if (!(repeatDay || repeatMonth || repeatWeek || repeatYear))
    return 0; // 不重复
  if (repeatYear)
    return 1;
  if (repeatMonth)
    return 2;
  if (repeatWeek)
    return 3;
  return 4; // 每日重复
}

bool normalizeScheduleQueueMessage(const String &rawText, String &outText)
{
  outText = rawText;
  outText.replace("\r", " ");
  outText.replace("\n", " ");
  outText.trim();
  if (!outText.length())
  {
    outText = kDefaultReminderMessage;
  }
  if (outText.length() > kSleepTextMaxLen)
  {
    outText.remove(kSleepTextMaxLen);
  }
  return outText.length() > 0;
}

#include "StorageRecoveryUpdate.inc"

void clearScheduleInterruptQueue()
{
  gScheduleInterruptCount = 0;
  gScheduleInterruptActive = false;
  gScheduleInterruptKeyLatch = false;
  gScheduleInterruptPendingStart = false;
  gScheduleInterruptLastPlayMs = 0;
  for (size_t i = 0; i < kScheduleInterruptQueueMax; ++i)
  {
    gScheduleInterruptQueue[i].text[0] = '\0';
    gScheduleInterruptQueue[i].intervalSec = 0;
    gScheduleInterruptQueue[i].reminderTimes = 0;
    gScheduleInterruptQueue[i].reminderCount = 0;
    gScheduleInterruptQueue[i].hostMessage = false;
  }
}

bool appendScheduleInterruptQueueItem(const String &rawText, uint16_t intervalSec, uint16_t reminderTimes,
                                      bool hostMessage)
{
  String normalized;
  if (!normalizeScheduleQueueMessage(rawText, normalized))
    return false;
  if (gScheduleInterruptCount >= kScheduleInterruptQueueMax)
    return false;

  ScheduleInterruptQueueItem &item = gScheduleInterruptQueue[gScheduleInterruptCount];
  normalized.toCharArray(item.text, sizeof(item.text));
  item.text[kSleepTextMaxLen] = '\0';
  item.intervalSec = hostMessage ? 0U : intervalSec;
  item.reminderTimes = hostMessage ? 0U : reminderTimes;
  item.reminderCount = 0;
  item.hostMessage = hostMessage;
  ++gScheduleInterruptCount;
  return true;
}

bool enqueueScheduleInterruptMessage(const String &rawText)
{
  return appendScheduleInterruptQueueItem(rawText, 0, 0, false);
}

bool insertHostMessageIntoScheduleInterruptQueueFront(const String &rawText)
{
  String normalized;
  if (!normalizeScheduleQueueMessage(rawText, normalized))
    return false;
  if (gScheduleInterruptCount >= kScheduleInterruptQueueMax)
  {
    if (gScheduleInterruptCount == 0)
      return false;
    --gScheduleInterruptCount; // Drop tail.
  }

  size_t insertPos = gScheduleInterruptActive ? 1U : 0U;
  if (insertPos > gScheduleInterruptCount)
  {
    insertPos = gScheduleInterruptCount;
  }

  for (size_t i = gScheduleInterruptCount; i > insertPos; --i)
  {
    gScheduleInterruptQueue[i] = gScheduleInterruptQueue[i - 1];
  }

  ScheduleInterruptQueueItem &item = gScheduleInterruptQueue[insertPos];
  normalized.toCharArray(item.text, sizeof(item.text));
  item.text[kSleepTextMaxLen] = '\0';
  item.intervalSec = 0;
  item.reminderTimes = 0;
  item.reminderCount = 0;
  item.hostMessage = true;
  ++gScheduleInterruptCount;
  return true;
}

bool popScheduleInterruptMessage(String &outText)
{
  outText = "";
  if (gScheduleInterruptCount == 0)
    return false;
  outText = String(gScheduleInterruptQueue[0].text);
  for (size_t i = 1; i < gScheduleInterruptCount; ++i)
  {
    gScheduleInterruptQueue[i - 1] = gScheduleInterruptQueue[i];
  }
  --gScheduleInterruptCount;
  if (gScheduleInterruptCount == 0)
  {
    gScheduleInterruptActive = false;
    gScheduleInterruptKeyLatch = false;
  }
  outText.trim();
  return outText.length() > 0;
}

bool writeExact(fs::File &f, const void *data, size_t len)
{
  if (!data || len == 0)
    return len == 0;
  return f.write(static_cast<const uint8_t *>(data), len) == len;
}

bool readExact(fs::File &f, void *data, size_t len)
{
  if (!data || len == 0)
    return len == 0;
  return f.read(static_cast<uint8_t *>(data), len) == static_cast<int>(len);
}

void clearSleepRtcContext()
{
  gSleepRtcCtx.magic = 0;
  gSleepRtcCtx.version = 0;
  gSleepRtcCtx.snapshotValid = 0;
  gSleepRtcCtx.appMode = 0;
  gSleepRtcCtx.nextWakeSec = 0;
  gSleepRtcCtx.pendingReminder = 0;
  gSleepRtcCtx.reminderIntervalSec = 0;
  gSleepRtcCtx.reminderTimes = 0;
  gSleepRtcCtx.expectedUnix = 0;
  gSleepRtcCtx.reminderMessage[0] = '\0';
}

bool hasValidSleepRtcContext()
{
  return gSleepRtcCtx.magic == kSleepRtcCtxMagic && gSleepRtcCtx.version == kSleepRtcCtxVersion &&
         gSleepRtcCtx.snapshotValid != 0;
}

float clampUnitFloat(float value)
{
  if (value != value)
    return 1.0f; // NaN fallback
  if (value < 0.0f)
    return 0.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

uint8_t backlightDutyBrightFromLevel(float level)
{
  const float clamped = clampUnitFloat(level);
  const int duty = static_cast<int>(clamped * 255.0f + 0.5f);
  if (duty < 0)
    return 0;
  if (duty > 255)
    return 255;
  return static_cast<uint8_t>(duty);
}

uint8_t backlightDutyDimFromBright(uint8_t brightDuty)
{
  if (brightDuty == 0)
    return 0;
  uint8_t dimDuty = static_cast<uint8_t>(brightDuty / 2);
  if (dimDuty == 0)
    dimDuty = 1;
  return dimDuty;
}

void applyBacklightState(BacklightState state)
{
  const uint8_t brightDuty = backlightDutyBrightFromLevel(gBacklightLevel);
  const uint8_t dimDuty = backlightDutyDimFromBright(brightDuty);
  switch (state)
  {
  case kBacklightBright:
    ledcWrite(0, brightDuty);
    break;
  case kBacklightDim:
    ledcWrite(0, dimDuty);
    break;
  case kBacklightOff:
    ledcWrite(0, kBacklightDutyOff);
    break;
  }
}

bool wakeBacklightByKeyIfNeeded()
{
  bool wakeOnly = false;
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  if (gBacklightState != kBacklightBright)
  {
    gBacklightState = kBacklightBright;
    wakeOnly = true;
  }
  portEXIT_CRITICAL(&gBacklightMux);
  if (wakeOnly)
  {
    applyBacklightState(kBacklightBright);
  }
  return wakeOnly;
}

uint32_t backlightCloseDelayMs()
{
  const int closeSec = (gBacklightCloseTimeSec < 0) ? 0 : gBacklightCloseTimeSec;
  return static_cast<uint32_t>(closeSec) * 1000UL;
}

uint32_t staOnlySleepAfterOffDelayMs()
{
  int minutes = gSleepTimeMin;
  if (minutes < 0)
    minutes = 0;
  uint64_t waitMs = static_cast<uint64_t>(minutes) * 60ULL * 1000ULL;
  if (waitMs > static_cast<uint64_t>(0x7FFFFFFFUL))
  {
    waitMs = static_cast<uint64_t>(0x7FFFFFFFUL);
  }
  return static_cast<uint32_t>(waitMs);
}

bool staOnlySleepTimeoutReached()
{
  int backlightTimeSec = -1;
  uint32_t lastActivity = 0;
  BacklightState stateNow = kBacklightBright;
  portENTER_CRITICAL(&gBacklightMux);
  backlightTimeSec = gBacklightTimeSec;
  lastActivity = gBacklightLastActivityMs;
  stateNow = gBacklightState;
  portEXIT_CRITICAL(&gBacklightMux);

  if (stateNow != kBacklightOff || backlightTimeSec < 0)
    return false;

  const uint32_t dimMs = static_cast<uint32_t>(backlightTimeSec) * 1000UL;
  const uint32_t offAtMs = lastActivity + dimMs + backlightCloseDelayMs();
  const uint32_t nowMs = millis();
  return (nowMs - offAtMs) >= staOnlySleepAfterOffDelayMs();
}

[[noreturn]] void enterStaOnlyDeepSleep()
{
  Serial.printf("[STA_ONLY] backlight off for %d min, entering deep sleep\n", gSleepTimeMin);
  // Stop FAT readers before writing the sleep snapshot; FatFS is not
  // journaled, so avoid concurrent audio reads during metadata updates.
  mixer.stopBG();
  mixer.stopInsert();
  const bool snapshotOk = saveSleepSnapshotToFat();
  if (snapshotOk)
  {
    markSleepRtcContextForSleep();
  }
  else
  {
    Serial.println("[SLEEP] snapshot save failed, fallback to plain sleep");
    clearSleepRtcContext();
  }
  wirelessPortalStop();
  WiFi.mode(WIFI_OFF);
  unmountFat();
  waitWakeKeyReleaseBeforeSleep();
  enterDeepSleepNow(kRtcWakeDefaultSec);
}

void backlightTask(void *param)
{
  (void)param;
  while (true)
  {
    int backlightTimeSec = -1;
    uint32_t lastActivity = 0;
    BacklightState stateNow = kBacklightBright;
    const uint32_t nowMs = millis();

    portENTER_CRITICAL(&gBacklightMux);
    backlightTimeSec = gBacklightTimeSec;
    lastActivity = gBacklightLastActivityMs;
    stateNow = gBacklightState;
    portEXIT_CRITICAL(&gBacklightMux);

    BacklightState desired = kBacklightBright;
    if (backlightTimeSec >= 0)
    {
      const uint32_t dimMs = static_cast<uint32_t>(backlightTimeSec) * 1000UL;
      const uint32_t closeDelayMs = backlightCloseDelayMs();
      const uint32_t elapsedMs = nowMs - lastActivity;
      if (elapsedMs >= dimMs + closeDelayMs)
      {
        desired = kBacklightOff;
      }
      else if (elapsedMs >= dimMs)
      {
        desired = kBacklightDim;
      }
    }

    if (desired != stateNow)
    {
      portENTER_CRITICAL(&gBacklightMux);
      gBacklightState = desired;
      portEXIT_CRITICAL(&gBacklightMux);
      applyBacklightState(desired);
    }

    vTaskDelay(pdMS_TO_TICKS(kBacklightTaskTickMs));
  }
}

void ensureBacklightTaskStarted()
{
  if (gBacklightTaskHandle)
    return;
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  gBacklightState = kBacklightBright;
  portEXIT_CRITICAL(&gBacklightMux);
  applyBacklightState(kBacklightBright);
  xTaskCreatePinnedToCore(backlightTask, "BacklightTask", 4096, nullptr, 1, &gBacklightTaskHandle, 1);
}

} // namespace

namespace
{

constexpr uint8_t kBatteryAdcPin = 1;        // IO1
constexpr float kBatteryDividerRatio = 2.0f; // 47k:47k divider -> VIN = 2 * Vpin
constexpr uint32_t kBatteryUpdateIntervalMs = 30000UL;
constexpr uint8_t kBatterySampleCount = 8;
constexpr uint32_t kBatterySampleGapMs = 2UL;
constexpr float kBatteryFilterAlpha = 0.35f;
constexpr float kBatteryVoltageEmpty = 3.30f;
constexpr float kBatteryVoltageFull = 4.20f;
constexpr float kBatteryChargingDetectVoltage = 4.60f; // VIN around 5V when charging

portMUX_TYPE gBatteryMux = portMUX_INITIALIZER_UNLOCKED;
BatteryStatus gBatteryStatus;
bool gBatteryPinConfigured = false;

float clampBatteryVoltage(float value)
{
  return value < 0.0f ? 0.0f : value;
}

int batteryPercentFromVoltage(float vinVoltage)
{
  if (vinVoltage <= kBatteryVoltageEmpty)
    return 0;
  if (vinVoltage >= kBatteryVoltageFull)
    return 100;
  const float ratio = (vinVoltage - kBatteryVoltageEmpty) / (kBatteryVoltageFull - kBatteryVoltageEmpty);
  const int percent = static_cast<int>(ratio * 100.0f + 0.5f);
  if (percent < 0)
    return 0;
  if (percent > 100)
    return 100;
  return percent;
}

void ensureBatteryPinConfigured()
{
  if (gBatteryPinConfigured)
    return;
  pinMode(kBatteryAdcPin, INPUT);
#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
#if defined(ADC_11db)
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);
#endif
#endif
  // Throw away one reading after configuration for better stability.
  (void)analogRead(kBatteryAdcPin);
#if defined(ARDUINO_ARCH_ESP32)
  (void)analogReadMilliVolts(kBatteryAdcPin);
#endif
  delay(2);
  gBatteryPinConfigured = true;
}

bool sampleBatteryVoltage(float &outPinVoltage, float &outVinVoltage, uint16_t &outRawAdc)
{
  ensureBatteryPinConfigured();

  uint32_t rawSum = 0;
  uint32_t mvSum = 0;
  uint8_t mvCount = 0;

  for (uint8_t i = 0; i < kBatterySampleCount; ++i)
  {
    int raw = analogRead(kBatteryAdcPin);
    if (raw < 0)
      raw = 0;
    rawSum += static_cast<uint32_t>(raw);
#if defined(ARDUINO_ARCH_ESP32)
    const int mv = analogReadMilliVolts(kBatteryAdcPin);
    if (mv > 0)
    {
      mvSum += static_cast<uint32_t>(mv);
      ++mvCount;
    }
#endif
    if (kBatterySampleGapMs)
    {
      delay(kBatterySampleGapMs);
    }
  }

  outRawAdc = static_cast<uint16_t>(rawSum / kBatterySampleCount);

  float pinVoltage = 0.0f;
  if (mvCount > 0)
  {
    pinVoltage = (static_cast<float>(mvSum) / static_cast<float>(mvCount)) / 1000.0f;
  }
  else
  {
    pinVoltage = (static_cast<float>(outRawAdc) / 4095.0f) * 3.3f;
  }

  outPinVoltage = clampBatteryVoltage(pinVoltage);
  outVinVoltage = outPinVoltage * kBatteryDividerRatio;
  return true;
}

} // namespace

void serviceBatteryMonitor(bool force)
{
  BatteryStatus previous;
  portENTER_CRITICAL(&gBatteryMux);
  previous = gBatteryStatus;
  portEXIT_CRITICAL(&gBatteryMux);

  const uint32_t now = millis();
  if (!force && previous.initialized && (now - previous.updatedMs) < kBatteryUpdateIntervalMs)
  {
    return;
  }

  float pinVoltage = 0.0f;
  float vinVoltage = 0.0f;
  uint16_t rawAdc = 0;
  if (!sampleBatteryVoltage(pinVoltage, vinVoltage, rawAdc))
  {
    return;
  }

  const bool charging = vinVoltage >= kBatteryChargingDetectVoltage;
  float filteredVin = vinVoltage;
  if (previous.initialized)
  {
    filteredVin = previous.filteredVinVoltage + (vinVoltage - previous.filteredVinVoltage) * kBatteryFilterAlpha;
  }
  filteredVin = clampBatteryVoltage(filteredVin);

  int percent = batteryPercentFromVoltage(filteredVin);
  if (previous.initialized && !charging && percent > previous.percent)
  {
    // Not charging: ignore any upward jump from sampling noise/load rebound.
    percent = previous.percent;
    filteredVin = previous.filteredVinVoltage;
  }

  BatteryStatus next = previous;
  next.available = true;
  next.initialized = true;
  next.charging = charging;
  next.rawAdc = rawAdc;
  next.pinVoltage = pinVoltage;
  next.vinVoltage = vinVoltage;
  next.filteredVinVoltage = filteredVin;
  next.percent = percent;
  next.updatedMs = now;

  portENTER_CRITICAL(&gBatteryMux);
  gBatteryStatus = next;
  portEXIT_CRITICAL(&gBatteryMux);
}

bool appGetBatteryStatus(BatteryStatus &outStatus)
{
  portENTER_CRITICAL(&gBatteryMux);
  outStatus = gBatteryStatus;
  portEXIT_CRITICAL(&gBatteryMux);
  return outStatus.available && outStatus.initialized;
}

#include "UsbBootFlow.inc"

#include "WebImageView.inc"

static AppLoopMode gAppLoopMode = APP_MODE_AP_STA;
static AppModeEnterCallback gAppModeEnterCallback = nullptr;
static AppModeEnterCallback gAppModeInitCallbacks[3] = {nullptr, nullptr, nullptr};
static bool gAppModeEnterPending = true;
static bool gSkipStartupPromptOnce = true;

static bool consumeStartupPromptSkip(AppLoopMode mode)
{
  if (!gSkipStartupPromptOnce)
    return false;
  gSkipStartupPromptOnce = false;
  Serial.printf("[BOOT] startup mode=%s, skip prompt once\n", appModeToIniValue(mode));
  return true;
}

enum class StaOnlinePhase : uint8_t
{
  kPromptWaitShort = 0,
  kConnecting = 1,
  kFailWaitShort = 2,
  kConnected = 3,
  kDisconnectedWaitShort = 4,
};

static StaOnlinePhase gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
static String gStaNetSsid;
static String gStaNetPassword;
static String gStaNetApi;
static uint8_t gStaRetryCount = 0;
static uint32_t gStaAttemptStartMs = 0;
static constexpr uint32_t kStaAttemptTimeoutMs = 10000UL;
static constexpr uint8_t kStaMaxRetryCount = 5;
static constexpr const char *kStaPromptMsg = u8"模式:联网 | 短按开始连接WiFi";
static constexpr const char *kStaMissingCfgMsg =
    u8"WIFI\u914D\u7F6E\u7F3A\u5931\uFF1A\u77ED\u6309\u5207\u6362\u6A21\u5F0F";
static constexpr const char *kStaConnectingPrefix = u8"\u6B63\u5728\u8FDE\u63A5\uFF1A";
static constexpr const char *kStaRetryPrefix = u8"|重试次数";
static constexpr const char *kStaConnectOkPrefix = u8"WiFi连接成功 | 配置网址         http://";
static constexpr const char *kStaConnectFailMsg =
    u8"WIFI\u8FDE\u63A5\u5931\u8D25\uFF1A\u77ED\u6309\u5207\u6362\u6A21\u5F0F";
static constexpr const char *kStaCloudQueueEmptyMsg = u8"正在连接都市神经网络...";
static constexpr const char *kStaDisconnectedMsg = u8"WIFI已断开，按键重新连接";

static constexpr const char *kApPromptMsg = u8"模式：正常 | 热点已启动";

static constexpr const char *kStaonlyPromptMsg = u8"模式：省电 | 无线功能已禁用";

static constexpr const char *kStaCloudApiUrlDefault = "http://115.190.145.254:8080/random";
static constexpr size_t kStaPrefetchDepth = 20;
static_assert(kStaPrefetchDepth == kSleepStaQueueMax, "kSleepStaQueueMax must match kStaPrefetchDepth");
static constexpr uint32_t kStaQueueEmptyHintCooldownMs = 1800UL;
static constexpr uint32_t kStaFetchFailCooldownMs = 1000UL;
static constexpr uint32_t kStaFetcherTickMs = 200UL;
static constexpr uint32_t kStaHttpConnectTimeoutMs = 3500UL;
static constexpr uint32_t kStaHttpReadTimeoutMs = 4500UL;
static constexpr uint32_t kStaNtpPollTimeoutMs = 6000UL;
static constexpr uint32_t kStaNtpAttemptCooldownMs = 10000UL;
static constexpr int32_t kStaNtpGmtOffsetSec = 8 * 3600;
static constexpr const char *kStaNtpServer1 = "ntp.ntsc.ac.cn";
static constexpr const char *kStaNtpServer2 = "cn.pool.ntp.org";
static constexpr const char *kStaNtpServer3 = "pool.ntp.org";
static constexpr UBaseType_t kStaFetcherPriority = 1;
static constexpr BaseType_t kStaFetcherCore = 0;
static constexpr uint32_t kStaFetcherStackSize = 12288UL;
static String gStaMsgQueue[kStaPrefetchDepth];
static size_t gStaMsgQueueHead = 0;
static size_t gStaMsgQueueSize = 0;
static uint32_t gStaLastQueueEmptyHintMs = 0;
static uint32_t gStaNextFetchAllowedMs = 0;
static volatile bool gStaFetchInProgress = false;
static WiFiClientSecure gStaHttpsClient;
static bool gStaHttpsClientReady = false;
static SemaphoreHandle_t gStaMsgQueueMutex = nullptr;
static TaskHandle_t gStaFetcherTaskHandle = nullptr;
static bool gStaNtpSyncedThisSession = false;
static uint32_t gStaNtpLastAttemptMs = 0;
static bool gStaBottlePriorityActive = false;
static size_t gStaBottlePriorityNextLogical = 0;

struct SleepSnapshotData
{
  SleepSnapshotHeader header;
  std::vector<String> staQueue;
  std::vector<String> regularQueue;
  std::vector<String> hostQueue;
};

uint64_t absDiffU64(uint64_t a, uint64_t b)
{
  return (a >= b) ? (a - b) : (b - a);
}

int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
  year -= (month <= 2U) ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int monthAdj = static_cast<int>(month) + ((month > 2U) ? -3 : 9);
  const unsigned doy = static_cast<unsigned>((153 * monthAdj + 2) / 5 + static_cast<int>(day) - 1);
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

#include "ScheduleService.inc"

#include "SleepService.inc"
