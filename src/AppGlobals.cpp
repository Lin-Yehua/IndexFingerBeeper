/*
 * 文件说明: 模块实现文件。
 * 文件功能: 实现对应模块的运行逻辑和内部辅助函数。
 *
 * 函数表:
 * - fatFsTakeWriteMutex: 模块内部辅助函数。
 * - fatFsGiveWriteMutex: 模块内部辅助函数。
 */
#include "AppGlobals.h"

const char *kFatPartitionLabel = "fatfs";
const char *kFatMountPoint = "/fat";
const char *kLittleFsPartitionLabel = "littlefs";

namespace
{
portMUX_TYPE sFatFsWriteMutexCreateMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t sFatFsWriteMutex = nullptr;
} // namespace

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite Text = TFT_eSprite(&tft);
TFT_eSprite spriteBoot = TFT_eSprite(&tft);
TFT_eSprite spriteBG = TFT_eSprite(&tft);
CsvTextReader csv;
WavMixerI2S mixer;

const char *message = nullptr;
const char *junkChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01289!@#$%^&*()[]{}<>?/|\\~`+-=_";

USBMSC msc;
wl_handle_t wlHandle = WL_INVALID_HANDLE;
const esp_partition_t *fatPart = nullptr;
size_t flashBytes = 0;
uint32_t sectorCount = 0;
uint32_t mscBlockSize = 512;

bool fatMounted = false;
bool usbModeActive = false;
volatile bool usbHostActive = false;
bool usbHostActivePrev = false;
bool usbDisconnectedLogged = false;

bool appInitialized = false;
bool displayBootstrapped = false;
uint8_t Sound_count = 0;
int csvCount = 0;
int csvArray[kCsvArrayCapacity] = {0};
float gInsertGain = 0.2f;
float gBgGain = 0.2f;
float gBacklightLevel = 1.0f;
int gWrongProb3 = 25;
int gWrongProb5 = 12;
int gInsertSoundBaseProbability = 10;
int gInsertSoundIncreaseProbability = 5;
bool gEnableReprint = true;
int gBacklightTimeSec = -1;
int gBacklightCloseTimeSec = 20;
int gSleepTimeMin = 1;
bool firstFlag = false;
uint8_t RUNSTATE = 0;

bool fatFsTakeWriteMutex(uint32_t timeoutMs)
{
  SemaphoreHandle_t mutex = sFatFsWriteMutex;
  if (!mutex)
  {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created)
      return false;

    portENTER_CRITICAL(&sFatFsWriteMutexCreateMux);
    if (!sFatFsWriteMutex)
    {
      sFatFsWriteMutex = created;
      created = nullptr;
    }
    mutex = sFatFsWriteMutex;
    portEXIT_CRITICAL(&sFatFsWriteMutexCreateMux);

    if (created)
    {
      vSemaphoreDelete(created);
    }
  }

  return xSemaphoreTake(mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void fatFsGiveWriteMutex()
{
  if (sFatFsWriteMutex)
  {
    xSemaphoreGive(sFatFsWriteMutex);
  }
}
