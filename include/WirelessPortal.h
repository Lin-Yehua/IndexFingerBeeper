#pragma once

#include <Arduino.h>

bool wirelessPortalStart();
void wirelessPortalStop();
bool wirelessPortalPopMessage(String &outMessage);
bool wirelessPortalHasPendingMessage();
bool wirelessPortalPopHostMessage(String &outMessage);
bool wirelessPortalHasPendingHostMessage();
bool wirelessPortalConsumeCsvReloadRequest();
