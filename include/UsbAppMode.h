#pragma once

#include <Arduino.h>

enum AppLoopMode : uint8_t {
  APP_MODE_AP_STA = 0,
  APP_MODE_STA_ONLINE = 1,
  APP_MODE_STA_ONLY = 2,
};

typedef void (*AppModeEnterCallback)(AppLoopMode mode);

void ensureDisplayReady();
void showUsbModeScreen();
void applyAudioGainsFromSettingIni();

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
bool onStartStop(uint8_t power_condition, bool start, bool load_eject);
void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

void onApStaInit(AppLoopMode mode);
void onStaOnlineInit(AppLoopMode mode);
void onStaOnlyInit(AppLoopMode mode);

bool mountFat();
void unmountFat();
bool openRawBackend();
void closeRawBackend();
bool enterUsbMode();
bool enterAppMode();
void applyPendingFatUpdatesFromUpdateDir();
bool appConsumeUpdateRebootRequest();
bool appConsumeForceAppUpdateBoot();

bool initProjectResources();
void processAppLoop();
void setAppModeEnterCallback(AppModeEnterCallback callback);
void setAppModeInitCallback(AppLoopMode mode, AppModeEnterCallback callback);
AppLoopMode getAppLoopMode();
void applyStartupModeFromSettingIni();
void runBootAnimationTaskStart();
void runBootAnimationTaskWait();
void runBootAnimationTaskAndWait();

void notifyBacklightActivity();
void setBacklightTimeSeconds(int seconds);
void setBacklightLevel(float level);

struct BatteryStatus {
  bool available = false;
  bool initialized = false;
  bool charging = false;
  uint16_t rawAdc = 0;
  float pinVoltage = 0.0f;
  float vinVoltage = 0.0f;
  float filteredVinVoltage = 0.0f;
  int percent = 0;
  uint32_t updatedMs = 0;
};

void serviceBatteryMonitor(bool force = false);
bool appGetBatteryStatus(BatteryStatus &outStatus);

bool appHandleRtcMaintenanceWakeIfNeeded();
bool appShouldFastResumeFromDeepSleep();
bool appRestoreFromDeepSleepSnapshot();
