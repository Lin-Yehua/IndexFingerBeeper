#pragma once

#include <Arduino.h>
#include <FS.h>
#include <USBMSC.h>
#include <wear_levelling.h>

class Esp32S3UsbFatMsc
{
public:
  enum class UsbState
  {
    NotStarted,
    Started,
    Resumed,
    Suspended,
    Stopped
  };

  struct Config
  {
    const char *fatPartitionLabel;
    const char *fatMountPoint;
    bool formatFatOnFail;
    bool autoStartUsb;
    bool exposeFatOnBoot;
    bool remountOnUsbStopped;
    bool remountOnEject;
    bool restartAfterRelease;
    bool verboseLogs;
    uint8_t maxOpenFiles;
    const char *vendorId;
    const char *productId;
    const char *productRevision;

    Config();
  };

  typedef void (*StateCallback)(Esp32S3UsbFatMsc &mgr);
  typedef void (*UsbCallback)(Esp32S3UsbFatMsc &mgr, UsbState state);

  Esp32S3UsbFatMsc();

  bool begin();
  bool begin(const Config &cfg);
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
  uint64_t capacityBytes() const;

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
  void logf(const char *fmt, ...) const;

  void notifyTransferStart();
  void notifyTransferStop();
  void notifyFatMounted();
  void notifyUsbState();

  Config _cfg;
  USBMSC _msc;
  wl_handle_t _wlHandleRaw = WL_INVALID_HANDLE;
  uint32_t _sectorCount = 0;
  uint16_t _sectorSize = 512;
  uint64_t _capacityBytes = 0;

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
