/*
 * 文件说明: 无线门户消息服务实现。
 * 文件功能: 处理 ESP-NOW 接收、CSV/日程文件读写、消息队列和设置保存接口。
 *
 * 函数表:
 * - onEspNowRecv: 事件回调处理函数。
 * - initEspNowReceiver: 初始化或确保对应资源可用。
 * - deinitEspNowReceiver: 停止、释放或清理对应状态。
 * - setCsvReloadRequested: 保存、写入或更新对应数据。
 * - setScheduleReloadRequested: 保存、写入或更新对应数据。
 * - takeCsvReloadRequested: 读取、获取或消费对应数据。
 * - takeScheduleReloadRequested: 读取、获取或消费对应数据。
 * - readDataCsvText: 读取、获取或消费对应数据。
 * - saveDataCsvText: 保存、写入或更新对应数据。
 * - readScheduleCsvText: 读取、获取或消费对应数据。
 * - saveScheduleCsvText: 保存、写入或更新对应数据。
 * - loadStaConfigFromSettingIni: 读取、获取或消费对应数据。
 * - persistStaConfigToSettingIni: 保存、写入或更新对应数据。
 * - rtcDateTimeJson: 模块内部辅助函数。
 * - parseRtcDateTimeFromRequest: 解析、规范化或格式化对应内容。
 * - enqueueMessageToQueue: 保存、写入或更新对应数据。
 * - enqueueRegularMessage: 保存、写入或更新对应数据。
 * - enqueueImmediateMessage: 保存、写入或更新对应数据。
 * - persistAudioGainsToSettingIni: 保存、写入或更新对应数据。
 * - persistEffectSettingsToSettingIni: 保存、写入或更新对应数据。
 * - statusJson: 模块内部辅助函数。
 * - apRootUrl: 模块内部辅助函数。
 * - redirectToPortal: 绘制界面、输出内容或响应请求。
 * - detectContentType: 计算、判断或转换对应结果。
 * - streamFromLittleFs: 绘制界面、输出内容或响应请求。
 * - handleStaticFile: 处理对应业务流程或事件分支。
 */
void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int dataLen)
{
  if (!gHostMessageQueue)
    return;
  if (!data || dataLen < static_cast<int>(sizeof(EspNowTextPacket)))
    return;

  if (gHostMacFilterEnabled)
  {
    if (!macAddr || memcmp(macAddr, gHostMacFilter, 6) != 0)
    {
      static uint32_t lastFilterDropLogMs = 0;
      const uint32_t now = millis();
      if (now - lastFilterDropLogMs >= 2000U)
      {
        lastFilterDropLogMs = now;
        String srcText = "<NULL>";
        if (macAddr)
          srcText = formatMacString(macAddr);
        Serial.printf("[ESPNOW] drop by HostMAC filter src=%s expect=%s\n", srcText.c_str(), gHostMacFilterText);
      }
      return;
    }
  }

  const EspNowTextPacket *pkt = reinterpret_cast<const EspNowTextPacket *>(data);
  if (pkt->magic != kEspNowTextMagic || pkt->type != kEspNowMsgTypeText)
    return;

  WebQueuedMessage msg = {};
  msg.text[0] = '\0';
  const size_t maxCopy = sizeof(msg.text) - 1;
  size_t n = strnlen(pkt->text, kEspNowTextMaxBytes);
  if (n > maxCopy)
    n = maxCopy;
  memcpy(msg.text, pkt->text, n);
  msg.text[n] = '\0';
  if (!msg.text[0])
    return;

  if (xQueueSend(gHostMessageQueue, &msg, 0) != pdTRUE)
  {
    WebQueuedMessage drop = {};
    (void)xQueueReceive(gHostMessageQueue, &drop, 0);
    (void)xQueueSend(gHostMessageQueue, &msg, 0);
  }
}

bool initEspNowReceiver()
{
  if (gEspNowReady)
    return true;
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[ESPNOW] init failed");
    return false;
  }
  if (esp_now_register_recv_cb(onEspNowRecv) != ESP_OK)
  {
    Serial.println("[ESPNOW] register recv callback failed");
    esp_now_deinit();
    return false;
  }
  gEspNowReady = true;
  Serial.println("[ESPNOW] receiver ready");
  return true;
}

void deinitEspNowReceiver()
{
  if (!gEspNowReady)
    return;
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  gEspNowReady = false;
}

void setCsvReloadRequested()
{
  portENTER_CRITICAL(&gFlagMux);
  gCsvReloadRequested = true;
  portEXIT_CRITICAL(&gFlagMux);
}

