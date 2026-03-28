#pragma once

#include <Arduino.h>

bool wirelessPortalStart();
void wirelessPortalStop();
bool wirelessPortalPopMessage(String &outMessage);
bool wirelessPortalConsumeCsvReloadRequest();
