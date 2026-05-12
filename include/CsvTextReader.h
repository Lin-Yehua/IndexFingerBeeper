/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - load: 读取、获取或消费对应数据。
 * - size: 模块内部辅助函数。
 * - getMaxRows: 读取、获取或消费对应数据。
 * - trimQuotes: 解析、规范化或格式化对应内容。
 * - getTextByIndex: 读取、获取或消费对应数据。
 * - getTextById: 读取、获取或消费对应数据。
 * - getRandomText: 读取、获取或消费对应数据。
 */
#ifndef CSV_TEXT_READER_H
#define CSV_TEXT_READER_H

#include <Arduino.h>
#include <FS.h>

#define CSV_MAX_ROWS 200
#define CSV_TEXT_LEN 256

struct CsvTextRow
{
  int id;
  char text[CSV_TEXT_LEN];
};

class CsvTextReader
{
public:
  CsvTextReader();

  bool load(fs::FS &fs, const char *path);

  int size();
  int getMaxRows();

  const char *getTextByIndex(int index);

  const char *getTextById(int id);

  const char *getRandomText();

private:
  CsvTextRow rows[CSV_MAX_ROWS];
  int rowCount;

  void trimQuotes(char *str);
};

#endif