void setScheduleReloadRequested()
{
  portENTER_CRITICAL(&gFlagMux);
  gScheduleReloadRequested = true;
  portEXIT_CRITICAL(&gFlagMux);
}

bool takeCsvReloadRequested()
{
  bool value = false;
  portENTER_CRITICAL(&gFlagMux);
  value = gCsvReloadRequested;
  gCsvReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);
  return value;
}

bool takeScheduleReloadRequested()
{
  bool value = false;
  portENTER_CRITICAL(&gFlagMux);
  value = gScheduleReloadRequested;
  gScheduleReloadRequested = false;
  portEXIT_CRITICAL(&gFlagMux);
  return value;
}

String readDataCsvText()
{
  if (!fatMounted)
    return "";
  fs::File f = FFat.open("/data.csv", FILE_READ);
  if (!f)
    return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveDataCsvText(const String &content)
{
  if (!fatMounted)
    return false;
  if (!writeTextFileAtomically("/data.csv", "/data.tmp", content))
    return false;
  setCsvReloadRequested();
  return true;
}

String readScheduleCsvText()
{
  if (!fatMounted)
    return "";
  fs::File f = FFat.open("/schedule.csv", FILE_READ);
  if (!f)
    return "";
  String content = f.readString();
  f.close();
  return content;
}

bool saveScheduleCsvText(const String &content)
{
  if (!fatMounted)
    return false;
  if (!writeTextFileAtomically("/schedule.csv", "/schedule.tmp", content))
    return false;
  setScheduleReloadRequested();
  return true;
}

void loadStaConfigFromSettingIni(String &outSsid, String &outPassword, String &outNet)
{
  outSsid = "";
  outPassword = "";
  outNet = "";
  if (!fatMounted)
    return;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
    return;

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
      outNet = value;
    }
  }
  f.close();
}

bool persistStaConfigToSettingIni(const String &ssidRaw, const String &passwordRaw, const String &netRaw)
{
  if (!fatMounted)
    return false;

  String ssid = ssidRaw;
  String password = passwordRaw;
  String net = netRaw;
  ssid.trim();
  password.trim();
  net.trim();

  if (!ssid.length())
    return false;
  if (ssid.length() > 32)
    return false;
  if (password.length() && (password.length() < 8 || password.length() > 63))
    return false;
  if (net.length())
  {
    String netLower = net;
    netLower.toLowerCase();
    if (!netLower.startsWith("http://") && !netLower.startsWith("https://"))
      return false;
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

  bool foundSsid = false;
  bool foundPassword = false;
  bool foundNet = false;
  String output;
  output.reserve(original.length() + 192);

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
        if (key == "netssid")
        {
          line = "NetSSID = \"" + ssid + "\";";
          foundSsid = true;
        }
        else if (key == "netpassword")
        {
          line = "NetPassword = \"" + password + "\";";
          foundPassword = true;
        }
        else if (key == "net")
        {
          line = "Net = \"" + net + "\";";
          foundNet = true;
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

  if (!foundSsid)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "NetSSID = \"" + ssid + "\";\n";
  }
  if (!foundPassword)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "NetPassword = \"" + password + "\";\n";
  }
  if (!foundNet)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "Net = \"" + net + "\";\n";
  }

  return writeTextFileAtomically("/setting.ini", "/setting.tmp", output);
}

String rtcDateTimeJson()
{
  Ds1302DateTime dt;
  if (!rtc.readDateTime(dt) || !ds1302IsValidDateTime(dt))
  {
    return "{\"ok\":false}";
  }

  int week = static_cast<int>(
      (daysFromCivil(static_cast<int>(dt.year), static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day)) + 3LL) %
      7LL);
  if (week < 0)
    week += 7;
  week += 1;
  String out = "{\"ok\":true,\"year\":";
  out += String(static_cast<unsigned int>(dt.year));
  out += ",\"month\":";
  out += String(static_cast<unsigned int>(dt.month));
  out += ",\"day\":";
  out += String(static_cast<unsigned int>(dt.day));
  out += ",\"hour\":";
  out += String(static_cast<unsigned int>(dt.hour));
  out += ",\"minute\":";
  out += String(static_cast<unsigned int>(dt.minute));
  out += ",\"second\":";
  out += String(static_cast<unsigned int>(dt.second));
  out += ",\"week\":";
  out += String(static_cast<unsigned int>(week));
  out += "}";
  return out;
}

