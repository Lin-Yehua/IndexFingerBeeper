#include <Arduino.h>
#include <FFat.h>
#include <USB.h>
#include <USBMSC.h>
#include <vector>

#include "HostWirelessPortal.h"
#include "esp_system.h"

extern "C"
{
#include "esp_partition.h"
#include "wear_levelling.h"
}

namespace
{

constexpr char kFatPartitionLabel[] = "fatfs";
constexpr char kFatMountPoint[] = "/fat";

USBMSC gMsc;
wl_handle_t gWlHandle = WL_INVALID_HANDLE;
const esp_partition_t *gFatPart = nullptr;
size_t gFlashBytes = 0;
uint32_t gSectorCount = 0;
uint32_t gMscBlockSize = 512;
bool gFatMounted = false;
bool gUsbModeActive = false;
volatile bool gUsbHostActive = false;
bool gUsbHostActivePrev = false;
bool gUsbDisconnectedLogged = false;

bool wlWriteRmw(size_t addr, const uint8_t *src, size_t len)
{
  if (gWlHandle == WL_INVALID_HANDLE)
    return false;

  const size_t wlSector = wl_sector_size(gWlHandle);
  if (wlSector == 0)
    return false;

  std::vector<uint8_t> cache(wlSector);
  if (cache.empty())
    return false;

  while (len > 0)
  {
    const size_t base = (addr / wlSector) * wlSector;
    const size_t inSector = addr - base;
    size_t chunk = wlSector - inSector;
    if (chunk > len)
      chunk = len;

    if (wl_read(gWlHandle, base, cache.data(), wlSector) != ESP_OK)
      return false;
    memcpy(cache.data() + inSector, src, chunk);
    if (wl_erase_range(gWlHandle, base, wlSector) != ESP_OK)
      return false;
    if (wl_write(gWlHandle, base, cache.data(), wlSector) != ESP_OK)
      return false;

    addr += chunk;
    src += chunk;
    len -= chunk;
  }

  return true;
}

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
  if (gWlHandle == WL_INVALID_HANDLE)
    return -1;
  const size_t addr = static_cast<size_t>(lba) * gMscBlockSize + offset;
  if (wl_read(gWlHandle, addr, buffer, bufsize) != ESP_OK)
    return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
  if (gWlHandle == WL_INVALID_HANDLE)
    return -1;
  const size_t addr = static_cast<size_t>(lba) * gMscBlockSize + offset;
  if (!wlWriteRmw(addr, buffer, bufsize))
    return -1;
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t power_condition, bool start, bool load_eject)
{
  (void)power_condition;
  Serial.printf("[HOST][MSC] start=%d eject=%d\n", start, load_eject);
  return true;
}

void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  (void)arg;
  (void)event_data;
  if (event_base != ARDUINO_USB_EVENTS)
    return;

  switch (event_id)
  {
  case ARDUINO_USB_RESUME_EVENT:
    gUsbHostActive = true;
    Serial.println("[HOST][USB] host active");
    break;
  case ARDUINO_USB_SUSPEND_EVENT:
    Serial.println("[HOST][USB] host suspended");
    break;
  case ARDUINO_USB_STOPPED_EVENT:
    gUsbHostActive = false;
    if (!gUsbDisconnectedLogged)
    {
      Serial.println("[HOST][USB] host disconnected");
      gUsbDisconnectedLogged = true;
    }
    break;
  case ARDUINO_USB_STARTED_EVENT:
    gUsbHostActive = true;
    gUsbDisconnectedLogged = false;
    Serial.println("[HOST][USB] device started");
    break;
  default:
    break;
  }
}

bool mountFat()
{
  if (gFatMounted)
    return true;
  if (!FFat.begin(false, kFatMountPoint, 10, kFatPartitionLabel))
  {
    Serial.println("[HOST] FFat.begin failed");
    return false;
  }
  gFatMounted = true;
  Serial.println("[HOST] FAT mounted");
  return true;
}

void unmountFat()
{
  if (!gFatMounted)
    return;
  FFat.end();
  gFatMounted = false;
  Serial.println("[HOST] FAT unmounted");
}

