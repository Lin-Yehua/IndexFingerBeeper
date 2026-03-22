#include "Esp32S3UsbFatMsc_v3.h"

#include <Arduino.h>
#include <USB.h>
#include <FFat.h>
#include <stdarg.h>

extern "C" {
#include <esp_partition.h>
}

Esp32S3UsbFatMsc *Esp32S3UsbFatMsc::s_instance = nullptr;

Esp32S3UsbFatMsc::Config::Config()
  : fatPartitionLabel("ffat"),
    fatMountPoint("/ffat"),
    formatFatOnFail(true),
    autoStartUsb(true),
    exposeFatOnBoot(true),
    remountOnUsbStopped(false),
    remountOnEject(true),
    restartAfterRelease(false),
    verboseLogs(true),
    maxOpenFiles(10),
    vendorId("ESP32"),
    productId("GEN4_FAT"),
    productRevision("1.0") {}

Esp32S3UsbFatMsc::Esp32S3UsbFatMsc() {}

void Esp32S3UsbFatMsc::logf(const char *fmt, ...) const {
  if (!_cfg.verboseLogs) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  Serial.print("[UsbFatMsc] ");
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  Serial.print(buf);
  Serial.println();
  va_end(args);
}

bool Esp32S3UsbFatMsc::begin() {
  Config cfg;
  return begin(cfg);
}

bool Esp32S3UsbFatMsc::begin(const Config &cfg) {
  _cfg = cfg;

  if (s_instance != nullptr && s_instance != this) {
    Serial.println("[UsbFatMsc] Only one instance is supported.");
    return false;
  }
  s_instance = this;

  _pendingRemount = false;
  _pendingRestart = false;
  _usbReady = false;
  _fatMounted = false;
  _transferMode = false;
  _hostActive = false;
  _usbState = UsbState::NotStarted;
  _sectorCount = 0;
  _sectorSize = 512;
  _capacityBytes = 0;

  if (!ensureFatFilesystem()) {
    Serial.println("[UsbFatMsc] FAT partition init failed.");
    return false;
  }

  if (!openRawFatBackend()) {
    Serial.println("[UsbFatMsc] FAT backend open failed.");
    return false;
  }

  if (_sectorCount == 0 || _sectorSize == 0) {
    Serial.println("[UsbFatMsc] Invalid MSC capacity.");
    closeRawFatBackend();
    return false;
  }

  _msc.vendorID(_cfg.vendorId);
  _msc.productID(_cfg.productId);
  _msc.productRevision(_cfg.productRevision);
  _msc.onRead(onReadThunk);
  _msc.onWrite(onWriteThunk);
  _msc.onStartStop(onStartStopThunk);
  _msc.mediaPresent(false);

  logf("MSC begin block_count=%lu block_size=%u capacity=%llu bytes", (unsigned long)_sectorCount, _sectorSize, _capacityBytes);

  if (!_msc.begin(_sectorCount, _sectorSize)) {
    Serial.println("[UsbFatMsc] USBMSC begin failed.");
    closeRawFatBackend();
    return false;
  }

  USB.onEvent(usbEventThunk);

  if (_cfg.autoStartUsb) {
    if (!USB.begin()) {
      Serial.println("[UsbFatMsc] USB begin failed.");
      _msc.end();
      closeRawFatBackend();
      return false;
    }
    _usbReady = true;
  }

  if (_cfg.exposeFatOnBoot) {
    _msc.mediaPresent(true);
    _transferMode = true;
    notifyTransferStart();
    logf("mediaPresent(true)");
  } else {
    _msc.mediaPresent(false);
    closeRawFatBackend();
    if (!remountFat()) {
      Serial.println("[UsbFatMsc] FAT mount for app failed.");
      return false;
    }
  }

  return true;
}

void Esp32S3UsbFatMsc::loop() {
  applyPendingReleaseAction();
}

void Esp32S3UsbFatMsc::end() {
  _msc.mediaPresent(false);
  _transferMode = false;
  _hostActive = false;
  closeRawFatBackend();
  if (!_fatMounted) {
    remountFat();
  }
  _msc.end();
  _usbReady = false;
  _usbState = UsbState::NotStarted;
  notifyUsbState();
}

bool Esp32S3UsbFatMsc::startTransferMode() {
  if (_transferMode) {
    return true;
  }

  if (!unmountFat()) {
    return false;
  }

  if (!openRawFatBackend()) {
    return false;
  }

  _msc.mediaPresent(true);
  _transferMode = true;
  logf("mediaPresent(true)");
  notifyTransferStart();
  return true;
}

