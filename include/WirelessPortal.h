#pragma once

#include <Arduino.h>

bool wirelessPortalStart();
bool wirelessPortalStartEspNowOnly();
void wirelessPortalStop();
bool wirelessPortalPopMessage(String &outMessage);
bool wirelessPortalHasPendingMessage();
bool wirelessPortalPopImmediateMessage(String &outMessage);
bool wirelessPortalHasPendingImmediateMessage();
bool wirelessPortalPopHostMessage(String &outMessage);
bool wirelessPortalHasPendingHostMessage();
bool wirelessPortalPushMessageForRestore(const String &text);
bool wirelessPortalPushImmediateMessageForRestore(const String &text);
bool wirelessPortalPushHostMessageForRestore(const String &text);
bool wirelessPortalConsumeCsvReloadRequest();
bool wirelessPortalConsumeScheduleReloadRequest();
bool wirelessPortalInstantRefreshNoKeyEnabled();
bool wirelessPortalTakePendingImage(uint16_t *outPixels, size_t outCapacityPixels, uint16_t &outWidth,
                                    uint16_t &outHeight, int16_t &outCenterX, int16_t &outCenterY,
                                    size_t &outPixelCount);