bool parseRtcDateTimeFromRequest(Ds1302DateTime &out, String &errorOut)
{
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  const String yearRaw = gWebServer->arg("year");
  const String monthRaw = gWebServer->arg("month");
  const String dayRaw = gWebServer->arg("day");
  const String hourRaw = gWebServer->arg("hour");
  const String minuteRaw = gWebServer->arg("minute");
  const String secondRaw = gWebServer->arg("second");
  if (!parseIntString(yearRaw, year) || year < 2000 || year > 2099)
  {
    errorOut = "invalid year";
    return false;
  }
  if (!parseIntString(monthRaw, month) || month < 1 || month > 12)
  {
    errorOut = "invalid month";
    return false;
  }
  if (!parseIntString(dayRaw, day) || day < 1 || day > 31)
  {
    errorOut = "invalid day";
    return false;
  }
  if (!parseIntString(hourRaw, hour) || hour < 0 || hour > 23)
  {
    errorOut = "invalid hour";
    return false;
  }
  if (!parseIntString(minuteRaw, minute) || minute < 0 || minute > 59)
  {
    errorOut = "invalid minute";
    return false;
  }
  if (!parseIntString(secondRaw, second) || second < 0 || second > 59)
  {
    errorOut = "invalid second";
    return false;
  }

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(hour);
  out.minute = static_cast<uint8_t>(minute);
  out.second = static_cast<uint8_t>(second);
  if (!ds1302IsValidDateTime(out))
  {
    errorOut = "datetime out of range";
    return false;
  }
  return true;
}

bool enqueueMessageToQueue(QueueHandle_t queue, const String &textRaw, String &errorOut)
{
  if (!queue)
  {
    errorOut = "queue not ready";
    return false;
  }

  String text = textRaw;
  text.trim();
  if (!text.length())
  {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen)
    text.remove(kTextMaxLen);

  WebQueuedMessage msg = {};
  text.toCharArray(msg.text, sizeof(msg.text));

  if (xQueueSend(queue, &msg, 0) != pdTRUE)
  {
    errorOut = "queue is full";
    return false;
  }
  return true;
}

bool enqueueRegularMessage(const String &textRaw, String &errorOut)
{
  return enqueueMessageToQueue(gMessageQueue, textRaw, errorOut);
}

bool enqueueImmediateMessage(const String &textRaw, String &errorOut)
{
  String text = textRaw;
  text.trim();
  if (!text.length())
  {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > kTextMaxLen)
    text.remove(kTextMaxLen);

  char local[kTextMaxLen + 1] = {0};
  text.toCharArray(local, sizeof(local));

  portENTER_CRITICAL(&gImmediateMessageMux);
  memcpy(gImmediateMessage.text, local, sizeof(gImmediateMessage.text));
  gImmediateMessage.text[sizeof(gImmediateMessage.text) - 1] = '\0';
  gImmediateMessage.pending = true;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  return true;
}

UBaseType_t immediatePendingCount()
{
  bool pending = false;
  portENTER_CRITICAL(&gImmediateMessageMux);
  pending = gImmediateMessage.pending;
  portEXIT_CRITICAL(&gImmediateMessageMux);
  return pending ? 1U : 0U;
}

bool persistAudioGainsToSettingIni()
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

  bool foundInsert = false;
  bool foundBg = false;
  String output;
  output.reserve(original.length() + 96);

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
        if (key == "insertgain")
        {
          line = "InsertGain = " + String(gInsertGain, 3) + ";";
          foundInsert = true;
        }
        else if (key == "backgroundgain")
        {
          line = "BackGroundGain = " + String(gBgGain, 3) + ";";
          foundBg = true;
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

  if (!foundInsert)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "InsertGain = " + String(gInsertGain, 3) + ";\n";
  }
  if (!foundBg)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "BackGroundGain = " + String(gBgGain, 3) + ";\n";
  }

  return writeTextFileAtomically("/setting.ini", "/setting.tmp", output);
}

