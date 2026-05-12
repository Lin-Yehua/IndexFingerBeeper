/*
 * 文件说明: 无线门户格式化与配置辅助实现。
 * 文件功能: 提供音量换算、INI 值清洗、JSON 转义、MAC 解析和 Host 过滤设置。
 *
 * 函数表:
 * - forceStaChannel: 应用配置或切换运行状态。
 * - gainToPercent: 计算、判断或转换对应结果。
 * - insertVolumePercent: 保存、写入或更新对应数据。
 * - bgVolumePercent: 模块内部辅助函数。
 * - masterVolumePercent: 模块内部辅助函数。
 * - percentToGain: 计算、判断或转换对应结果。
 * - applyVolumePercents: 应用配置或切换运行状态。
 * - stripIniValue: 解析、规范化或格式化对应内容。
 * - jsonEscape: 解析、规范化或格式化对应内容。
 * - formatMacString: 解析、规范化或格式化对应内容。
 * - parseMacString: 解析、规范化或格式化对应内容。
 * - applyHostMacFilterSetting: 应用配置或切换运行状态。
 * - isLegacyInstantRefreshSettingKey: 计算、判断或转换对应结果。
 * - removeLegacyInstantRefreshModeFromSettingIni: 模块内部辅助函数。
 * - loadInstantRefreshModeFromPreferences: 读取、获取或消费对应数据。
 * - persistInstantRefreshModeToPreferences: 保存、写入或更新对应数据。
 */
bool forceStaChannel(uint8_t channel)
{
  if (channel < kApChannelMin || channel > kApChannelMax)
  {
    Serial.printf("[ESPNOW] invalid channel: %u\n", static_cast<unsigned int>(channel));
    return false;
  }

  const esp_err_t setRet = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (setRet != ESP_OK)
  {
    Serial.printf("[ESPNOW] esp_wifi_set_channel failed: %d\n", static_cast<int>(setRet));
    return false;
  }

  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK)
  {
    Serial.printf("[ESPNOW] STA channel locked to %u\n", static_cast<unsigned int>(primary));
  }
  return true;
}

int gainToPercent(float gain)
{
  const float clamped = clampGain(gain);
  return static_cast<int>(clamped * 100.0f + 0.5f);
}

int insertVolumePercent()
{
  return gainToPercent(gInsertGain);
}

int bgVolumePercent()
{
  return gainToPercent(gBgGain);
}

int masterVolumePercent()
{
  return gainToPercent((gInsertGain + gBgGain) * 0.5f);
}

float percentToGain(int percent)
{
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  return static_cast<float>(percent) / 100.0f;
}

void applyVolumePercents(int insertPercent, int bgPercent)
{
  gInsertGain = percentToGain(insertPercent);
  gBgGain = percentToGain(bgPercent);
  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
}

String stripIniValue(String value)
{
  value.trim();
  const int semicolon = value.indexOf(';');
  if (semicolon >= 0)
    value = value.substring(0, semicolon);
  const int hash = value.indexOf('#');
  if (hash >= 0)
    value = value.substring(0, hash);
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

String jsonEscape(const String &value)
{
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i)
  {
    const char c = value[i];
    switch (c)
    {
    case '\"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
    {
      const uint8_t uc = static_cast<uint8_t>(c);
      if (uc < 0x20U)
      {
        char buf[7] = {0};
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(uc));
        escaped += buf;
      }
      else
      {
        escaped += c;
      }
      break;
    }
    }
  }
  return escaped;
}

String formatMacString(const uint8_t mac[6])
{
  char buf[18] = {0};
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool parseMacString(const String &text, uint8_t outMac[6])
{
  if (!outMac)
    return false;
  String s = text;
  s.trim();
  if (!s.length())
    return false;

  unsigned int b[6] = {0};
  int n = sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6)
  {
    n = sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  }
  if (n != 6)
    return false;

  for (int i = 0; i < 6; ++i)
  {
    if (b[i] > 0xFFU)
      return false;
    outMac[i] = static_cast<uint8_t>(b[i]);
  }
  return true;
}

bool applyHostMacFilterSetting(const String &rawMac, bool printOnError)
{
  String value = rawMac;
  value.trim();
  if (!value.length())
  {
    memset(gHostMacFilter, 0, sizeof(gHostMacFilter));
    gHostMacFilterEnabled = false;
    gHostMacFilterText[0] = '\0';
    return true;
  }

  uint8_t parsed[6] = {0};
  if (!parseMacString(value, parsed))
  {
    if (printOnError)
    {
      Serial.printf("[WEB] invalid HostMAC format: %s\n", value.c_str());
    }
    return false;
  }

  memcpy(gHostMacFilter, parsed, sizeof(gHostMacFilter));
  gHostMacFilterEnabled = true;
  const String fmt = formatMacString(gHostMacFilter);
  copyStringToBuf(fmt, gHostMacFilterText, sizeof(gHostMacFilterText));
  return true;
}

bool isLegacyInstantRefreshSettingKey(const String &key)
{
  return key == "instantrefreshnokey" || key == "instantrefresh";
}

void removeLegacyInstantRefreshModeFromSettingIni()
{
  if (!fatMounted)
    return;
  if (!FFat.exists("/setting.ini"))
    return;

  fs::File rf = FFat.open("/setting.ini", FILE_READ);
  if (!rf)
    return;
  const String original = rf.readString();
  rf.close();

  bool removed = false;
  String output;
  output.reserve(original.length());

  int start = 0;
  while (start <= original.length())
  {
    const int end = original.indexOf('\n', start);
    const String line = (end >= 0) ? original.substring(start, end) : original.substring(start);

    bool dropLine = false;
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
        if (isLegacyInstantRefreshSettingKey(key))
        {
          dropLine = true;
          removed = true;
        }
      }
    }

    if (!dropLine)
    {
      output += line;
      if (end >= 0)
      {
        output += '\n';
      }
    }

    if (end >= 0)
    {
      start = end + 1;
    }
    else
    {
      break;
    }
  }

  if (!removed)
    return;
  if (!writeTextFileAtomically("/setting.ini", "/setting.tmp", output))
    return;
  Serial.println("[WEB] removed legacy InstantRefreshNoKey from /setting.ini");
}

bool loadInstantRefreshModeFromPreferences(bool fallbackValue, bool &outValue)
{
  outValue = fallbackValue;
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, true))
  {
    Serial.println("[WEB] Preferences open (RO) failed for instantRefreshNoKey");
    return false;
  }
  outValue = prefs.getBool(kPrefsInstantRefreshNoKey, fallbackValue);
  prefs.end();
  return true;
}

bool persistInstantRefreshModeToPreferences(bool enabled)
{
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false))
  {
    return false;
  }
  const bool ok = prefs.putBool(kPrefsInstantRefreshNoKey, enabled) > 0;
  prefs.end();
  return ok;
}
