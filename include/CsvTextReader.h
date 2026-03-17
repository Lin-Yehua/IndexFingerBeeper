#ifndef CSV_TEXT_READER_H
#define CSV_TEXT_READER_H

#include <Arduino.h>
#include <FS.h>

#define CSV_MAX_ROWS 200
#define CSV_TEXT_LEN 256

struct CsvTextRow {
    int id;
    char text[CSV_TEXT_LEN];
};

class CsvTextReader {
public:

    CsvTextReader();

    bool load(fs::FS &fs, const char *path);

    int size();

    const char* getTextByIndex(int index);

    const char* getTextById(int id);

    const char* getRandomText();

private:

    CsvTextRow rows[CSV_MAX_ROWS];
    int rowCount;

    void trimQuotes(char *str);
};

#endif