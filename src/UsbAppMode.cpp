/*
 * UsbAppMode aggregation translation unit.
 *
 * The private implementation is split across src/usb_app/*.inc.  These files
 * are intentionally included here so PlatformIO still compiles one translation
 * unit while the implementation remains readable by subsystem.
 */
#include "UsbAppMode.h"
#include "AppGlobals.h"
#include "DeviceUuid.h"
#include "DisplayEffects.h"
#include "Ds1302Rtc.h"
#include "Index_B.h"
#include "Key_Drv.h"
#include "WirelessPortal.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <Arduino.h>
#include <FFat.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <USB.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <ctype.h>
#include <esp_ota_ops.h>
#include <limits>
#include <time.h>
#include <vector>

#include "usb_app/CoreState.inc"
#include "usb_app/StaCloudConfigService.inc"
#include "usb_app/AppModeRouting.inc"
#include "usb_app/AppLoopController.inc"

void onApStaInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  // Force a clean restart so switching back from STA-only always re-creates AP.
  wirelessPortalStop();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaNetSsid = "";
  gStaNetPassword = "";
  gStaNetApi = kStaCloudApiUrlDefault;
  clearStaMessageQueue();
  if (!wirelessPortalStart())
  {
    Serial.println("[AP] wirelessPortalStart failed on AP init");
  }
  if (!consumeStartupPromptSkip(mode))
  {
    playAPMessage(kApPromptMsg);
  }
}

void onStaOnlineInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  if (!wirelessPortalStart())
  {
    Serial.println("[STA] wirelessPortalStart failed on STA_Online init");
  }
  WiFi.disconnect(true, false);

  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  clearStaMessageQueue();

  const bool loaded = loadStaCredentialsFromSettingIni(gStaNetSsid, gStaNetPassword, gStaNetApi);
  if (!loaded)
  {
    Serial.println("[STA] NetSSID missing in /setting.ini");
  }
  else
  {
    Serial.printf("[STA] target ssid: %s\\n", gStaNetSsid.c_str());
  }
  Serial.printf("[STA] target net: %s\\n", gStaNetApi.c_str());

  ensureStaFetcherTaskStarted();
  if (consumeStartupPromptSkip(mode))
  {
    beginStaConnectAttempt();
  }
  else
  {
    playStaMessage(kStaPromptMsg);
  }
}

void onStaOnlyInit(AppLoopMode mode)
{
  waitStaFetcherIdle(1000);
  resetStaHttpClient();
  wirelessPortalStop();
  gStaOnlinePhase = StaOnlinePhase::kPromptWaitShort;
  gStaRetryCount = 0;
  gStaAttemptStartMs = 0;
  gStaLastQueueEmptyHintMs = 0;
  gStaNextFetchAllowedMs = 0;
  gStaFetchInProgress = false;
  gStaNtpSyncedThisSession = false;
  gStaNtpLastAttemptMs = 0;
  gStaNetApi = kStaCloudApiUrlDefault;
  clearStaMessageQueue();
  WiFi.disconnect(true, false);
  if (!wirelessPortalStartEspNowOnly())
  {
    Serial.println("[STA_ONLY] wirelessPortalStartEspNowOnly failed");
  }
  if (!consumeStartupPromptSkip(mode))
  {
    firstFlag = false;
    get_Keycode();
    playStaOnlyMessage(kStaonlyPromptMsg);
  }
}

bool appHandleRtcMaintenanceWakeIfNeeded()
{
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
  {
    return false;
  }
  if (!hasValidSleepRtcContext())
  {
    return false;
  }
  Serial.println("[SLEEP] timer wake detected, run RTC maintenance");
  const bool shouldContinueBoot = handleRtcMaintenanceWake();
  return !shouldContinueBoot;
}

bool appShouldFastResumeFromDeepSleep()
{
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0)
  {
    return hasValidSleepRtcContext();
  }
  if (cause == ESP_SLEEP_WAKEUP_TIMER)
  {
    return hasValidSleepRtcContext() && (gSleepRtcCtx.pendingReminder != 0);
  }
  return false;
}

bool appRestoreFromDeepSleepSnapshot()
{
  if (!appShouldFastResumeFromDeepSleep())
  {
    return false;
  }
  const bool timerReminderWake =
      (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) && (gSleepRtcCtx.pendingReminder != 0);
  char reminderMessage[kSleepTextMaxLen + 1] = {0};
  uint16_t reminderIntervalSec = 0;
  uint16_t reminderTimes = 0;
  if (timerReminderWake)
  {
    memcpy(reminderMessage, gSleepRtcCtx.reminderMessage, sizeof(reminderMessage));
    reminderMessage[sizeof(reminderMessage) - 1] = '\0';
    reminderIntervalSec = gSleepRtcCtx.reminderIntervalSec;
    reminderTimes = gSleepRtcCtx.reminderTimes;
    if (!reminderMessage[0])
    {
      sanitizeMessageForSnapshot(String(kDefaultReminderMessage), reminderMessage);
    }
  }

  SleepSnapshotData snapshot;
  if (!loadSleepSnapshotFromFat(snapshot))
  {
    Serial.println("[SLEEP] snapshot not found or invalid, fallback normal app boot");
    clearSleepRtcContext();
    return false;
  }
  if (!applySleepSnapshot(snapshot, !timerReminderWake))
  {
    Serial.println("[SLEEP] snapshot apply failed, fallback normal app boot");
    clearSleepRtcContext();
    return false;
  }
  gSkipStartupPromptOnce = false;

  if (timerReminderWake)
  {
    (void)markCurrentMinuteScheduleAsTriggeredFromRtc();
    clearScheduleInterruptQueue();
    if (appendScheduleInterruptQueueItem(String(reminderMessage), reminderIntervalSec, reminderTimes, false))
    {
      // Keep legacy behavior: timer reminder wake should play immediately
      // without waiting for processAppLoop state gates.
      gScheduleInterruptPendingStart = false;
      gScheduleInterruptActive = true;
      gScheduleInterruptKeyLatch = false;
      gScheduleInterruptQueue[0].reminderCount = 0;
      playMessageWithGlitch(gScheduleInterruptQueue[0].text);
      gScheduleInterruptLastPlayMs = millis();
      Serial.printf("[SLEEP] reminder played immediately and queued (interval=%u, times=%u)\n",
                    static_cast<unsigned int>(reminderIntervalSec), static_cast<unsigned int>(reminderTimes));
    }
    else
    {
      Serial.println("[SLEEP] reminder queue restore failed, fallback single play");
      playMessageWithGlitch(reminderMessage);
    }
  }

  (void)FFat.remove(kSleepSnapshotPath);
  clearSleepRtcContext();
  Serial.println("[SLEEP] snapshot restored");
  return true;
}
