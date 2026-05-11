#include "CsvTextReader.h"

CsvTextReader::CsvTextReader()
{
  rowCount = 0;
}

void CsvTextReader::trimQuotes(char *str)
{

  int len = strlen(str);

  if (len >= 2 && str[0] == '"' && str[len - 1] == '"')
  {

    memmove(str, str + 1, len - 2);
    str[len - 2] = '\0';
  }
}

bool CsvTextReader::load(fs::FS &fs, const char *path)
{

  File file = fs.open(path, "r");

  if (!file)
  {
    return false;
  }

  rowCount = 0;

  char line[512];

  while (file.available() && rowCount < CSV_MAX_ROWS)
  {

    int len = file.readBytesUntil('\n', line, sizeof(line) - 1);

    line[len] = '\0';

    int lineLen = strlen(line);

    if (lineLen > 0 && line[lineLen - 1] == '\r')
    {
      line[lineLen - 1] = '\0';
    }

    if (strlen(line) == 0)
    {
      continue;
    }

    char *comma = strchr(line, ',');

    if (!comma)
    {
      continue;
    }

    *comma = '\0';

    char *idStr = line;
    char *textStr = comma + 1;

    rows[rowCount].id = atoi(idStr);

    strncpy(rows[rowCount].text, textStr, CSV_TEXT_LEN - 1);
    rows[rowCount].text[CSV_TEXT_LEN - 1] = '\0';

    trimQuotes(rows[rowCount].text);

    rowCount++;
  }

  file.close();

  return true;
}

int CsvTextReader::size()
{
  return rowCount;
}

int CsvTextReader::getMaxRows()
{
  return rowCount;
}

const char *CsvTextReader::getTextByIndex(int index)
{

  if (index < 0 || index >= rowCount)
  {
    return NULL;
  }

  return rows[index].text;
}

const char *CsvTextReader::getTextById(int id)
{

  for (int i = 0; i < rowCount; i++)
  {

    if (rows[i].id == id)
    {
      return rows[i].text;
    }
  }

  return NULL;
}

const char *CsvTextReader::getRandomText()
{

  if (rowCount == 0)
  {
    return NULL;
  }

  int index = random(rowCount);

  return rows[index].text;
}