bool Esp32S3UsbFatMsc::stopTransferMode(bool remountFatAfterStop) {
  if (_transferMode) {
    _msc.mediaPresent(false);
    logf("mediaPresent(false)");
    _transferMode = false;
    _hostActive = false;
    closeRawFatBackend();
    notifyTransferStop();
  }

  if (remountFatAfterStop) {
    return remountFat();
  }

  return true;
}

bool Esp32S3UsbFatMsc::remountFat() {
  if (_fatMounted) {
    return true;
  }

  if (FFat.begin(_cfg.formatFatOnFail, _cfg.fatMountPoint, _cfg.maxOpenFiles, _cfg.fatPartitionLabel)) {
    _fatMounted = true;
    logf("FFat mounted for app on %s label=%s", _cfg.fatMountPoint, _cfg.fatPartitionLabel);
    notifyFatMounted();
    return true;
  }

  Serial.println("[UsbFatMsc] FFat.begin failed.");
  return false;
}

bool Esp32S3UsbFatMsc::unmountFat() {
  if (!_fatMounted) {
    return true;
  }

  FFat.end();
  _fatMounted = false;
  logf("FFat unmounted from app");
  return true;
}

bool Esp32S3UsbFatMsc::isFatMounted() const { return _fatMounted; }
bool Esp32S3UsbFatMsc::isInTransferMode() const { return _transferMode; }
bool Esp32S3UsbFatMsc::isUsbReady() const { return _usbReady; }
bool Esp32S3UsbFatMsc::isHostActive() const { return _hostActive; }
Esp32S3UsbFatMsc::UsbState Esp32S3UsbFatMsc::usbState() const { return _usbState; }

bool Esp32S3UsbFatMsc::fatExists(const char *path) {
  if (!_fatMounted) return false;
  return FFat.exists(path);
}

File Esp32S3UsbFatMsc::openFat(const char *path, const char *mode) {
  if (!_fatMounted) return File();
  return FFat.open(path, mode);
}

bool Esp32S3UsbFatMsc::readTextFile(const char *path, String &out) {
  out = "";
  File f = openFat(path, FILE_READ);
  if (!f || f.isDirectory()) return false;
  while (f.available()) out += char(f.read());
  f.close();
  return true;
}

const char *Esp32S3UsbFatMsc::fatMountPoint() const { return _cfg.fatMountPoint; }
const char *Esp32S3UsbFatMsc::fatPartitionLabel() const { return _cfg.fatPartitionLabel; }
uint32_t Esp32S3UsbFatMsc::sectorCount() const { return _sectorCount; }
uint16_t Esp32S3UsbFatMsc::sectorSize() const { return _sectorSize; }
uint64_t Esp32S3UsbFatMsc::capacityBytes() const { return _capacityBytes; }

void Esp32S3UsbFatMsc::onTransferStart(StateCallback cb) { _onTransferStart = cb; }
void Esp32S3UsbFatMsc::onTransferStop(StateCallback cb) { _onTransferStop = cb; }
void Esp32S3UsbFatMsc::onFatMounted(StateCallback cb) { _onFatMounted = cb; }
void Esp32S3UsbFatMsc::onUsbState(UsbCallback cb) { _onUsbState = cb; }

void Esp32S3UsbFatMsc::usbEventThunk(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg;
  if (s_instance != nullptr) s_instance->handleUsbEvent(event_base, event_id, event_data);
}

int32_t Esp32S3UsbFatMsc::onReadThunk(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (s_instance == nullptr) return -1;
  return s_instance->onRead(lba, offset, buffer, bufsize);
}

int32_t Esp32S3UsbFatMsc::onWriteThunk(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (s_instance == nullptr) return -1;
  return s_instance->onWrite(lba, offset, buffer, bufsize);
}

bool Esp32S3UsbFatMsc::onStartStopThunk(uint8_t power_condition, bool start, bool load_eject) {
  if (s_instance == nullptr) return false;
  return s_instance->onStartStop(power_condition, start, load_eject);
}

