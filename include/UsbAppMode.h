/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - ensureDisplayReady: 初始化或确保对应资源可用。
 * - showUsbModeScreen: 绘制界面、输出内容或响应请求。
 * - applyAudioGainsFromSettingIni: 应用配置或切换运行状态。
 * - onRead: 事件回调处理函数。
 * - onWrite: 事件回调处理函数。
 * - onStartStop: 事件回调处理函数。
 * - onUsbEvent: 事件回调处理函数。
 * - onApStaInit: 事件回调处理函数。
 * - onStaOnlineInit: 事件回调处理函数。
 * - onStaOnlyInit: 事件回调处理函数。
 * - mountFat: 挂载或卸载对应文件系统。
 * - unmountFat: 挂载或卸载对应文件系统。
 * - openRawBackend: 初始化或确保对应资源可用。
 * - closeRawBackend: 停止、释放或清理对应状态。
 * - enterUsbMode: 模块内部辅助函数。
 * - enterAppMode: 模块内部辅助函数。
 * - applyPendingFatUpdatesFromUpdateDir: 应用配置或切换运行状态。
 * - appConsumeUpdateRebootRequest: 模块内部辅助函数。
 * - appConsumeForceAppUpdateBoot: 模块内部辅助函数。
 * - initProjectResources: 初始化或确保对应资源可用。
 * - processAppLoop: 执行 APP 主循环调度。
 * - setAppModeEnterCallback: 保存、写入或更新对应数据。
 * - setAppModeInitCallback: 保存、写入或更新对应数据。
 * - getAppLoopMode: 读取、获取或消费对应数据。
 * - applyStartupModeFromSettingIni: 应用配置或切换运行状态。
 * - runBootAnimationTaskStart: FreeRTOS 任务入口或任务控制函数。
 * - runBootAnimationTaskWait: FreeRTOS 任务入口或任务控制函数。
 * - runBootAnimationTaskAndWait: FreeRTOS 任务入口或任务控制函数。
 * - notifyBacklightActivity: 模块内部辅助函数。
 * - setBacklightTimeSeconds: 保存、写入或更新对应数据。
 * - setBacklightLevel: 保存、写入或更新对应数据。
 * - serviceBatteryMonitor: 周期服务函数，维护对应后台状态。
 * - appGetBatteryStatus: 模块内部辅助函数。
 * - appHandleRtcMaintenanceWakeIfNeeded: 模块内部辅助函数。
 * - appShouldFastResumeFromDeepSleep: 模块内部辅助函数。
 * - appRestoreFromDeepSleepSnapshot: 模块内部辅助函数。
 */
#pragma once

#include <Arduino.h>

enum AppLoopMode : uint8_t
{
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

struct BatteryStatus
{
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
