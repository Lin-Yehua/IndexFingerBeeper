/*
 * 文件说明: STA 联网和云端消息私有实现。
 * 文件功能: 维护 STA 消息队列、解析云端响应、保存/读取联网配置，并完成 NTP 校时。
 *
 * 函数表:
 * - ensureStaQueueMutex: 初始化或确保对应资源可用。
 * - resetStaHttpClient: 停止、释放或清理对应状态。
 * - clearStaMessageQueue: 停止、释放或清理对应状态。
 * - normalizeStaQueueMessage: 解析、规范化或格式化对应内容。
 * - pushStaMessageQueueLocked: 保存、写入或更新对应数据。
 * - pushStaMessageQueue: 保存、写入或更新对应数据。
 * - popStaMessageQueue: 读取、获取或消费对应数据。
 * - staMessageQueueSize: 模块内部辅助函数。
 * - hexNibble: 模块内部辅助函数。
 * - appendUtf8Codepoint: 保存、写入或更新对应数据。
 * - extractJsonStringField: 模块内部辅助函数。
 * - extractJsonTextField: 模块内部辅助函数。
 * - percentEncodeUriComponent: 计算、判断或转换对应结果。
 * - normalizeMacAddressForApi: 解析、规范化或格式化对应内容。
 * - appendDeviceIdentityToApiUrl: 保存、写入或更新对应数据。
 * - fetchStaMessageFromCloud: 模块内部辅助函数。
 * - fetchAndQueueOneStaMessage: 模块内部辅助函数。
 * - staMessageFetcherTask: FreeRTOS 任务入口或任务控制函数。
 * - waitStaFetcherIdle: 模块内部辅助函数。
 * - ensureStaFetcherTaskStarted: 初始化或确保对应资源可用。
 * - trimIniValue: 解析、规范化或格式化对应内容。
 * - parseAppModeFromIniValue: 解析、规范化或格式化对应内容。
 * - loadAppModeFromSettingIni: 读取、获取或消费对应数据。
 * - persistAppModeToSettingIni: 保存、写入或更新对应数据。
 * - loadStaCredentialsFromSettingIni: 读取、获取或消费对应数据。
 * - tmToDs1302DateTime: 模块内部辅助函数。
 * - syncDs1302FromStaNtp: 模块内部辅助函数。
 * - appModeToIniValue: 模块内部辅助函数。
 */
static bool ensureStaQueueMutex()
{
  if (gStaMsgQueueMutex)
    return true;
  gStaMsgQueueMutex = xSemaphoreCreateMutex();
  return gStaMsgQueueMutex != nullptr;
}

static void resetStaHttpClient()
{
  if (gStaHttpsClientReady)
  {
    gStaHttpsClient.stop();
  }
  gStaHttpsClientReady = false;
}

static void clearStaMessageQueue()
{
  if (!ensureStaQueueMutex())
    return;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return;
  for (size_t i = 0; i < kStaPrefetchDepth; ++i)
  {
    gStaMsgQueue[i] = "";
  }
  gStaMsgQueueHead = 0;
  gStaMsgQueueSize = 0;
  gStaBottlePriorityActive = false;
  gStaBottlePriorityNextLogical = 0;
  xSemaphoreGive(gStaMsgQueueMutex);
}

static bool normalizeStaQueueMessage(const String &message, String &outNormalized)
{
  outNormalized = message;
  outNormalized.replace("\r", " ");
  outNormalized.replace("\n", " ");
  outNormalized.trim();
  if (!outNormalized.length())
    return false;
  if (outNormalized.length() > kSleepTextMaxLen)
  {
    outNormalized.remove(kSleepTextMaxLen);
  }
  return outNormalized.length() > 0;
}

static bool pushStaMessageQueueLocked(const String &normalized)
{
  if (gStaMsgQueueSize >= kStaPrefetchDepth)
    return false;
  const size_t tail = (gStaMsgQueueHead + gStaMsgQueueSize) % kStaPrefetchDepth;
  gStaMsgQueue[tail] = normalized;
  gStaMsgQueueSize++;
  return true;
}

