#include "Ds1302Rtc.h"
Ds1302Rtc rtc(47, 48, 45);  // CLK, DAT, RST
namespace {

constexpr uint8_t kRegSeconds = 0x80;
constexpr uint8_t kRegMinutes = 0x82;
constexpr uint8_t kRegHours = 0x84;
constexpr uint8_t kRegDate = 0x86;
constexpr uint8_t kRegMonth = 0x88;
constexpr uint8_t kRegDay = 0x8A;
constexpr uint8_t kRegYear = 0x8C;
constexpr uint8_t kRegWriteProtect = 0x8E;
constexpr uint8_t kCmdClockBurstWrite = 0xBE;
constexpr uint8_t kCmdClockBurstRead = 0xBF;

bool isLeapYear(uint16_t year) {
  if ((year % 4U) != 0U) return false;
  if ((year % 100U) != 0U) return true;
  return (year % 400U) == 0U;
}

uint8_t maxDayInMonth(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

}  // namespace

bool ds1302IsValidDateTime(const Ds1302DateTime &dt) {
  if (dt.year < 2000 || dt.year > 2099) return false;
  if (dt.month < 1 || dt.month > 12) return false;
  const uint8_t maxDay = maxDayInMonth(dt.year, dt.month);
  if (maxDay == 0 || dt.day < 1 || dt.day > maxDay) return false;
  if (dt.hour > 23) return false;
  if (dt.minute > 59) return false;
  if (dt.second > 59) return false;
  return true;
}

Ds1302Rtc::Ds1302Rtc(uint8_t clkPin, uint8_t datPin, uint8_t rstPin)
    : clkPin_(clkPin), datPin_(datPin), rstPin_(rstPin) {}

void Ds1302Rtc::begin() {
  pinMode(clkPin_, OUTPUT);
  pinMode(rstPin_, OUTPUT);
  pinMode(datPin_, INPUT);
  digitalWrite(clkPin_, LOW);
  digitalWrite(rstPin_, LOW);

  // Make startup state writable and ensure oscillator is running (CH=0).
  setWriteProtect(false);
  const uint8_t secRaw = readRegister(kRegSeconds);
  writeRegister(kRegSeconds, secRaw & 0x7F);
}

bool Ds1302Rtc::readDateTime(Ds1302DateTime &out) {
  uint8_t raw[8] = {0};
  readClockBurst(raw, sizeof(raw));
  if (decodeBurstDateTime(raw, out)) {
    return true;
  }

  // Recovery path: clear write-protect and CH bit, then retry once.
  setWriteProtect(false);
  const uint8_t secRaw = readRegister(kRegSeconds);
  writeRegister(kRegSeconds, secRaw & 0x7F);
  readClockBurst(raw, sizeof(raw));
  return decodeBurstDateTime(raw, out);
}

bool Ds1302Rtc::decodeBurstDateTime(const uint8_t *raw, Ds1302DateTime &out) {
  if (!raw) return false;

  out.second = bcdToDec(raw[0] & 0x7F);
  out.minute = bcdToDec(raw[1] & 0x7F);
  out.hour = decodeHour(raw[2]);
  out.day = bcdToDec(raw[3] & 0x3F);
  out.month = bcdToDec(raw[4] & 0x1F);
  out.year = static_cast<uint16_t>(2000 + bcdToDec(raw[6] & 0xFF));
  return ds1302IsValidDateTime(out);
}

bool Ds1302Rtc::writeDateTime(const Ds1302DateTime &dt) {
  if (!ds1302IsValidDateTime(dt)) return false;

  uint8_t raw[8] = {0};
  raw[0] = decToBcd(dt.second) & 0x7F;
  raw[1] = decToBcd(dt.minute) & 0x7F;
  raw[2] = decToBcd(dt.hour) & 0x3F;  // Force 24-hour mode.
  raw[3] = decToBcd(dt.day) & 0x3F;
  raw[4] = decToBcd(dt.month) & 0x1F;
  raw[5] = dayOfWeek(dt.year, dt.month, dt.day);
  raw[6] = decToBcd(static_cast<uint8_t>(dt.year - 2000));
  raw[7] = 0x00;  // Write protect off.

  setWriteProtect(false);
  writeClockBurst(raw, sizeof(raw));
  return true;
}

bool Ds1302Rtc::readYmd(uint16_t &year, uint8_t &month, uint8_t &day) {
  Ds1302DateTime dt;
  if (!readDateTime(dt)) return false;
  year = dt.year;
  month = dt.month;
  day = dt.day;
  return true;
}

bool Ds1302Rtc::readHms(uint8_t &hour, uint8_t &minute, uint8_t &second) {
  Ds1302DateTime dt;
  if (!readDateTime(dt)) return false;
  hour = dt.hour;
  minute = dt.minute;
  second = dt.second;
  return true;
}

bool Ds1302Rtc::writeYmd(uint16_t year, uint8_t month, uint8_t day) {
  Ds1302DateTime dt;
  if (!readDateTime(dt)) {
    dt.year = year;
    dt.month = month;
    dt.day = day;
    return writeDateTime(dt);
  }
  dt.year = year;
  dt.month = month;
  dt.day = day;
  return writeDateTime(dt);
}

bool Ds1302Rtc::writeHms(uint8_t hour, uint8_t minute, uint8_t second) {
  Ds1302DateTime dt;
  if (!readDateTime(dt)) {
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    return writeDateTime(dt);
  }
  dt.hour = hour;
  dt.minute = minute;
  dt.second = second;
  return writeDateTime(dt);
}

bool Ds1302Rtc::readYear(uint16_t &year) {
  const uint8_t raw = readRegister(kRegYear);
  year = static_cast<uint16_t>(2000 + bcdToDec(raw));
  return year >= 2000 && year <= 2099;
}

bool Ds1302Rtc::readMonth(uint8_t &month) {
  month = bcdToDec(readRegister(kRegMonth) & 0x1F);
  return month >= 1 && month <= 12;
}

bool Ds1302Rtc::readDay(uint8_t &day) {
  day = bcdToDec(readRegister(kRegDate) & 0x3F);
  return day >= 1 && day <= 31;
}

bool Ds1302Rtc::readHour(uint8_t &hour) {
  hour = decodeHour(readRegister(kRegHours));
  return hour <= 23;
}

bool Ds1302Rtc::readMinute(uint8_t &minute) {
  minute = bcdToDec(readRegister(kRegMinutes) & 0x7F);
  return minute <= 59;
}

bool Ds1302Rtc::readSecond(uint8_t &second) {
  second = bcdToDec(readRegister(kRegSeconds) & 0x7F);
  return second <= 59;
}

bool Ds1302Rtc::writeYear(uint16_t year) {
  if (year < 2000 || year > 2099) return false;
  setWriteProtect(false);
  writeRegister(kRegYear, decToBcd(static_cast<uint8_t>(year - 2000)));
  return true;
}

bool Ds1302Rtc::writeMonth(uint8_t month) {
  if (month < 1 || month > 12) return false;
  setWriteProtect(false);
  writeRegister(kRegMonth, decToBcd(month) & 0x1F);
  return true;
}

bool Ds1302Rtc::writeDay(uint8_t day) {
  if (day < 1 || day > 31) return false;
  setWriteProtect(false);
  writeRegister(kRegDate, decToBcd(day) & 0x3F);
  return true;
}

bool Ds1302Rtc::writeHour(uint8_t hour) {
  if (hour > 23) return false;
  setWriteProtect(false);
  writeRegister(kRegHours, decToBcd(hour) & 0x3F);  // 24-hour mode.
  return true;
}

bool Ds1302Rtc::writeMinute(uint8_t minute) {
  if (minute > 59) return false;
  setWriteProtect(false);
  writeRegister(kRegMinutes, decToBcd(minute) & 0x7F);
  return true;
}

bool Ds1302Rtc::writeSecond(uint8_t second) {
  if (second > 59) return false;
  setWriteProtect(false);
  writeRegister(kRegSeconds, decToBcd(second) & 0x7F);  // CH=0, keep clock running.
  return true;
}

uint8_t Ds1302Rtc::decToBcd(uint8_t value) {
  return static_cast<uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

uint8_t Ds1302Rtc::bcdToDec(uint8_t value) {
  return static_cast<uint8_t>(((value >> 4U) * 10U) + (value & 0x0FU));
}

uint8_t Ds1302Rtc::decodeHour(uint8_t rawHour) {
  if (rawHour & 0x80) {
    uint8_t hour = bcdToDec(rawHour & 0x1F);
    if (hour == 12) hour = 0;
    if (rawHour & 0x20) {
      hour = static_cast<uint8_t>(hour + 12);
    }
    return static_cast<uint8_t>(hour % 24);
  }
  return bcdToDec(rawHour & 0x3F);
}

uint8_t Ds1302Rtc::dayOfWeek(uint16_t year, uint8_t month, uint8_t day) {
  static const uint8_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  uint16_t y = year;
  if (month < 3) {
    --y;
  }
  const uint8_t dow0 = static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7);
  return dow0 == 0 ? 1 : static_cast<uint8_t>(dow0 + 1);
}

void Ds1302Rtc::beginTransfer() {
  digitalWrite(clkPin_, LOW);
  setDataOut();
  digitalWrite(rstPin_, HIGH);
  delayMicroseconds(1);
}

void Ds1302Rtc::endTransfer() {
  setDataOut();
  digitalWrite(datPin_, LOW);
  digitalWrite(rstPin_, LOW);
  digitalWrite(clkPin_, LOW);
}

void Ds1302Rtc::setDataOut() {
  pinMode(datPin_, OUTPUT);
}

void Ds1302Rtc::setDataIn() {
  pinMode(datPin_, INPUT);
}

void Ds1302Rtc::writeByte(uint8_t value) {
  setDataOut();
  for (uint8_t i = 0; i < 8; ++i) {
    digitalWrite(datPin_, (value >> i) & 0x01U ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(clkPin_, HIGH);
    delayMicroseconds(1);
    digitalWrite(clkPin_, LOW);
  }
}

uint8_t Ds1302Rtc::readByte() {
  setDataIn();
  delayMicroseconds(1);
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (digitalRead(datPin_)) {
      value |= static_cast<uint8_t>(1U << i);
    }
    digitalWrite(clkPin_, HIGH);
    delayMicroseconds(1);
    digitalWrite(clkPin_, LOW);
    delayMicroseconds(1);
  }
  return value;
}

void Ds1302Rtc::writeRegister(uint8_t reg, uint8_t value) {
  beginTransfer();
  writeByte(reg & 0xFEU);
  writeByte(value);
  endTransfer();
}

uint8_t Ds1302Rtc::readRegister(uint8_t reg) {
  beginTransfer();
  writeByte(reg | 0x01U);
  const uint8_t value = readByte();
  endTransfer();
  return value;
}

void Ds1302Rtc::writeClockBurst(const uint8_t *data, size_t len) {
  beginTransfer();
  writeByte(kCmdClockBurstWrite);
  for (size_t i = 0; i < len; ++i) {
    writeByte(data[i]);
  }
  endTransfer();
}

void Ds1302Rtc::readClockBurst(uint8_t *data, size_t len) {
  beginTransfer();
  writeByte(kCmdClockBurstRead);
  for (size_t i = 0; i < len; ++i) {
    data[i] = readByte();
  }
  endTransfer();
}

void Ds1302Rtc::setWriteProtect(bool enabled) {
  writeRegister(kRegWriteProtect, enabled ? 0x80 : 0x00);
}
