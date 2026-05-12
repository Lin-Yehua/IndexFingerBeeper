/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - ds1302IsValidDateTime: 模块内部辅助函数。
 * - begin: 初始化或确保对应资源可用。
 * - readDateTime: 读取、获取或消费对应数据。
 * - writeDateTime: 保存、写入或更新对应数据。
 * - readYmd: 读取、获取或消费对应数据。
 * - readHms: 读取、获取或消费对应数据。
 * - writeYmd: 保存、写入或更新对应数据。
 * - writeHms: 保存、写入或更新对应数据。
 * - readYear: 读取、获取或消费对应数据。
 * - readMonth: 读取、获取或消费对应数据。
 * - readDay: 读取、获取或消费对应数据。
 * - readHour: 读取、获取或消费对应数据。
 * - readMinute: 读取、获取或消费对应数据。
 * - readSecond: 读取、获取或消费对应数据。
 * - writeYear: 保存、写入或更新对应数据。
 * - writeMonth: 保存、写入或更新对应数据。
 * - writeDay: 保存、写入或更新对应数据。
 * - writeHour: 保存、写入或更新对应数据。
 * - writeMinute: 保存、写入或更新对应数据。
 * - writeSecond: 保存、写入或更新对应数据。
 * - decodeBurstDateTime: 解析、规范化或格式化对应内容。
 * - decToBcd: 模块内部辅助函数。
 * - bcdToDec: 模块内部辅助函数。
 * - decodeHour: 解析、规范化或格式化对应内容。
 * - dayOfWeek: 模块内部辅助函数。
 * - beginTransfer: 初始化或确保对应资源可用。
 * - endTransfer: 模块内部辅助函数。
 * - setDataOut: 保存、写入或更新对应数据。
 * - setDataIn: 保存、写入或更新对应数据。
 * - writeByte: 保存、写入或更新对应数据。
 * - readByte: 读取、获取或消费对应数据。
 * - writeRegister: 保存、写入或更新对应数据。
 * - readRegister: 读取、获取或消费对应数据。
 * - writeClockBurst: 保存、写入或更新对应数据。
 * - readClockBurst: 读取、获取或消费对应数据。
 * - setWriteProtect: 保存、写入或更新对应数据。
 */
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