static bool pushStaMessageQueue(const String &message, bool prioritizeBottleMessage)
{
  if (!ensureStaQueueMutex())
    return false;
  String normalized = message;
  if (!normalizeStaQueueMessage(message, normalized))
    return false;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return false;

  bool ok = false;
  if (prioritizeBottleMessage)
  {
    if (!gStaBottlePriorityActive)
    {
      gStaBottlePriorityActive = true;
      // Always start overriding from queue head.
      gStaBottlePriorityNextLogical = 0;
    }
    if (gStaBottlePriorityNextLogical > gStaMsgQueueSize)
    {
      gStaBottlePriorityNextLogical = gStaMsgQueueSize;
    }

    if (gStaBottlePriorityNextLogical < gStaMsgQueueSize)
    {
      const size_t writeIdx = (gStaMsgQueueHead + gStaBottlePriorityNextLogical) % kStaPrefetchDepth;
      gStaMsgQueue[writeIdx] = normalized;
      gStaBottlePriorityNextLogical++;
      ok = true;
    }
    else
    {
      ok = pushStaMessageQueueLocked(normalized);
      if (ok)
      {
        // Continue from tail for subsequent bottle messages.
        gStaBottlePriorityNextLogical = gStaMsgQueueSize;
      }
    }
  }
  else
  {
    gStaBottlePriorityActive = false;
    gStaBottlePriorityNextLogical = 0;
    ok = pushStaMessageQueueLocked(normalized);
  }

  xSemaphoreGive(gStaMsgQueueMutex);
  return ok;
}

static bool popStaMessageQueue(String &outMessage)
{
  if (!ensureStaQueueMutex())
    return false;
  outMessage = "";
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return false;
  if (gStaMsgQueueSize == 0)
  {
    xSemaphoreGive(gStaMsgQueueMutex);
    return false;
  }

  outMessage = gStaMsgQueue[gStaMsgQueueHead];
  gStaMsgQueue[gStaMsgQueueHead] = "";
  gStaMsgQueueHead = (gStaMsgQueueHead + 1) % kStaPrefetchDepth;
  gStaMsgQueueSize--;
  if (gStaMsgQueueSize == 0)
  {
    gStaBottlePriorityActive = false;
    gStaBottlePriorityNextLogical = 0;
  }
  else if (gStaBottlePriorityActive)
  {
    if (gStaBottlePriorityNextLogical > 0)
    {
      gStaBottlePriorityNextLogical--;
    }
    if (gStaBottlePriorityNextLogical > gStaMsgQueueSize)
    {
      gStaBottlePriorityNextLogical = gStaMsgQueueSize;
    }
  }
  xSemaphoreGive(gStaMsgQueueMutex);
  return outMessage.length() > 0;
}

