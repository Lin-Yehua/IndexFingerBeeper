/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - 无: 本文件不声明或定义函数。
 */
#pragma once

#include <Arduino.h>

constexpr uint32_t kEspNowTextMagic = 0x54584e57UL; // "WNXT"
constexpr uint8_t kEspNowMsgTypeText = 1;
constexpr size_t kEspNowTextMaxBytes = 192;

struct EspNowTextPacket
{
  uint32_t magic;
  uint8_t type;
  uint8_t reserved;
  uint16_t seq;
  char text[kEspNowTextMaxBytes];
};
