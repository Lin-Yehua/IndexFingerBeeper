#include "CsvTextReader.h"

#include <climits>
#include <cstdlib>
#include <esp_heap_caps.h>

bool fatFsTakeWriteMutex(uint32_t timeoutMs);
void fatFsGiveWriteMutex();

namespace
{
constexpr size_t kCsvReadBufferSize = 1024;

class ScopedFatFsLock
{
public:
  explicit ScopedFatFsLock(uint32_t timeoutMs) : locked(fatFsTakeWriteMutex(timeoutMs)) {}
  ~ScopedFatFsLock()
  {
    if (locked)
      fatFsGiveWriteMutex();
  }
  explicit operator bool() const { return locked; }

private:
  bool locked;
};

// Streaming equivalent of atoi() for the ID field. Keeping this state avoids
// allocating a temporary line (or imposing a maximum line length) while the
// file is indexed.
class StreamingIdParser
{
public:
  StreamingIdParser() { reset(); }

  void reset()
  {
    phase = kLeadingSpace;
    negative = false;
    magnitude = 0;
  }

  void push(char ch)
  {
    if (phase == kDone)
      return;

    if (phase == kLeadingSpace)
    {
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f')
        return;
      if (ch == '+' || ch == '-')
      {
        negative = (ch == '-');
        phase = kAfterSign;
        return;
      }
      if (ch < '0' || ch > '9')
      {
        phase = kDone;
        return;
      }
      phase = kDigits;
    }
    else if (phase == kAfterSign)
    {
      if (ch < '0' || ch > '9')
      {
        phase = kDone;
        return;
      }
      phase = kDigits;
    }

    if (phase == kDigits)
    {
      if (ch < '0' || ch > '9')
      {
        phase = kDone;
        return;
      }
      const uint32_t digit = static_cast<uint32_t>(ch - '0');
      const uint32_t limit = negative ? (static_cast<uint32_t>(INT_MAX) + 1U)
                                      : static_cast<uint32_t>(INT_MAX);
      if (magnitude > (limit - digit) / 10U)
        magnitude = limit;
      else
        magnitude = magnitude * 10U + digit;
    }
  }

  int32_t value() const
  {
    if (!negative)
      return static_cast<int32_t>(magnitude);
    if (magnitude >= static_cast<uint32_t>(INT_MAX) + 1U)
      return INT_MIN;
    return -static_cast<int32_t>(magnitude);
  }

private:
  enum Phase : uint8_t
  {
    kLeadingSpace,
    kAfterSign,
    kDigits,
    kDone
  };

  Phase phase;
  bool negative;
  uint32_t magnitude;
};
} // namespace

CsvTextReader::CsvTextReader()
    : rows(nullptr), rowCount(0), sourceFs(nullptr), idsAreSequential(false)
{
  textBuffer[0] = '\0';
}

CsvTextReader::~CsvTextReader()
{
  heap_caps_free(rows);
}

void CsvTextReader::clear()
{
  heap_caps_free(rows);
  rows = nullptr;
  rowCount = 0;
  sourceFs = nullptr;
  sourcePath = "";
  idsAreSequential = false;
  textBuffer[0] = '\0';
}

bool CsvTextReader::scanFile(fs::File &file, RowIndex *output, size_t outputCapacity,
                             size_t &foundRows, bool *sequentialIds)
{
  foundRows = 0;
  if (sequentialIds)
    *sequentialIds = true;

  if (!file.seek(0, fs::SeekSet))
    return false;

  uint8_t buffer[kCsvReadBufferSize];
  uint32_t absoluteOffset = 0;
  bool hasComma = false;
  uint32_t currentTextOffset = 0;
  uint32_t currentTextLength = 0;
  char lastTextByte = '\0';
  int32_t currentId = 0;
  StreamingIdParser idParser;

  auto finishLine = [&]() -> bool
  {
    if (hasComma)
    {
      uint32_t storedLength = currentTextLength;
      if (storedLength > 0 && lastTextByte == '\r')
        --storedLength;

      if (output)
      {
        if (foundRows >= outputCapacity)
          return false;
        output[foundRows].id = currentId;
        output[foundRows].textOffset = currentTextOffset;
        output[foundRows].textLength = storedLength;
        if (sequentialIds && currentId != static_cast<int32_t>(foundRows + 1U))
          *sequentialIds = false;
      }
      ++foundRows;
    }

    hasComma = false;
    currentTextOffset = 0;
    currentTextLength = 0;
    lastTextByte = '\0';
    currentId = 0;
    idParser.reset();
    return true;
  };

  while (true)
  {
    const size_t bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead == 0)
      break;

    for (size_t i = 0; i < bytesRead; ++i, ++absoluteOffset)
    {
      const char ch = static_cast<char>(buffer[i]);
      if (ch == '\n')
      {
        if (!finishLine())
          return false;
        continue;
      }

      if (!hasComma)
      {
        if (ch == ',')
        {
          hasComma = true;
          currentId = idParser.value();
          currentTextOffset = absoluteOffset + 1U;
        }
        else
        {
          idParser.push(ch);
        }
      }
      else
      {
        ++currentTextLength;
        lastTextByte = ch;
      }
    }
  }

  // readBytesUntil() in the old implementation also accepted a final line
  // without a newline, so preserve that behavior.
  if (hasComma && !finishLine())
    return false;

  return static_cast<uint64_t>(absoluteOffset) == static_cast<uint64_t>(file.size());
}

