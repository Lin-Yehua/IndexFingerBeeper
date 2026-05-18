#pragma once

#include <Arduino.h>

#include "Ds1302Rtc.h"

bool deviceUuidRead(String &outUuid);
bool deviceUuidEnsureFromNtpDateTime(const Ds1302DateTime &dt, String &outUuid, String &errorOut);
