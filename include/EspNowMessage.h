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
