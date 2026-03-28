#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <USBMSC.h>
#include "CsvTextReader.h"
#include "WavMixerI2S.h"

extern "C" {
#include "wear_levelling.h"
#include "esp_partition.h"
}

extern const char *kFatPartitionLabel;
extern const char *kFatMountPoint;
extern const char *kLittleFsPartitionLabel;

extern TFT_eSPI tft;
extern TFT_eSprite Text;
extern TFT_eSprite spriteBoot;
extern TFT_eSprite spriteBG;
extern CsvTextReader csv;
extern WavMixerI2S mixer;

extern const char *message;
extern const char *junkChars;

extern USBMSC msc;
extern wl_handle_t wlHandle;
extern const esp_partition_t *fatPart;
extern size_t flashBytes;
extern uint32_t sectorCount;
extern uint32_t mscBlockSize;

extern bool fatMounted;
extern bool usbModeActive;
extern volatile bool usbHostActive;
extern bool usbHostActivePrev;
extern bool usbDisconnectedLogged;

extern bool appInitialized;
extern bool displayBootstrapped;
extern uint8_t Sound_count;
constexpr int kCsvArrayCapacity = 8192;
extern int csvCount;
extern int csvArray[kCsvArrayCapacity];
extern float gInsertGain;
extern float gBgGain;
extern int gWrongProb3;
extern int gWrongProb5;
extern bool gEnableReprint;
extern int gBacklightTimeSec;
extern bool firstFlag;
extern uint8_t RUNSTATE;
