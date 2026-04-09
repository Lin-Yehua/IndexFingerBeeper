#include "AppGlobals.h"

const char *kFatPartitionLabel = "fatfs";
const char *kFatMountPoint = "/fat";
const char *kLittleFsPartitionLabel = "littlefs";

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
bool gEnableReprint = true;
int gBacklightTimeSec = -1;
bool firstFlag = false;
uint8_t RUNSTATE = 0;