bool persistEffectSettingsToSettingIni(int wrongProb3, int wrongProb5, bool enableReprint, float backlightLevel,
                                       int backlightTimeSec, int backlightCloseTimeSec, int sleepTimeMin)
{
  if (!fatMounted)
    return false;

  wrongProb3 = constrain(wrongProb3, 0, 100);
  wrongProb5 = constrain(wrongProb5, 0, 100);
  backlightLevel = clampGain(backlightLevel);
  if (backlightCloseTimeSec < 0)
    backlightCloseTimeSec = 0;
  if (sleepTimeMin < 0)
    sleepTimeMin = 0;

  String original;
  if (FFat.exists("/setting.ini"))
  {
    fs::File rf = FFat.open("/setting.ini", FILE_READ);
    if (!rf)
      return false;
    original = rf.readString();
    rf.close();
  }

  bool foundWrong3 = false;
  bool foundWrong5 = false;
  bool foundReprint = false;
  bool foundBacklight = false;
  bool foundBacklightTime = false;
  bool foundBacklightCloseTime = false;
  bool foundSleepTime = false;
  String output;
  output.reserve(original.length() + 240);

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
        if (key == "testwrongindexpersent_3area")
        {
          line = "TestWrongIndexPersent_3Area = " + String(wrongProb3) + ";";
          foundWrong3 = true;
        }
        else if (key == "testwrongindexpersent_5area")
        {
          line = "TestWrongIndexPersent_5Area = " + String(wrongProb5) + ";";
          foundWrong5 = true;
        }
        else if (key == "enablereprint")
        {
          line = String("EnableReprint = ") + (enableReprint ? "true;" : "false;");
          foundReprint = true;
        }
        else if (key == "backlight")
        {
          line = "BackLight = " + String(backlightLevel, 3) + ";";
          foundBacklight = true;
        }
        else if (key == "backlighttime")
        {
          line = "BacklightTime = " + String(backlightTimeSec) + ";";
          foundBacklightTime = true;
        }
        else if (key == "backlightclosetime")
        {
          line = "BacklightCloseTime = " + String(backlightCloseTimeSec) + ";";
          foundBacklightCloseTime = true;
        }
        else if (key == "sleeptime" || key == "sleepafteroffmin" || key == "backlightoffsleepmin")
        {
          line = "SleepTime = " + String(sleepTimeMin) + ";";
          foundSleepTime = true;
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

  if (!foundWrong3)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "TestWrongIndexPersent_3Area = " + String(wrongProb3) + ";\n";
  }
  if (!foundWrong5)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "TestWrongIndexPersent_5Area = " + String(wrongProb5) + ";\n";
  }
  if (!foundReprint)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += String("EnableReprint = ") + (enableReprint ? "true;\n" : "false;\n");
  }
  if (!foundBacklight)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "BackLight = " + String(backlightLevel, 3) + ";\n";
  }
  if (!foundBacklightTime)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "BacklightTime = " + String(backlightTimeSec) + ";\n";
  }
  if (!foundBacklightCloseTime)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "BacklightCloseTime = " + String(backlightCloseTimeSec) + ";\n";
  }
  if (!foundSleepTime)
  {
    if (output.length() && output[output.length() - 1] != '\n')
      output += '\n';
    output += "SleepTime = " + String(sleepTimeMin) + ";\n";
  }

  return writeTextFileAtomically("/setting.ini", "/setting.tmp", output);
}