static size_t staMessageQueueSize()
{
  if (!ensureStaQueueMutex())
    return 0;
  if (xSemaphoreTake(gStaMsgQueueMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return 0;
  const size_t size = gStaMsgQueueSize;
  xSemaphoreGive(gStaMsgQueueMutex);
  return size;
}

static int hexNibble(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

static void appendUtf8Codepoint(String &out, uint16_t codepoint)
{
  if (codepoint <= 0x7F)
  {
    out += static_cast<char>(codepoint);
    return;
  }
  if (codepoint <= 0x7FF)
  {
    out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
    return;
  }
  out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
  out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out += static_cast<char>(0x80 | (codepoint & 0x3F));
}

static bool extractJsonStringField(const String &json, const char *fieldName, String &outText)
{
  outText = "";
  if (!fieldName || !fieldName[0])
    return false;

  String key = "\"";
  key += fieldName;
  key += "\"";

  const int keyPos = json.indexOf(key);
  if (keyPos < 0)
    return false;

  int colon = json.indexOf(':', keyPos + key.length());
  if (colon < 0)
    return false;
  colon++;
  while (colon < static_cast<int>(json.length()) &&
         (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n'))
  {
    colon++;
  }
  if (colon >= static_cast<int>(json.length()) || json[colon] != '"')
    return false;

  String decoded;
  decoded.reserve(128);
  for (int i = colon + 1; i < static_cast<int>(json.length()); ++i)
  {
    const char c = json[i];
    if (c == '"')
    {
      decoded.trim();
      if (!decoded.length())
        return false;
      outText = decoded;
      return true;
    }
    if (c != '\\')
    {
      decoded += c;
      continue;
    }
    if (i + 1 >= static_cast<int>(json.length()))
      return false;
    const char esc = json[++i];
    switch (esc)
    {
    case '"':
    case '\\':
    case '/':
      decoded += esc;
      break;
    case 'b':
      decoded += '\b';
      break;
    case 'f':
      decoded += '\f';
      break;
    case 'n':
      decoded += '\n';
      break;
    case 'r':
      decoded += '\r';
      break;
    case 't':
      decoded += '\t';
      break;
    case 'u':
    {
      if (i + 4 >= static_cast<int>(json.length()))
        return false;
      const int h0 = hexNibble(json[i + 1]);
      const int h1 = hexNibble(json[i + 2]);
      const int h2 = hexNibble(json[i + 3]);
      const int h3 = hexNibble(json[i + 4]);
      if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
        return false;
      const uint16_t cp = static_cast<uint16_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
      appendUtf8Codepoint(decoded, cp);
      i += 4;
      break;
    }
    default:
      decoded += esc;
      break;
    }
  }

  return false;
}

static bool extractJsonTextField(const String &json, String &outText)
{
  if (!extractJsonStringField(json, "text", outText))
    return false;
  outText.trim();
  return outText.length() > 0;
}

static String percentEncodeUriComponent(const String &value)
{
  static constexpr char kHex[] = "0123456789ABCDEF";
  String out;
  out.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i)
  {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' ||
                      c == '_' || c == '.' || c == '~';
    if (safe)
    {
      out += static_cast<char>(c);
      continue;
    }
    out += '%';
    out += kHex[(c >> 4) & 0x0F];
    out += kHex[c & 0x0F];
  }
  return out;
}

static String normalizeMacAddressForApi(const String &raw)
{
  String compact;
  compact.reserve(12);
  for (size_t i = 0; i < raw.length(); ++i)
  {
    const char ch = raw[i];
    if (isxdigit(static_cast<unsigned char>(ch)))
    {
      compact += static_cast<char>(toupper(static_cast<unsigned char>(ch)));
      continue;
    }
    if (ch == ':' || ch == '-' || ch == ' ')
    {
      continue;
    }
    return "";
  }
  if (compact.length() != 12)
    return "";

  String mac;
  mac.reserve(17);
  for (int i = 0; i < 12; i += 2)
  {
    if (i > 0)
      mac += ':';
    mac += compact[i];
    mac += compact[i + 1];
  }
  return mac;
}

static String appendDeviceIdentityToApiUrl(const String &baseUrl)
{
  String out = baseUrl;
  if (!out.length())
    return out;

  const String mac = normalizeMacAddressForApi(WiFi.macAddress());
  String uuid;
  if (!deviceUuidRead(uuid))
  {
    uuid = "";
  }
  uuid.trim();
  uuid.toUpperCase();

  if (!uuid.length() && !mac.length())
  {
    return out;
  }

  String lower = out;
  lower.toLowerCase();
  const bool hasUuidParam = (lower.indexOf("uuid=") >= 0);
  const bool hasMacParam = (lower.indexOf("mac=") >= 0);
  if ((hasUuidParam || !uuid.length()) && (hasMacParam || !mac.length()))
  {
    return out;
  }

  const bool hasQuery = out.indexOf('?') >= 0;
  char sep = hasQuery ? '&' : '?';
  if (!hasUuidParam && uuid.length())
  {
    out += sep;
    out += "uuid=";
    out += percentEncodeUriComponent(uuid);
    sep = '&';
  }
  if (!hasMacParam && mac.length())
  {
    out += sep;
    out += "mac=";
    out += percentEncodeUriComponent(mac);
  }
  return out;
}

static bool fetchStaMessageFromCloud(String &outMessage, bool &outIsBottleMessage)
{
  outMessage = "";
  outIsBottleMessage = false;
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String apiUrl = gStaNetApi;
  if (!apiUrl.length())
  {
    apiUrl = kStaCloudApiUrlDefault;
  }
  apiUrl.trim();
  String apiUrlLower = apiUrl;
  apiUrlLower.toLowerCase();
  const bool useHttps = apiUrlLower.startsWith("https://");
  const bool useHttp = apiUrlLower.startsWith("http://");
  if (!useHttps && !useHttp)
  {
    Serial.printf("[STA] API URL scheme unsupported: %s\n", apiUrl.c_str());
    return false;
  }

  if (useHttps && !gStaHttpsClientReady)
  {
    gStaHttpsClient.setInsecure();
    gStaHttpsClient.setTimeout(kStaHttpReadTimeoutMs);
    gStaHttpsClientReady = true;
  }

  HTTPClient http;
  http.setConnectTimeout(kStaHttpConnectTimeoutMs);
  http.setTimeout(kStaHttpReadTimeoutMs);
  http.setReuse(true);
  const String requestUrl = appendDeviceIdentityToApiUrl(apiUrl);
  const bool beginOk = useHttps ? http.begin(gStaHttpsClient, requestUrl) : http.begin(requestUrl);
  if (!beginOk)
  {
    Serial.println("[STA] API HTTP begin failed");
    if (useHttps)
    {
      resetStaHttpClient();
    }
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.setUserAgent("TestTFT-ESP32S3/1.0");

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("[STA] API GET failed code=%d\n", httpCode);
    http.end();
    if (httpCode < 0 && useHttps)
    {
      resetStaHttpClient();
    }
    return false;
  }

  const String payload = http.getString();
  http.end();
  if (!extractJsonTextField(payload, outMessage))
  {
    Serial.printf("[STA] API JSON parse failed, len=%u\n", static_cast<unsigned int>(payload.length()));
    return false;
  }
  String source;
  if (extractJsonStringField(payload, "source", source))
  {
    source.trim();
    source.toLowerCase();
    outIsBottleMessage = (source == "device_queue");
  }
  return true;
}

static bool fetchAndQueueOneStaMessage()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;
  if (staMessageQueueSize() >= kStaPrefetchDepth)
    return false;
  const uint32_t nowMs = millis();
  if (gStaNextFetchAllowedMs != 0 && static_cast<int32_t>(nowMs - gStaNextFetchAllowedMs) < 0)
  {
    return false;
  }

  gStaFetchInProgress = true;
  String fetched;
  bool fetchedIsBottle = false;
  const bool fetchedOk = fetchStaMessageFromCloud(fetched, fetchedIsBottle);
  gStaFetchInProgress = false;

  if (!fetchedOk)
  {
    gStaNextFetchAllowedMs = nowMs + kStaFetchFailCooldownMs;
    return false;
  }
  if (!pushStaMessageQueue(fetched, fetchedIsBottle))
    return false;
  const size_t queued = staMessageQueueSize();
  gStaNextFetchAllowedMs = 0;
  Serial.printf("[STA] queued cloud message count=%u source=%s\n", static_cast<unsigned int>(queued),
                fetchedIsBottle ? "device_queue" : "main_random");
  return true;
}

static void staMessageFetcherTask(void *param)
{
  (void)param;
  while (true)
  {
    const bool shouldFetch = (gAppLoopMode == APP_MODE_STA_ONLINE) && (gStaOnlinePhase == StaOnlinePhase::kConnected) &&
                             (WiFi.status() == WL_CONNECTED);

    if (shouldFetch && staMessageQueueSize() < kStaPrefetchDepth)
    {
      while (staMessageQueueSize() < kStaPrefetchDepth)
      {
        if (!fetchAndQueueOneStaMessage())
        {
          break;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kStaFetcherTickMs));
  }
}

static void waitStaFetcherIdle(uint32_t maxWaitMs)
{
  const uint32_t started = millis();
  while (gStaFetchInProgress && (millis() - started) < maxWaitMs)
  {
    delay(10);
  }
}

static void ensureStaFetcherTaskStarted()
{
  if (gStaFetcherTaskHandle)
    return;
  if (!ensureStaQueueMutex())
  {
    Serial.println("[STA] queue mutex create failed");
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(staMessageFetcherTask, "StaMsgFetch", kStaFetcherStackSize, nullptr,
                                                kStaFetcherPriority, &gStaFetcherTaskHandle, kStaFetcherCore);
  if (ok != pdPASS)
  {
    gStaFetcherTaskHandle = nullptr;
    Serial.println("[STA] fetcher task create failed");
  }
}

static String trimIniValue(String value)
{
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0)
  {
    value = value.substring(0, semicolon);
  }
  value.trim();
  if (value.length() >= 2)
  {
    const char first = value[0];
    const char last = value[value.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
    {
      value = value.substring(1, value.length() - 1);
      value.trim();
    }
  }
  return value;
}

static bool parseAppModeFromIniValue(String value, AppLoopMode &outMode)
{
  value.trim();
  value.toLowerCase();
  value.replace("-", "_");
  value.replace(" ", "");

  if (value == "ap_config" || value == "ap_sta" || value == "ap")
  {
    outMode = APP_MODE_AP_STA;
    return true;
  }
  if (value == "sta_online" || value == "staonline")
  {
    outMode = APP_MODE_STA_ONLINE;
    return true;
  }
  if (value == "sta_only" || value == "staonly")
  {
    outMode = APP_MODE_STA_ONLY;
    return true;
  }
  return false;
}

static const char *appModeToIniValue(AppLoopMode mode)
{
  switch (mode)
  {
  case APP_MODE_AP_STA:
    return "AP_Config";
  case APP_MODE_STA_ONLINE:
    return "STA_Online";
  case APP_MODE_STA_ONLY:
    return "STA_Only";
  default:
    return "AP_Config";
  }
}

static bool loadAppModeFromSettingIni(AppLoopMode &outMode)
{
  outMode = APP_MODE_AP_STA;
  if (!fatMounted)
    return false;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
    return false;

  bool found = false;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length())
      continue;
    if (line.startsWith("#") || line.startsWith(";"))
      continue;

    const int eq = line.indexOf('=');
    if (eq <= 0)
      continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = trimIniValue(value);

    if (key == "mode")
    {
      AppLoopMode parsed = APP_MODE_AP_STA;
      if (parseAppModeFromIniValue(value, parsed))
      {
        outMode = parsed;
        found = true;
      }
      else
      {
        Serial.printf("[BOOT] invalid Mode in /setting.ini: %s\n", value.c_str());
      }
    }
  }
  f.close();
  return found;
}

static bool persistAppModeToSettingIni(AppLoopMode mode)
{
  if (!fatMounted)
    return false;

  String original;
  if (FFat.exists("/setting.ini"))
  {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf)
      return false;
    original = rf.readString();
    rf.close();
  }

  bool foundMode = false;
  const String modeLine = String("Mode = ") + appModeToIniValue(mode) + ";";
  String output;
  output.reserve(original.length() + 32);

  int start = 0;
  while (start <= original.length())
  {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() && !trimmed.startsWith("#") && !trimmed.startsWith(";"))
    {
      const int eq = trimmed.indexOf('=');
      if (eq > 0)
      {
        String key = trimmed.substring(0, eq);
        key.trim();
        key.toLowerCase();
        if (key == "mode")
        {
          line = modeLine;
          foundMode = true;
        }
      }
    }

    output += line;
    if (end >= 0)
    {
      output += '\n';
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  if (!foundMode)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += modeLine;
    output += '\n';
  }

  return writeTextFileAtomicallyToFat("/setting.ini", "/setting.tmp", output);
}

static bool loadStaCredentialsFromSettingIni(String &outSsid, String &outPassword, String &outNet)
{
  outSsid = "";
  outPassword = "";
  outNet = kStaCloudApiUrlDefault;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
  {
    Serial.println("[STA] /setting.ini not found");
    return false;
  }

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length())
      continue;
    if (line.startsWith("#") || line.startsWith(";"))
      continue;

    const int eq = line.indexOf('=');
    if (eq <= 0)
      continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value = trimIniValue(value);

    if (key == "netssid")
    {
      outSsid = value;
    }
    else if (key == "netpassword")
    {
      outPassword = value;
    }
    else if (key == "net")
    {
      String netLower = value;
      netLower.toLowerCase();
      if (netLower.startsWith("http://") || netLower.startsWith("https://"))
      {
        outNet = value;
      }
      else if (value.length())
      {
        Serial.printf("[STA] invalid Net in /setting.ini: %s\n", value.c_str());
      }
    }
  }
  f.close();

  return outSsid.length() > 0;
}

