/*
 * 文件说明: 无线门户服务器核心状态实现。
 * 文件功能: 定义 WebServer 派生类、全局队列/状态，以及通用解析辅助函数。
 *
 * 函数表:
 * - writeTextFileAtomically: 保存、写入或更新对应数据。
 * - copyStringToBuf: 模块内部辅助函数。
 * - clampGain: 计算、判断或转换对应结果。
 * - clampApChannel: 计算、判断或转换对应结果。
 * - parseApChannel: 解析、规范化或格式化对应内容。
 * - parseBoolString: 解析、规范化或格式化对应内容。
 * - parseIntString: 解析、规范化或格式化对应内容。
 * - parseFloatString: 解析、规范化或格式化对应内容。
 * - daysFromCivil: 计算、判断或转换对应结果。
 */
bool writeTextFileAtomically(const char *path, const char *tmpPath, const String &content)
{
  if (!path || !tmpPath || !fatMounted)
    return false;
  if (!fatFsTakeWriteMutex(2000))
    return false;

  bool ok = false;
  (void)FFat.remove(tmpPath);
  fs::File f = FFat.open(tmpPath, "w");
  if (!f)
  {
    fatFsGiveWriteMutex();
    return false;
  }

  const size_t written = f.print(content);
  f.flush();
  f.close();

  if (written == content.length())
  {
    if ((!FFat.exists(path) || FFat.remove(path)) && FFat.rename(tmpPath, path))
    {
      ok = true;
    }
  }

  if (!ok)
  {
    (void)FFat.remove(tmpPath);
  }

  fatFsGiveWriteMutex();
  return ok;
}

class PortalWebServer final : public WebServer
{
public:
  explicit PortalWebServer(uint16_t port) : WebServer(port)
  {
  }

  void handleClient() override
  {
    if (_currentStatus == HC_NONE)
    {
      _currentClient = _server.available();
      if (!_currentClient)
      {
        if (_nullDelay)
        {
          delay(1);
        }
        return;
      }

      _currentStatus = HC_WAIT_READ;
      _statusChange = millis();
    }

    bool keepCurrentClient = false;
    bool callYield = false;

    if (_currentClient.connected())
    {
      switch (_currentStatus)
      {
      case HC_NONE:
        break;
      case HC_WAIT_READ:
        if (_currentClient.available())
        {
          // Avoid long busy-loop parsing on captive-portal half-requests.
          static_cast<Stream &>(_currentClient).setTimeout(120);
          if (_parseRequest(_currentClient))
          {
            _currentClient.setTimeout(HTTP_MAX_SEND_WAIT / 1000);
            _contentLength = CONTENT_LENGTH_NOT_SET;
            _handleRequest();
          }
        }
        else
        {
          if (millis() - _statusChange <= HTTP_MAX_DATA_WAIT)
          {
            keepCurrentClient = true;
          }
          callYield = true;
        }
        break;
      case HC_WAIT_CLOSE:
        if (millis() - _statusChange <= HTTP_MAX_CLOSE_WAIT)
        {
          keepCurrentClient = true;
          callYield = true;
        }
        break;
      }
    }

    if (!keepCurrentClient)
    {
      _currentClient = WiFiClient();
      _currentStatus = HC_NONE;
      _currentUpload.reset();
      _currentRaw.reset();
    }

    if (callYield)
    {
      yield();
    }
  }
};

QueueHandle_t gMessageQueue = nullptr;
QueueHandle_t gHostMessageQueue = nullptr;
SemaphoreHandle_t gImageMutex = nullptr;
TaskHandle_t gWebTaskHandle = nullptr;
PortalWebServer *gWebServer = nullptr;
DNSServer *gDnsServer = nullptr;
char gApSsid[kApSsidMaxLen] = {0};
char gApPassword[kApPasswordMaxLen] = {0};
uint8_t gApChannel = kDefaultApChannel;
bool gEnableAp = true;
bool gEnableEspNow = true;
bool gEspNowReady = false;
uint8_t gHostMacFilter[6] = {0};
bool gHostMacFilterEnabled = false;
char gHostMacFilterText[18] = {0};
bool gInstantRefreshNoKey = false;
volatile bool gWebTaskRunning = false;
volatile bool gCsvReloadRequested = false;
volatile bool gScheduleReloadRequested = false;
bool gPortalStarted = false;
PendingImageFrame gPendingImage;
ImageUploadState gImageUpload;
portMUX_TYPE gFlagMux = portMUX_INITIALIZER_UNLOCKED;
struct ImmediateMessageState
{
  char text[kTextMaxLen + 1] = {0};
  bool pending = false;
};
ImmediateMessageState gImmediateMessage;
portMUX_TYPE gImmediateMessageMux = portMUX_INITIALIZER_UNLOCKED;

void copyStringToBuf(const String &src, char *dst, size_t dstSize)
{
  if (!dst || dstSize == 0)
    return;
  if (!src.length())
  {
    dst[0] = '\0';
    return;
  }
  src.toCharArray(dst, dstSize);
  dst[dstSize - 1] = '\0';
}

float clampGain(float value)
{
  if (value < 0.0f)
    return 0.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

uint8_t clampApChannel(int channel)
{
  if (channel < static_cast<int>(kApChannelMin))
    return kApChannelMin;
  if (channel > static_cast<int>(kApChannelMax))
    return kApChannelMax;
  return static_cast<uint8_t>(channel);
}

bool parseApChannel(const String &raw, uint8_t &outChannel)
{
  String s = raw;
  s.trim();
  if (!s.length())
    return false;
  const int channel = s.toInt();
  if (channel < static_cast<int>(kApChannelMin) || channel > static_cast<int>(kApChannelMax))
  {
    return false;
  }
  outChannel = static_cast<uint8_t>(channel);
  return true;
}

bool parseBoolString(const String &raw, bool &outValue)
{
  String v = raw;
  v.trim();
  v.toLowerCase();
  if (!v.length())
    return false;
  if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "enable" || v == "enabled")
  {
    outValue = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no" || v == "disable" || v == "disabled")
  {
    outValue = false;
    return true;
  }
  return false;
}

bool parseIntString(const String &raw, int &outValue)
{
  String v = raw;
  v.trim();
  if (!v.length())
    return false;

  int start = 0;
  if (v[0] == '+' || v[0] == '-')
  {
    if (v.length() == 1)
      return false;
    start = 1;
  }

  for (int i = start; i < v.length(); ++i)
  {
    if (!isDigit(v[i]))
      return false;
  }

  outValue = v.toInt();
  return true;
}

bool parseFloatString(const String &raw, float &outValue)
{
  String v = raw;
  v.trim();
  if (!v.length())
    return false;

  char *endPtr = nullptr;
  const float parsed = strtof(v.c_str(), &endPtr);
  if (endPtr == v.c_str())
    return false;
  while (*endPtr == ' ' || *endPtr == '\t' || *endPtr == '\r' || *endPtr == '\n')
  {
    ++endPtr;
  }
  if (*endPtr != '\0')
    return false;
  if (parsed != parsed)
    return false;

  outValue = parsed;
  return true;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
  year -= (month <= 2U) ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int monthAdj = static_cast<int>(month) + ((month > 2U) ? -3 : 9);
  const unsigned doy = static_cast<unsigned>((153 * monthAdj + 2) / 5 + static_cast<int>(day) - 1);
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}
