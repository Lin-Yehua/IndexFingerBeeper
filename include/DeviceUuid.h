/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - deviceUuidRead: 模块内部辅助函数。
 * - deviceUuidEnsureFromNtpDateTime: 模块内部辅助函数。
 */
#pragma once

#include <Arduino.h>

#include "Ds1302Rtc.h"

bool deviceUuidRead(String &outUuid);
bool deviceUuidEnsureFromNtpDateTime(const Ds1302DateTime &dt, String &outUuid, String &errorOut);