bool openRawBackend()
{
  if (gWlHandle != WL_INVALID_HANDLE)
    return true;

  if (!gFatPart)
  {
    gFatPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, kFatPartitionLabel);
  }
  if (!gFatPart)
  {
    Serial.printf("[HOST][MSC] FAT partition '%s' not found\n", kFatPartitionLabel);
    return false;
  }

  if (wl_mount(gFatPart, &gWlHandle) != ESP_OK)
  {
    Serial.println("[HOST][MSC] wl_mount failed");
    gWlHandle = WL_INVALID_HANDLE;
    return false;
  }

  gFlashBytes = wl_size(gWlHandle);
  gMscBlockSize = static_cast<uint32_t>(wl_sector_size(gWlHandle));
  if (gMscBlockSize == 0 || gMscBlockSize > 65535)
  {
    Serial.printf("[HOST][MSC] invalid block size: %lu\n", static_cast<unsigned long>(gMscBlockSize));
    wl_unmount(gWlHandle);
    gWlHandle = WL_INVALID_HANDLE;
    return false;
  }

  gSectorCount = static_cast<uint32_t>(gFlashBytes / gMscBlockSize);
  if (gSectorCount == 0)
  {
    Serial.println("[HOST][MSC] invalid sector count");
    wl_unmount(gWlHandle);
    gWlHandle = WL_INVALID_HANDLE;
    return false;
  }

  Serial.printf("[HOST][MSC] backend ready: %lu sectors x %lu bytes\n", static_cast<unsigned long>(gSectorCount),
                static_cast<unsigned long>(gMscBlockSize));
  return true;
}

void closeRawBackend()
{
  if (gWlHandle == WL_INVALID_HANDLE)
    return;
  wl_unmount(gWlHandle);
  gWlHandle = WL_INVALID_HANDLE;
  Serial.println("[HOST][MSC] backend closed");
}

bool enterUsbMode()
{
  if (gUsbModeActive)
    return true;
  unmountFat();
  if (!openRawBackend())
    return false;
  gMsc.mediaPresent(true);
  gUsbModeActive = true;
  Serial.println("[HOST][MSC] USB mode active");
  return true;
}

bool enterAppMode()
{
  if (!gUsbModeActive && gFatMounted)
    return true;
  if (gUsbModeActive)
  {
    gMsc.mediaPresent(false);
    gUsbModeActive = false;
    delay(200);
  }
  closeRawBackend();
  return mountFat();
}

} // namespace

void TaskPowerLED(void *pvParameters)
{
  digitalWrite(10, LOW);
  delay(2000);
  digitalWrite(10, HIGH);

  while (true)
  {
    delay(5000);
    digitalWrite(10, LOW);
    delay(80);
    digitalWrite(10, HIGH);
  }
}
void LED_Blitz(uint8_t Index, uint8_t time_ms)
{
  for (uint8_t i = 0; i < Index; i++)
  {
    digitalWrite(10, LOW);
    delay(80);
    digitalWrite(10, HIGH);
    delay(time_ms);
  }
}
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[HOST] ESP-NOW broadcaster boot");

  pinMode(10, OUTPUT);

  xTaskCreate(TaskPowerLED, "LED", 4096, NULL, 1, NULL);

  gMsc.vendorID("ESP32");
  gMsc.productID("S3_FAT_MSC");
  gMsc.productRevision("1.0");
  gMsc.onRead(onRead);
  gMsc.onWrite(onWrite);
  gMsc.onStartStop(onStartStop);
  gMsc.mediaPresent(false);

  if (!openRawBackend())
  {
    Serial.println("[HOST][BOOT] raw FAT backend failed");
    while (true)
      delay(1000);
  }
  if (!gMsc.begin(gSectorCount, static_cast<uint16_t>(gMscBlockSize)))
  {
    Serial.println("[HOST][BOOT] MSC begin failed");
    while (true)
      delay(1000);
  }
  closeRawBackend();

  USB.onEvent(onUsbEvent);
  USB.begin();
  Serial.println("[HOST][BOOT] USB initialized");

  LED_Blitz(3, 160);

  const uint32_t t0 = millis();
  while (millis() - t0 < 1500)
  {
    if (gUsbHostActive)
      break;
    delay(10);
  }

  if (gUsbHostActive)
  {
    Serial.println("[HOST][BOOT] USB detected -> USB mode");
    enterUsbMode();
  }
  else
  {
    Serial.println("[HOST][BOOT] USB not detected -> HOST mode");
    if (enterAppMode())
    {
      if (!hostPortalStart())
      {
        Serial.println("[HOST] portal start failed");
      }
    }
  }
  gUsbHostActivePrev = gUsbHostActive;
}

void loop()
{
  if (gUsbHostActive != gUsbHostActivePrev)
  {
    if (gUsbHostActive)
    {
      Serial.println("[HOST][AUTO] USB plugged -> USB mode");
      LED_Blitz(4, 100);
      hostPortalStop();
      enterUsbMode();
    }
    else
    {
      Serial.println("[HOST][AUTO] USB unplugged -> restart");
      LED_Blitz(4, 300);
      // delay(120);
      esp_restart();
    }
    gUsbHostActivePrev = gUsbHostActive;
  }

  delay(20);
}
