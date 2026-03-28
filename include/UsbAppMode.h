#pragma once

#include <Arduino.h>

void ensureDisplayReady();
void showUsbModeScreen();
void applyAudioGainsFromSettingIni();

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
bool onStartStop(uint8_t power_condition, bool start, bool load_eject);
void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

bool mountFat();
void unmountFat();
bool openRawBackend();
void closeRawBackend();
bool enterUsbMode();
bool enterAppMode();

bool initProjectResources();
void processAppLoop();

void notifyBacklightActivity();
void setBacklightTimeSeconds(int seconds);
