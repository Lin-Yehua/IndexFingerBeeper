#ifndef CSV_TEXT_READER_H
#define CSV_TEXT_READER_H

#include <Arduino.h>
#include <FS.h>

// Text is still bounded because callers consume a C string and the display has
// a practical per-message limit. Row count, however, is no longer bounded by a
// fixed array: only a compact on-demand index is kept in RAM.
#define CSV_TEXT_LEN 256

class CsvTextReader
{
public:
  CsvTextReader();
  ~CsvTextReader();

  CsvTextReader(const CsvTextReader &) = delete;
  CsvTextReader &operator=(const CsvTextReader &) = delete;

  bool load(fs::FS &fs, const char *path);
  void clear();

  int size();
  int getMaxRows();

  const char *getTextByIndex(int index);
  const char *getTextById(int id);
  const char *getRandomText();

private:
  struct RowIndex
  {
    int32_t id;
    uint32_t textOffset;
    uint32_t textLength;
  };

  RowIndex *rows;
  size_t rowCount;
  fs::FS *sourceFs;
  String sourcePath;
  char textBuffer[CSV_TEXT_LEN];
  bool idsAreSequential;

  bool scanFile(fs::File &file, RowIndex *output, size_t outputCapacity,
                size_t &foundRows, bool *sequentialIds);
  const char *readText(const RowIndex &row);
};

#endif
