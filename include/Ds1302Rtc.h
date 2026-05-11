#pragma once

#include <Arduino.h>

struct Ds1302DateTime
{
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

bool ds1302IsValidDateTime(const Ds1302DateTime &dt);

class Ds1302Rtc
{
public:
  static constexpr uint8_t kDefaultClkPin = 47;
  static constexpr uint8_t kDefaultDatPin = 48;
  static constexpr uint8_t kDefaultRstPin = 45;

  Ds1302Rtc(uint8_t clkPin = kDefaultClkPin, uint8_t datPin = kDefaultDatPin, uint8_t rstPin = kDefaultRstPin);

  void begin();

  bool readDateTime(Ds1302DateTime &out);
  bool writeDateTime(const Ds1302DateTime &dt);

  bool readYmd(uint16_t &year, uint8_t &month, uint8_t &day);
  bool readHms(uint8_t &hour, uint8_t &minute, uint8_t &second);
  bool writeYmd(uint16_t year, uint8_t month, uint8_t day);
  bool writeHms(uint8_t hour, uint8_t minute, uint8_t second);

  bool readYear(uint16_t &year);
  bool readMonth(uint8_t &month);
  bool readDay(uint8_t &day);
  bool readHour(uint8_t &hour);
  bool readMinute(uint8_t &minute);
  bool readSecond(uint8_t &second);

  bool writeYear(uint16_t year);
  bool writeMonth(uint8_t month);
  bool writeDay(uint8_t day);
  bool writeHour(uint8_t hour);
  bool writeMinute(uint8_t minute);
  bool writeSecond(uint8_t second);

private:
  static bool decodeBurstDateTime(const uint8_t *raw, Ds1302DateTime &out);
  static uint8_t decToBcd(uint8_t value);
  static uint8_t bcdToDec(uint8_t value);
  static uint8_t decodeHour(uint8_t rawHour);
  static uint8_t dayOfWeek(uint16_t year, uint8_t month, uint8_t day);

  void beginTransfer();
  void endTransfer();
  void setDataOut();
  void setDataIn();
  void writeByte(uint8_t value);
  uint8_t readByte();

  void writeRegister(uint8_t reg, uint8_t value);
  uint8_t readRegister(uint8_t reg);
  void writeClockBurst(const uint8_t *data, size_t len);
  void readClockBurst(uint8_t *data, size_t len);
  void setWriteProtect(bool enabled);

  uint8_t clkPin_;
  uint8_t datPin_;
  uint8_t rstPin_;
};

extern Ds1302Rtc rtc;
