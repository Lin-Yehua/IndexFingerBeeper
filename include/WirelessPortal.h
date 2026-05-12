/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - wirelessPortalStart: 模块内部辅助函数。
 * - wirelessPortalStartEspNowOnly: 模块内部辅助函数。
 * - wirelessPortalStop: 模块内部辅助函数。
 * - wirelessPortalPopMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingMessage: 模块内部辅助函数。
 * - wirelessPortalPopImmediateMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingImmediateMessage: 模块内部辅助函数。
 * - wirelessPortalPopHostMessage: 模块内部辅助函数。
 * - wirelessPortalHasPendingHostMessage: 模块内部辅助函数。
 * - wirelessPortalPushMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalPushImmediateMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalPushHostMessageForRestore: 模块内部辅助函数。
 * - wirelessPortalConsumeCsvReloadRequest: 模块内部辅助函数。
 * - wirelessPortalConsumeScheduleReloadRequest: 模块内部辅助函数。
 * - wirelessPortalInstantRefreshNoKeyEnabled: 模块内部辅助函数。
 * - wirelessPortalTakePendingImage: 模块内部辅助函数。
 */
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