bool CsvTextReader::load(fs::FS &fs, const char *path)
{
  if (!path || !path[0])
    return false;

  ScopedFatFsLock fsLock(5000);
  if (!fsLock)
    return false;

  fs::File file = fs.open(path, FILE_READ);
  if (!file)
    return false;

  if (static_cast<uint64_t>(file.size()) > UINT32_MAX)
  {
    file.close();
    return false;
  }

  // Pass one counts valid rows. Pass two fills one exact-size compact index.
  // This avoids both the former fixed row ceiling and realloc fragmentation.
  size_t indexedRows = 0;
  if (!scanFile(file, nullptr, 0, indexedRows, nullptr) || indexedRows > static_cast<size_t>(INT_MAX))
  {
    file.close();
    return false;
  }

  RowIndex *newRows = nullptr;
  if (indexedRows > 0)
  {
    if (indexedRows > SIZE_MAX / sizeof(RowIndex))
    {
      file.close();
      return false;
    }
    const size_t indexBytes = indexedRows * sizeof(RowIndex);
    newRows = static_cast<RowIndex *>(
        heap_caps_malloc(indexBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!newRows)
    {
      newRows = static_cast<RowIndex *>(heap_caps_malloc(indexBytes, MALLOC_CAP_8BIT));
    }
    if (!newRows)
    {
      file.close();
      return false;
    }
  }

  size_t filledRows = 0;
  bool sequentialIds = true;
  const bool indexed = scanFile(file, newRows, indexedRows, filledRows, &sequentialIds);
  file.close();
  if (!indexed || filledRows != indexedRows)
  {
    heap_caps_free(newRows);
    return false;
  }

  // Only replace a working index after the complete new index succeeds. A
  // transient allocation/read failure therefore leaves the old data usable.
  heap_caps_free(rows);
  rows = newRows;
  rowCount = indexedRows;
  sourceFs = &fs;
  sourcePath = path;
  idsAreSequential = sequentialIds && indexedRows > 0;
  textBuffer[0] = '\0';
  return true;
}

int CsvTextReader::size()
{
  return static_cast<int>(rowCount);
}

int CsvTextReader::getMaxRows()
{
  return static_cast<int>(rowCount);
}

const char *CsvTextReader::readText(const RowIndex &row)
{
  if (!sourceFs || sourcePath.length() == 0)
    return nullptr;

  ScopedFatFsLock fsLock(2000);
  if (!fsLock)
    return nullptr;

  fs::File file = sourceFs->open(sourcePath.c_str(), FILE_READ);
  if (!file)
    return nullptr;

  uint32_t offset = row.textOffset;
  uint32_t length = row.textLength;
  bool quoted = false;
  if (length >= 2U && file.seek(offset, fs::SeekSet))
  {
    const int first = file.read();
    if (first == '"' && file.seek(offset + length - 1U, fs::SeekSet))
      quoted = (file.read() == '"');
  }

  if (quoted)
  {
    ++offset;
    length -= 2U;
  }

  if (!file.seek(offset, fs::SeekSet))
  {
    file.close();
    return nullptr;
  }

  size_t written = 0;
  uint32_t remaining = length;
  bool readFailed = false;
  while (remaining > 0U && written < sizeof(textBuffer) - 1U)
  {
    const int value = file.read();
    if (value < 0)
    {
      readFailed = true;
      break;
    }
    --remaining;

    char ch = static_cast<char>(value);
    // The web editor emits RFC-4180-style doubled quotes inside quoted text.
    if (quoted && ch == '"' && remaining > 0U && file.peek() == '"')
    {
      (void)file.read();
      --remaining;
    }
    textBuffer[written++] = ch;
  }
  file.close();
  if (readFailed)
  {
    textBuffer[0] = '\0';
    return nullptr;
  }
  textBuffer[written] = '\0';
  return textBuffer;
}

const char *CsvTextReader::getTextByIndex(int index)
{
  if (index < 0 || static_cast<size_t>(index) >= rowCount)
    return nullptr;
  return readText(rows[index]);
}

const char *CsvTextReader::getTextById(int id)
{
  if (idsAreSequential && id > 0 && static_cast<size_t>(id) <= rowCount)
    return readText(rows[id - 1]);

  for (size_t i = 0; i < rowCount; ++i)
  {
    if (rows[i].id == id)
      return readText(rows[i]);
  }
  return nullptr;
}

const char *CsvTextReader::getRandomText()
{
  if (rowCount == 0)
    return nullptr;
  return readText(rows[random(static_cast<long>(rowCount))]);
}