static bool tmToDs1302DateTime(const struct tm &in, Ds1302DateTime &out)
{
  out.year = static_cast<uint16_t>(in.tm_year + 1900);
  out.month = static_cast<uint8_t>(in.tm_mon + 1);
  out.day = static_cast<uint8_t>(in.tm_mday);
  out.hour = static_cast<uint8_t>(in.tm_hour);
  out.minute = static_cast<uint8_t>(in.tm_min);
  out.second = static_cast<uint8_t>(in.tm_sec);
  return ds1302IsValidDateTime(out);
}

static bool syncDs1302FromStaNtp(String &detailOut)
{
  detailOut = "";
  if (WiFi.status() != WL_CONNECTED)
  {
    detailOut = "wifi disconnected";
    return false;
  }

  const uint32_t nowMs = millis();
  if (gStaNtpLastAttemptMs != 0 && (nowMs - gStaNtpLastAttemptMs) < kStaNtpAttemptCooldownMs)
  {
    detailOut = gStaNtpSyncedThisSession ? "already synced" : "cooldown";
    return gStaNtpSyncedThisSession;
  }
  gStaNtpLastAttemptMs = nowMs;

  configTime(kStaNtpGmtOffsetSec, 0, kStaNtpServer1, kStaNtpServer2, kStaNtpServer3);
  struct tm tmNow = {};
  const uint32_t started = millis();
  bool gotTime = false;
  while ((millis() - started) < kStaNtpPollTimeoutMs)
  {
    if (getLocalTime(&tmNow, 300))
    {
      gotTime = true;
      break;
    }
    delay(60);
  }

  if (!gotTime)
  {
    detailOut = "ntp timeout";
    return false;
  }

  Ds1302DateTime synced;
  if (!tmToDs1302DateTime(tmNow, synced))
  {
    detailOut = "invalid ntp datetime";
    return false;
  }
  if (!rtc.writeDateTime(synced))
  {
    detailOut = "ds1302 write failed";
    return false;
  }
  String uuid;
  String uuidError;
  if (!deviceUuidEnsureFromNtpDateTime(synced, uuid, uuidError))
  {
    detailOut = "uuid save failed";
    if (uuidError.length())
    {
      detailOut += ": ";
      detailOut += uuidError;
    }
    return false;
  }

  gStaNtpSyncedThisSession = true;
  char buf[48] = {0};
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned int>(synced.year),
           static_cast<unsigned int>(synced.month), static_cast<unsigned int>(synced.day),
           static_cast<unsigned int>(synced.hour), static_cast<unsigned int>(synced.minute),
           static_cast<unsigned int>(synced.second));
  detailOut = String(buf);
  if (uuid.length())
  {
    detailOut += " UUID=";
    detailOut += uuid;
  }
  return true;
}