String statusJson()
{
  const String staMac = WiFi.macAddress();
  const String apMac = WiFi.softAPmacAddress();
  String deviceUuid;
  if (!deviceUuidRead(deviceUuid))
  {
    deviceUuid = "";
  }
  const UBaseType_t regularQueued = gMessageQueue ? uxQueueMessagesWaiting(gMessageQueue) : 0;
  const UBaseType_t immediateQueued = immediatePendingCount();
  const UBaseType_t queued = regularQueued + immediateQueued;
  const UBaseType_t hostQueued = gHostMessageQueue ? uxQueueMessagesWaiting(gHostMessageQueue) : 0;
  BatteryStatus batteryStatus{};
  const bool batteryOk = appGetBatteryStatus(batteryStatus);
  bool pendingReload = false;
  bool imagePending = false;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
  int16_t imageCenterX = kImageDefaultCenterX;
  int16_t imageCenterY = kImageDefaultCenterY;
  portENTER_CRITICAL(&gFlagMux);
  pendingReload = gCsvReloadRequested;
  portEXIT_CRITICAL(&gFlagMux);
  if (gImageMutex && xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(10)) == pdTRUE)
  {
    imagePending = gPendingImage.ready;
    imageWidth = gPendingImage.width;
    imageHeight = gPendingImage.height;
    imageCenterX = gPendingImage.centerX;
    imageCenterY = gPendingImage.centerY;
    xSemaphoreGive(gImageMutex);
  }

  String out = "{\"queue\":";
  out += String(static_cast<unsigned int>(queued));
  out += ",\"regularQueue\":";
  out += String(static_cast<unsigned int>(regularQueued));
  out += ",\"immediateQueue\":";
  out += String(static_cast<unsigned int>(immediateQueued));
  out += ",\"csvReloadPending\":";
  out += pendingReload ? "true" : "false";
  out += ",\"hostQueue\":";
  out += String(static_cast<unsigned int>(hostQueued));
  out += ",\"batteryOk\":";
  out += batteryOk ? "true" : "false";
  out += ",\"batteryPercent\":";
  out += String(batteryStatus.percent);
  out += ",\"batteryCharging\":";
  out += batteryStatus.charging ? "true" : "false";
  out += ",\"batteryVin\":";
  out += String(batteryStatus.vinVoltage, 3);
  out += ",\"batteryVinFiltered\":";
  out += String(batteryStatus.filteredVinVoltage, 3);
  out += ",\"volume\":";
  out += String(masterVolumePercent());
  out += ",\"insertVolume\":";
  out += String(insertVolumePercent());
  out += ",\"bgVolume\":";
  out += String(bgVolumePercent());
  out += ",\"instantRefreshNoKey\":";
  out += gInstantRefreshNoKey ? "true" : "false";
  out += ",\"hostMac\":\"";
  out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
  out += "\"";
  out += ",\"apSsid\":\"";
  out += jsonEscape(String(gApSsid));
  out += "\",\"apChannel\":";
  out += String(static_cast<unsigned int>(gApChannel));
  out += ",\"apPasswordSet\":";
  out += gApPassword[0] ? "true" : "false";
  out += ",\"enableAp\":";
  out += gEnableAp ? "true" : "false";
  out += ",\"enableEspNow\":";
  out += gEnableEspNow ? "true" : "false";
  out += ",\"wrongProb3\":";
  out += String(gWrongProb3);
  out += ",\"wrongProb5\":";
  out += String(gWrongProb5);
  out += ",\"enableReprint\":";
  out += gEnableReprint ? "true" : "false";
  out += ",\"backlight\":";
  out += String(clampGain(gBacklightLevel), 3);
  out += ",\"backlightTime\":";
  out += String(gBacklightTimeSec);
  out += ",\"backlightCloseTime\":";
  out += String(gBacklightCloseTimeSec);
  out += ",\"sleepAfterOffMin\":";
  out += String(gSleepTimeMin);
  out += ",\"imagePending\":";
  out += imagePending ? "true" : "false";
  out += ",\"imageWidth\":";
  out += String(static_cast<unsigned int>(imageWidth));
  out += ",\"imageHeight\":";
  out += String(static_cast<unsigned int>(imageHeight));
  out += ",\"imageCenterX\":";
  out += String(imageCenterX);
  out += ",\"imageCenterY\":";
  out += String(imageCenterY);
  out += ",\"selfMac\":\"";
  // Keep selfMac stable for ESP-NOW targeting: always use STA MAC.
  out += jsonEscape(staMac);
  out += "\"";
  out += ",\"selfStaMac\":\"";
  out += jsonEscape(staMac);
  out += "\"";
  out += ",\"selfApMac\":\"";
  out += jsonEscape(apMac);
  out += "\"";
  out += ",\"deviceUuid\":\"";
  out += jsonEscape(deviceUuid);
  out += "\"";
  out += "}";
  return out;
}

String apRootUrl()
{
  String url = "http://";
  url += WiFi.softAPIP().toString();
  url += "/";
  return url;
}

void redirectToPortal()
{
  gWebServer->sendHeader("Location", apRootUrl(), true);
  gWebServer->send(302, "text/plain", "");
}

String detectContentType(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html; charset=utf-8";
  if (path.endsWith(".css"))
    return "text/css";
  if (path.endsWith(".js"))
    return "application/javascript";
  if (path.endsWith(".json"))
    return "application/json";
  if (path.endsWith(".txt"))
    return "text/plain; charset=utf-8";
  if (path.endsWith(".png"))
    return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg"))
    return "image/jpeg";
  if (path.endsWith(".svg"))
    return "image/svg+xml";
  return "application/octet-stream";
}

bool streamFromLittleFs(const String &path)
{
  if (!LittleFS.exists(path))
    return false;
  fs::File f = LittleFS.open(path, FILE_READ);
  if (!f)
    return false;
  gWebServer->sendHeader("Cache-Control", "no-store");
  gWebServer->streamFile(f, detectContentType(path));
  f.close();
  return true;
}

void handleStaticFile(const String &path)
{
  if (!streamFromLittleFs(path))
  {
    gWebServer->send(404, "text/plain", "file not found");
  }
}

// 注册首页、静态资源和基础状态接口。
