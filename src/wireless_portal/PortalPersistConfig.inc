/*
 * 文件说明: 无线门户配置持久化实现。
 * 文件功能: 读取和保存 AP、STA、Host MAC 等 setting.ini 配置项。
 *
 * 函数表:
 * - loadApCredentialsFromSettingIni: 读取、获取或消费对应数据。
 * - persistHostMacToSettingIni: 保存、写入或更新对应数据。
 * - persistApConfigToSettingIni: 保存、写入或更新对应数据。
 */
void loadApCredentialsFromSettingIni()
{
  copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
  copyStringToBuf(String(kDefaultApPassword), gApPassword, sizeof(gApPassword));
  gApChannel = kDefaultApChannel;
  gEnableAp = true;
  gEnableEspNow = true;
  gInstantRefreshNoKey = false;
  applyHostMacFilterSetting("", false);

  bool persistedMode = gInstantRefreshNoKey;
  if (loadInstantRefreshModeFromPreferences(gInstantRefreshNoKey, persistedMode))
  {
    gInstantRefreshNoKey = persistedMode;
  }

  if (!fatMounted)
    return;
  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
  {
    Serial.println("[WEB] /setting.ini not found, using default AP config");
    return;
  }

  bool gotSsid = false;
  bool gotPassword = false;
  bool gotHostMac = false;
  bool gotChannel = false;
  String ssidValue;
  String passwordValue;
  String hostMacValue;
  String channelValue;

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
    value = stripIniValue(value);

    if (key == "ssip" || key == "ssid")
    {
      ssidValue = value;
      gotSsid = true;
    }
    else if (key == "password")
    {
      passwordValue = value;
      gotPassword = true;
    }
    else if (key == "hostmac")
    {
      hostMacValue = value;
      gotHostMac = true;
    }
    else if (key == "espnowchannel" || key == "channel" || key == "apchannel")
    {
      channelValue = value;
      gotChannel = true;
    }
    else if (key == "enableespnow")
    {
      bool parsed = true;
      if (parseBoolString(value, parsed))
      {
        gEnableEspNow = parsed;
      }
    }
  }
  f.close();
  removeLegacyInstantRefreshModeFromSettingIni();

  if (gotSsid)
  {
    if (ssidValue.length())
    {
      copyStringToBuf(ssidValue, gApSsid, sizeof(gApSsid));
    }
    else
    {
      copyStringToBuf(String(kDefaultApSsid), gApSsid, sizeof(gApSsid));
    }
  }

  if (gotPassword)
  {
    if (!passwordValue.length())
    {
      gApPassword[0] = '\0';
    }
    else if (passwordValue.length() >= 8)
    {
      copyStringToBuf(passwordValue, gApPassword, sizeof(gApPassword));
    }
    else
    {
      gApPassword[0] = '\0';
      Serial.println("[WEB] password length < 8, fallback to open AP");
    }
  }

  if (gotHostMac)
  {
    if (!applyHostMacFilterSetting(hostMacValue, true))
    {
      applyHostMacFilterSetting("", false);
    }
  }

  if (gotChannel)
  {
    uint8_t parsed = kDefaultApChannel;
    if (parseApChannel(channelValue, parsed))
    {
      gApChannel = parsed;
    }
    else
    {
      gApChannel = kDefaultApChannel;
      Serial.printf("[WEB] invalid channel in /setting.ini: %s, fallback=%u\n", channelValue.c_str(),
                    static_cast<unsigned int>(gApChannel));
    }
  }
}

bool persistHostMacToSettingIni(const String &hostMacRaw)
{
  if (!fatMounted)
    return false;

  String hostMac = hostMacRaw;
  hostMac.trim();
  if (hostMac.length())
  {
    uint8_t mac[6] = {0};
    if (!parseMacString(hostMac, mac))
      return false;
    hostMac = formatMacString(mac);
  }

  String original;
  if (FFat.exists("/setting.ini"))
  {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf)
      return false;
    original = rf.readString();
    rf.close();
  }

  bool foundHostMac = false;
  String output;
  output.reserve(original.length() + 48);

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
        if (key == "hostmac")
        {
          line = "HostMAC = \"" + hostMac + "\";";
          foundHostMac = true;
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

  if (!foundHostMac)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "HostMAC = \"" + hostMac + "\";\n";
  }

  return writeTextFileAtomically("/setting.ini", "/setting.tmp", output);
}

bool persistApConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, uint8_t channel)
{
  if (!fatMounted)
    return false;

  String ssid = ssidRaw;
  String password = passwordRaw;
  ssid.trim();
  password.trim();
  channel = clampApChannel(channel);

  if (!ssid.length())
    return false;
  if (ssid.length() >= sizeof(gApSsid))
    return false;
  if (password.length() && (password.length() < 8 || password.length() > 63))
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

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundChannel = false;
  String output;
  output.reserve(original.length() + 128);

  int start = 0;
  while (start <= original.length())
  {
    const int end = original.indexOf('\n', start);
    String line = (end >= 0) ? original.substring(start, end) : original.substring(start);
    bool keepLine = true;

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
        if (key == "ssip" || key == "ssid")
        {
          line = "SSID = \"" + ssid + "\";";
          foundSsid = true;
        }
        else if (key == "password")
        {
          line = "Password = \"" + password + "\";";
          foundPassword = true;
        }
        else if (key == "espnowchannel" || key == "channel" || key == "apchannel")
        {
          line = "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";";
          foundChannel = true;
        }
        else if (key == "enableap")
        {
          // EnableAP is no longer a setting.ini control item.
          // AP enablement is decided by current app mode.
          keepLine = false;
        }
      }
    }

    if (keepLine)
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

  if (!foundSsid)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "SSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "Password = \"" + password + "\";\n";
  }
  if (!foundChannel)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "EspNowChannel = " + String(static_cast<unsigned int>(channel)) + ";\n";
  }
  return writeTextFileAtomically("/setting.ini", "/setting.tmp", output);
}
