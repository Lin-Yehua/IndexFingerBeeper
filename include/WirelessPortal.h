#pragma once

#include <Arduino.h>

bool wirelessPortalStart();
void wirelessPortalStop();
bool wirelessPortalPopMessage(String &outMessage);
bool wirelessPortalHasPendingMessage();
bool wirelessPortalPopHostMessage(String &outMessage);
bool wirelessPortalHasPendingHostMessage();
bool wirelessPortalConsumeCsvReloadRequest();
bool wirelessPortalInstantRefreshNoKeyEnabled();
bool wirelessPortalTakePendingImage(uint16_t *outPixels,
                                    size_t outCapacityPixels,
                                    uint16_t &outWidth,
                                    uint16_t &outHeight,
                                    int16_t &outCenterX,
                                    int16_t &outCenterY,
                                    size_t &outPixelCount);