void Esp32S3UsbFatMsc::handleUsbEvent(esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base != ARDUINO_USB_EVENTS) return;
  (void)event_data;

  switch (event_id) {
    case ARDUINO_USB_STARTED_EVENT:
      _usbState = UsbState::Started;
      _usbReady = true;
      break;
    case ARDUINO_USB_RESUME_EVENT:
      _usbState = UsbState::Resumed;
      _hostActive = true;
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      _usbState = UsbState::Suspended;
      _hostActive = false;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      _usbState = UsbState::Stopped;
      _hostActive = false;
      // STOPPED on some Arduino-ESP32 / ESP32-S3 builds can be noisy.
      // Do not auto-remount by default unless the user explicitly enables it.
      if (_transferMode && _cfg.remountOnUsbStopped) {
        if (_cfg.restartAfterRelease) _pendingRestart = true;
        else _pendingRemount = true;
      }
      break;
    default:
      return;
  }

  notifyUsbState();
}

int32_t Esp32S3UsbFatMsc::onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (_wlHandleRaw == WL_INVALID_HANDLE) return -1;
  const size_t addr = (static_cast<size_t>(lba) * _sectorSize) + offset;
  if (wl_read(_wlHandleRaw, addr, buffer, bufsize) != ESP_OK) return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t Esp32S3UsbFatMsc::onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (_wlHandleRaw == WL_INVALID_HANDLE) return -1;
  const size_t addr = (static_cast<size_t>(lba) * _sectorSize) + offset;
  if (wl_write(_wlHandleRaw, addr, buffer, bufsize) != ESP_OK) return -1;
  return static_cast<int32_t>(bufsize);
}

bool Esp32S3UsbFatMsc::onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;

  if (start) {
    _hostActive = true;
    logf("StartStop start=1 eject=%d", (int)load_eject);
    return true;
  }

  _hostActive = false;
  logf("StartStop start=0 eject=%d", (int)load_eject);

  if (load_eject) {
    if (_cfg.restartAfterRelease) _pendingRestart = true;
    else if (_cfg.remountOnEject) _pendingRemount = true;
  }

  return true;
}

bool Esp32S3UsbFatMsc::ensureFatFilesystem() {
  if (!FFat.begin(_cfg.formatFatOnFail, _cfg.fatMountPoint, _cfg.maxOpenFiles, _cfg.fatPartitionLabel)) {
    return false;
  }
  _fatMounted = true;
  logf("FFat sanity mount OK on %s label=%s", _cfg.fatMountPoint, _cfg.fatPartitionLabel);
  FFat.end();
  _fatMounted = false;
  return true;
}

bool Esp32S3UsbFatMsc::openRawFatBackend() {
  if (_wlHandleRaw != WL_INVALID_HANDLE) {
    return true;
  }

  const esp_partition_t *part = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_DATA_FAT,
    _cfg.fatPartitionLabel
  );

  if (part == nullptr) {
    Serial.printf("[UsbFatMsc] FAT partition '%s' not found.\n", _cfg.fatPartitionLabel);
    return false;
  }

  if (wl_mount(part, &_wlHandleRaw) != ESP_OK) {
    Serial.println("[UsbFatMsc] wl_mount failed.");
    _wlHandleRaw = WL_INVALID_HANDLE;
    return false;
  }

  _sectorSize = wl_sector_size(_wlHandleRaw);
  _capacityBytes = wl_size(_wlHandleRaw);
  _sectorCount = (_sectorSize == 0) ? 0 : static_cast<uint32_t>(_capacityBytes / _sectorSize);

  logf("raw backend opened: wl_sector_size=%u wl_size=%llu sector_count=%lu",
       _sectorSize, _capacityBytes, (unsigned long)_sectorCount);

  if (_sectorSize == 0 || _sectorCount == 0) {
    closeRawFatBackend();
    return false;
  }

  return true;
}

void Esp32S3UsbFatMsc::closeRawFatBackend() {
  if (_wlHandleRaw == WL_INVALID_HANDLE) return;
  wl_unmount(_wlHandleRaw);
  _wlHandleRaw = WL_INVALID_HANDLE;
}

void Esp32S3UsbFatMsc::applyPendingReleaseAction() {
  if (_pendingRestart) {
    _pendingRestart = false;
    stopTransferMode(false);
    delay(50);
    ESP.restart();
    return;
  }

  if (_pendingRemount) {
    _pendingRemount = false;
    stopTransferMode(true);
    return;
  }
}

void Esp32S3UsbFatMsc::notifyTransferStart() { if (_onTransferStart) _onTransferStart(*this); }
void Esp32S3UsbFatMsc::notifyTransferStop() { if (_onTransferStop) _onTransferStop(*this); }
void Esp32S3UsbFatMsc::notifyFatMounted() { if (_onFatMounted) _onFatMounted(*this); }
void Esp32S3UsbFatMsc::notifyUsbState() { if (_onUsbState) _onUsbState(*this, _usbState); }
