/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - begin: 初始化或确保对应资源可用。
 * - loop: Arduino 主循环，周期处理运行任务。
 * - end: 模块内部辅助函数。
 * - startTransferMode: 模块内部辅助函数。
 * - stopTransferMode: 停止、释放或清理对应状态。
 * - remountFat: 模块内部辅助函数。
 * - unmountFat: 挂载或卸载对应文件系统。
 * - fatExists: 模块内部辅助函数。
 * - readTextFile: 读取、获取或消费对应数据。
 * - onTransferStart: 事件回调处理函数。
 * - onTransferStop: 事件回调处理函数。
 * - onFatMounted: 事件回调处理函数。
 * - onUsbState: 事件回调处理函数。
 * - usbEventThunk: 模块内部辅助函数。
 * - onReadThunk: 事件回调处理函数。
 * - onWriteThunk: 事件回调处理函数。
 * - onStartStopThunk: 事件回调处理函数。
 * - handleUsbEvent: 处理对应业务流程或事件分支。
 * - onRead: 事件回调处理函数。
 * - onWrite: 事件回调处理函数。
 * - onStartStop: 事件回调处理函数。
 * - ensureFatFilesystem: 初始化或确保对应资源可用。
 * - openRawFatBackend: 初始化或确保对应资源可用。
 * - closeRawFatBackend: 停止、释放或清理对应状态。
 * - applyPendingReleaseAction: 应用配置或切换运行状态。
 * - notifyTransferStart: 模块内部辅助函数。
 * - notifyTransferStop: 模块内部辅助函数。
 * - notifyFatMounted: 模块内部辅助函数。
 * - notifyUsbState: 模块内部辅助函数。
 * - Config: 模块内部辅助函数。
 */
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <USBMSC.h>
#include <wear_levelling.h>

class Esp32S3UsbFatMsc
{
public:
  enum class ReleaseAction
  {
    None,
    RemountFat,
    Restart
  };

  enum class UsbState
  {
    NotStarted,
    Started,
    Resumed,
    Suspended,
    Stopped
  };

  class Config
  {
  public:
    const char *fatPartitionLabel;
    const char *fatMountPoint;
    bool formatFatOnFail;
    bool autoStartUsb;
    bool exposeFatOnBoot;
    bool remountOnUsbStopped;
    bool remountOnEject;
    bool restartAfterRelease;
    uint8_t maxOpenFiles;
    const char *vendorId;
    const char *productId;
    const char *productRevision;

    Config()
        : fatPartitionLabel("ffat"), fatMountPoint("/ffat"), formatFatOnFail(true), autoStartUsb(true),
          exposeFatOnBoot(true), remountOnUsbStopped(true), remountOnEject(true), restartAfterRelease(false),
          maxOpenFiles(10), vendorId("ESP32"), productId("S3_FAT_DISK"), productRevision("1.0")
    {
    }
  };

  typedef void (*StateCallback)(Esp32S3UsbFatMsc &mgr);
  typedef void (*UsbCallback)(Esp32S3UsbFatMsc &mgr, UsbState state);

  Esp32S3UsbFatMsc();

  bool begin(const Config &cfg = Config());
  void loop();
  void end();

  bool startTransferMode();
  bool stopTransferMode(bool remountFat = true);
  bool remountFat();
  bool unmountFat();

  bool isFatMounted() const;
  bool isInTransferMode() const;
  bool isUsbReady() const;
  bool isHostActive() const;
  UsbState usbState() const;

  bool fatExists(const char *path);
  File openFat(const char *path, const char *mode = FILE_READ);
  bool readTextFile(const char *path, String &out);

  const char *fatMountPoint() const;
  const char *fatPartitionLabel() const;
  uint32_t sectorCount() const;
  uint16_t sectorSize() const;

  void onTransferStart(StateCallback cb);
  void onTransferStop(StateCallback cb);
  void onFatMounted(StateCallback cb);
  void onUsbState(UsbCallback cb);

private:
  static Esp32S3UsbFatMsc *s_instance;

  static void usbEventThunk(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
  static int32_t onReadThunk(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
  static int32_t onWriteThunk(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
  static bool onStartStopThunk(uint8_t power_condition, bool start, bool load_eject);

  void handleUsbEvent(esp_event_base_t event_base, int32_t event_id, void *event_data);
  int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
  int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
  bool onStartStop(uint8_t power_condition, bool start, bool load_eject);

  bool ensureFatFilesystem();
  bool openRawFatBackend();
  void closeRawFatBackend();
  void applyPendingReleaseAction();

  void notifyTransferStart();
  void notifyTransferStop();
  void notifyFatMounted();
  void notifyUsbState();

  Config _cfg;
  USBMSC _msc;
  wl_handle_t _wlHandleRaw = WL_INVALID_HANDLE;
  uint32_t _sectorCount = 0;
  uint16_t _sectorSize = 512;

  volatile bool _pendingRelease = false;
  volatile bool _pendingRemount = false;
  volatile bool _pendingRestart = false;

  bool _usbReady = false;
  bool _fatMounted = false;
  bool _transferMode = false;
  bool _hostActive = false;
  UsbState _usbState = UsbState::NotStarted;

  StateCallback _onTransferStart = nullptr;
  StateCallback _onTransferStop = nullptr;
  StateCallback _onFatMounted = nullptr;
  UsbCallback _onUsbState = nullptr;
};
