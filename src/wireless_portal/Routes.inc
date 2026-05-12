/*
 * 文件说明: 无线门户 HTTP 路由实现。
 * 文件功能: 按功能组注册静态资源、状态、RTC、配置、CSV/日程、消息和图片上传 API。
 *
 * 函数表:
 * - registerStaticRoutes: 注册对应功能组的路由或回调。
 * - registerRtcRoutes: 注册对应功能组的路由或回调。
 * - registerBatteryRoutes: 注册对应功能组的路由或回调。
 * - registerVolumeRoutes: 注册对应功能组的路由或回调。
 * - registerEffectRoutes: 注册对应功能组的路由或回调。
 * - registerConfigRoutes: 注册对应功能组的路由或回调。
 * - registerDataRoutes: 注册对应功能组的路由或回调。
 * - registerFallbackRoutes: 注册对应功能组的路由或回调。
 * - registerRoutes: 统一注册无线门户 HTTP 路由。
 */
void registerStaticRoutes()
{
  gWebServer->on("/", HTTP_GET,
                 []()
                 {
                   if (!streamFromLittleFs("/index.html"))
                   {
                     gWebServer->send(500, "text/plain", "missing /index.html in LittleFS");
                   }
                 });

  gWebServer->on("/index.html", HTTP_GET, []() { handleStaticFile("/index.html"); });
  gWebServer->on("/style.css", HTTP_GET, []() { handleStaticFile("/style.css"); });
  gWebServer->on("/app.js", HTTP_GET, []() { handleStaticFile("/app.js"); });
  gWebServer->on("/favicon.ico", HTTP_GET, []() { gWebServer->send(204, "text/plain", ""); });
  gWebServer->on("/apple-touch-icon.png", HTTP_GET, []() { gWebServer->send(204, "text/plain", ""); });

  gWebServer->on("/api/status", HTTP_GET, []() { gWebServer->send(200, "application/json", statusJson()); });
}

// 注册 RTC 读取、写入和手机时间同步接口。
void registerRtcRoutes()
{
  gWebServer->on("/api/rtc", HTTP_GET, []() { gWebServer->send(200, "application/json", rtcDateTimeJson()); });

  gWebServer->on("/api/rtc/set", HTTP_POST,
                 []()
                 {
                   Ds1302DateTime dt;
                   String err;
                   if (!parseRtcDateTimeFromRequest(dt, err))
                   {
                     gWebServer->send(400, "text/plain", err);
                     return;
                   }
                   if (!rtc.writeDateTime(dt))
                   {
                     gWebServer->send(500, "text/plain", "DS1302 write failed");
                     return;
                   }
                   // RTC adjusted manually, force schedule slots to rebuild against new time.
                   setScheduleReloadRequested();
                   gWebServer->send(200, "application/json", rtcDateTimeJson());
                 });

  gWebServer->on("/api/rtc/sync-phone", HTTP_POST,
                 []()
                 {
                   Ds1302DateTime dt;
                   String err;
                   if (!parseRtcDateTimeFromRequest(dt, err))
                   {
                     gWebServer->send(400, "text/plain", err);
                     return;
                   }
                   if (!rtc.writeDateTime(dt))
                   {
                     gWebServer->send(500, "text/plain", "DS1302 write failed");
                     return;
                   }
                   String uuid;
                   String uuidError;
                   if (!deviceUuidEnsureFromDateTime(dt, uuid, uuidError))
                   {
                     String msg = "UUID save failed";
                     if (uuidError.length())
                     {
                       msg += ": ";
                       msg += uuidError;
                     }
                     gWebServer->send(500, "text/plain", msg);
                     return;
                   }
                   // RTC adjusted manually, force schedule slots to rebuild against new time.
                   setScheduleReloadRequested();
                   gWebServer->send(200, "application/json", rtcDateTimeJson());
                 });
}

// 注册电池状态接口。
void registerBatteryRoutes()
{
  gWebServer->on("/api/battery", HTTP_GET,
                 []()
                 {
                   BatteryStatus batteryStatus{};
                   const bool ok = appGetBatteryStatus(batteryStatus);
                   if (!ok)
                   {
                     gWebServer->send(200, "application/json", "{\"ok\":false,\"message\":\"battery not ready\"}");
                     return;
                   }

                   String out = "{\"ok\":true,\"percent\":";
                   out += String(batteryStatus.percent);
                   out += ",\"charging\":";
                   out += batteryStatus.charging ? "true" : "false";
                   out += ",\"vinVoltage\":";
                   out += String(batteryStatus.vinVoltage, 3);
                   out += ",\"filteredVinVoltage\":";
                   out += String(batteryStatus.filteredVinVoltage, 3);
                   out += ",\"pinVoltage\":";
                   out += String(batteryStatus.pinVoltage, 3);
                   out += ",\"rawAdc\":";
                   out += String(batteryStatus.rawAdc);
                   out += ",\"updatedMs\":";
                   out += String(batteryStatus.updatedMs);
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });
}

// 注册音量读写接口。
void registerVolumeRoutes()
{
  gWebServer->on("/api/volume", HTTP_GET,
                 []()
                 {
                   String out = "{\"insertVolume\":";
                   out += String(insertVolumePercent());
                   out += ",\"bgVolume\":";
                   out += String(bgVolumePercent());
                   out += ",\"volume\":";
                   out += String(masterVolumePercent());
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/volume", HTTP_POST,
                 []()
                 {
                   String insertRaw = gWebServer->arg("insertVolume");
                   if (!insertRaw.length() && gWebServer->hasArg("insert"))
                   {
                     insertRaw = gWebServer->arg("insert");
                   }

                   String bgRaw = gWebServer->arg("bgVolume");
                   if (!bgRaw.length() && gWebServer->hasArg("backgroundVolume"))
                   {
                     bgRaw = gWebServer->arg("backgroundVolume");
                   }

                   String value = gWebServer->arg("volume");
                   String persistArg = gWebServer->arg("persist");
                   if (!value.length() && !insertRaw.length() && !bgRaw.length() && gWebServer->hasArg("plain"))
                   {
                     value = gWebServer->arg("plain");
                   }

                   bool persist = true;
                   persistArg.trim();
                   persistArg.toLowerCase();
                   if (persistArg == "0" || persistArg == "false" || persistArg == "off" || persistArg == "no")
                   {
                     persist = false;
                   }

                   bool hasAny = false;
                   int nextInsert = insertVolumePercent();
                   int nextBg = bgVolumePercent();

                   insertRaw.trim();
                   if (insertRaw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(insertRaw, parsed) || parsed < 0 || parsed > 100)
                     {
                       gWebServer->send(400, "text/plain", "insertVolume must be 0-100");
                       return;
                     }
                     nextInsert = parsed;
                     hasAny = true;
                   }

                   bgRaw.trim();
                   if (bgRaw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(bgRaw, parsed) || parsed < 0 || parsed > 100)
                     {
                       gWebServer->send(400, "text/plain", "bgVolume must be 0-100");
                       return;
                     }
                     nextBg = parsed;
                     hasAny = true;
                   }

                   value.trim();
                   if (value.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(value, parsed) || parsed < 0 || parsed > 100)
                     {
                       gWebServer->send(400, "text/plain", "volume must be 0-100");
                       return;
                     }
                     if (!insertRaw.length())
                       nextInsert = parsed;
                     if (!bgRaw.length())
                       nextBg = parsed;
                     hasAny = true;
                   }

                   if (!hasAny)
                   {
                     gWebServer->send(400, "text/plain", "volume is empty");
                     return;
                   }

                   applyVolumePercents(nextInsert, nextBg);
                   if (persist && !persistAudioGainsToSettingIni())
                   {
                     gWebServer->send(500, "text/plain", "volume applied but save /setting.ini failed");
                     return;
                   }

                   String out = "{\"insertVolume\":";
                   out += String(insertVolumePercent());
                   out += ",\"bgVolume\":";
                   out += String(bgVolumePercent());
                   out += ",\"volume\":";
                   out += String(masterVolumePercent());
                   out += ",\"persisted\":";
                   out += persist ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });
}

// 注册刷新模式和显示效果配置接口。
void registerEffectRoutes()
{
  gWebServer->on("/api/refreshmode", HTTP_GET,
                 []()
                 {
                   String out = "{\"instantRefreshNoKey\":";
                   out += gInstantRefreshNoKey ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/refreshmode", HTTP_POST,
                 []()
                 {
                   String raw = gWebServer->arg("instantRefreshNoKey");
                   if (!raw.length() && gWebServer->hasArg("instantrefreshnokey"))
                   {
                     raw = gWebServer->arg("instantrefreshnokey");
                   }
                   if (!raw.length() && gWebServer->hasArg("instantRefresh"))
                   {
                     raw = gWebServer->arg("instantRefresh");
                   }
                   if (!raw.length() && gWebServer->hasArg("plain"))
                   {
                     raw = gWebServer->arg("plain");
                   }
                   raw.trim();

                   bool next = gInstantRefreshNoKey;
                   if (!parseBoolString(raw, next))
                   {
                     gWebServer->send(400, "text/plain", "invalid instantRefreshNoKey");
                     return;
                   }

                   gInstantRefreshNoKey = next;
                   if (!persistInstantRefreshModeToPreferences(gInstantRefreshNoKey))
                   {
                     gWebServer->send(500, "text/plain", "mode applied but save Preferences failed");
                     return;
                   }

                   String out = "{\"instantRefreshNoKey\":";
                   out += gInstantRefreshNoKey ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/effects", HTTP_GET,
                 []()
                 {
                   String out = "{\"wrongProb3\":";
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
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/effects", HTTP_POST,
                 []()
                 {
                   int nextWrong3 = gWrongProb3;
                   int nextWrong5 = gWrongProb5;
                   bool nextEnableReprint = gEnableReprint;
                   float nextBacklightLevel = gBacklightLevel;
                   int nextBacklightTime = gBacklightTimeSec;
                   int nextBacklightCloseTime = gBacklightCloseTimeSec;
                   int nextSleepTimeMin = gSleepTimeMin;
                   bool hasAny = false;

                   String wrong3Raw = gWebServer->arg("wrongProb3");
                   if (!wrong3Raw.length() && gWebServer->hasArg("TestWrongIndexPersent_3Area"))
                   {
                     wrong3Raw = gWebServer->arg("TestWrongIndexPersent_3Area");
                   }
                   wrong3Raw.trim();
                   if (wrong3Raw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(wrong3Raw, parsed) || parsed < 0 || parsed > 100)
                     {
                       gWebServer->send(400, "text/plain", "wrongProb3 must be 0-100");
                       return;
                     }
                     nextWrong3 = parsed;
                     hasAny = true;
                   }

                   String wrong5Raw = gWebServer->arg("wrongProb5");
                   if (!wrong5Raw.length() && gWebServer->hasArg("TestWrongIndexPersent_5Area"))
                   {
                     wrong5Raw = gWebServer->arg("TestWrongIndexPersent_5Area");
                   }
                   wrong5Raw.trim();
                   if (wrong5Raw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(wrong5Raw, parsed) || parsed < 0 || parsed > 100)
                     {
                       gWebServer->send(400, "text/plain", "wrongProb5 must be 0-100");
                       return;
                     }
                     nextWrong5 = parsed;
                     hasAny = true;
                   }

                   String reprintRaw = gWebServer->arg("enableReprint");
                   if (!reprintRaw.length() && gWebServer->hasArg("EnableReprint"))
                   {
                     reprintRaw = gWebServer->arg("EnableReprint");
                   }
                   reprintRaw.trim();
                   if (reprintRaw.length())
                   {
                     bool parsed = false;
                     if (!parseBoolString(reprintRaw, parsed))
                     {
                       gWebServer->send(400, "text/plain", "invalid enableReprint");
                       return;
                     }
                     nextEnableReprint = parsed;
                     hasAny = true;
                   }

                   String backlightRaw = gWebServer->arg("backlightTime");
                   if (!backlightRaw.length() && gWebServer->hasArg("BacklightTime"))
                   {
                     backlightRaw = gWebServer->arg("BacklightTime");
                   }
                   backlightRaw.trim();
                   if (backlightRaw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(backlightRaw, parsed))
                     {
                       gWebServer->send(400, "text/plain", "invalid backlightTime");
                       return;
                     }
                     nextBacklightTime = parsed;
                     hasAny = true;
                   }

                   String backlightCloseRaw = gWebServer->arg("backlightCloseTime");
                   if (!backlightCloseRaw.length() && gWebServer->hasArg("BacklightCloseTime"))
                   {
                     backlightCloseRaw = gWebServer->arg("BacklightCloseTime");
                   }
                   backlightCloseRaw.trim();
                   if (backlightCloseRaw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(backlightCloseRaw, parsed) || parsed < 0)
                     {
                       gWebServer->send(400, "text/plain", "invalid backlightCloseTime");
                       return;
                     }
                     nextBacklightCloseTime = parsed;
                     hasAny = true;
                   }

                   String sleepTimeRaw = gWebServer->arg("sleepAfterOffMin");
                   if (!sleepTimeRaw.length() && gWebServer->hasArg("SleepTime"))
                   {
                     sleepTimeRaw = gWebServer->arg("SleepTime");
                   }
                   sleepTimeRaw.trim();
                   if (sleepTimeRaw.length())
                   {
                     int parsed = 0;
                     if (!parseIntString(sleepTimeRaw, parsed) || parsed < 0)
                     {
                       gWebServer->send(400, "text/plain", "invalid sleepAfterOffMin");
                       return;
                     }
                     nextSleepTimeMin = parsed;
                     hasAny = true;
                   }

                   String backlightLevelRaw = gWebServer->arg("backlight");
                   if (!backlightLevelRaw.length() && gWebServer->hasArg("BackLight"))
                   {
                     backlightLevelRaw = gWebServer->arg("BackLight");
                   }
                   backlightLevelRaw.trim();
                   if (backlightLevelRaw.length())
                   {
                     float parsed = 0.0f;
                     if (!parseFloatString(backlightLevelRaw, parsed) || parsed < 0.0f || parsed > 1.0f)
                     {
                       gWebServer->send(400, "text/plain", "backlight must be 0.0-1.0");
                       return;
                     }
                     nextBacklightLevel = parsed;
                     hasAny = true;
                   }

                   if (!hasAny)
                   {
                     gWebServer->send(400, "text/plain", "no effect settings provided");
                     return;
                   }

                   gWrongProb3 = nextWrong3;
                   gWrongProb5 = nextWrong5;
                   gEnableReprint = nextEnableReprint;
                   setBacklightLevel(nextBacklightLevel);
                   setBacklightTimeSeconds(nextBacklightTime);
                   gBacklightCloseTimeSec = nextBacklightCloseTime;
                   gSleepTimeMin = nextSleepTimeMin;

                   if (!persistEffectSettingsToSettingIni(gWrongProb3, gWrongProb5, gEnableReprint, gBacklightLevel,
                                                          gBacklightTimeSec, gBacklightCloseTimeSec, gSleepTimeMin))
                   {
                     gWebServer->send(500, "text/plain", "settings applied but save /setting.ini failed");
                     return;
                   }

                   String out = "{\"wrongProb3\":";
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
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });
}

// 注册 Host MAC、AP 和 STA 网络配置接口。
void registerConfigRoutes()
{
  gWebServer->on("/api/hostmac", HTTP_GET,
                 []()
                 {
                   String out = "{\"hostMac\":\"";
                   out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
                   out += "\",\"enabled\":";
                   out += gHostMacFilterEnabled ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/hostmac", HTTP_POST,
                 []()
                 {
                   String hostMac = gWebServer->arg("hostMac");
                   if (!hostMac.length() && gWebServer->hasArg("hostmac"))
                   {
                     hostMac = gWebServer->arg("hostmac");
                   }
                   if (!hostMac.length() && gWebServer->hasArg("plain"))
                   {
                     hostMac = gWebServer->arg("plain");
                   }
                   hostMac.trim();

                   if (!applyHostMacFilterSetting(hostMac, false))
                   {
                     gWebServer->send(400, "text/plain", "invalid hostMac format");
                     return;
                   }
                   if (!persistHostMacToSettingIni(hostMac))
                   {
                     gWebServer->send(500, "text/plain", "hostMac applied but save /setting.ini failed");
                     return;
                   }

                   String out = "{\"hostMac\":\"";
                   out += jsonEscape(gHostMacFilterEnabled ? String(gHostMacFilterText) : "");
                   out += "\",\"enabled\":";
                   out += gHostMacFilterEnabled ? "true" : "false";
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/apconfig", HTTP_GET,
                 []()
                 {
                   String out = "{\"ssid\":\"";
                   out += jsonEscape(String(gApSsid));
                   out += "\",\"password\":\"";
                   out += jsonEscape(String(gApPassword));
                   out += "\",\"passwordSet\":";
                   out += gApPassword[0] ? "true" : "false";
                   out += ",\"channel\":";
                   out += String(static_cast<unsigned int>(gApChannel));
                   out += "}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/apconfig", HTTP_POST,
                 []()
                 {
                   String ssid = gWebServer->arg("ssid");
                   if (!ssid.length() && gWebServer->hasArg("ssip"))
                   {
                     ssid = gWebServer->arg("ssip");
                   }
                   if (!ssid.length() && gWebServer->hasArg("plain"))
                   {
                     ssid = gWebServer->arg("plain");
                   }

                   String password = gWebServer->arg("password");
                   String channelArg = gWebServer->arg("channel");

                   ssid.trim();
                   password.trim();
                   channelArg.trim();

                   if (!ssid.length())
                   {
                     gWebServer->send(400, "text/plain", "ssid is empty");
                     return;
                   }
                   if (ssid.length() >= sizeof(gApSsid))
                   {
                     gWebServer->send(400, "text/plain", "ssid too long");
                     return;
                   }
                   if (password.length() && (password.length() < 8 || password.length() > 63))
                   {
                     gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
                     return;
                   }

                   uint8_t nextChannel = gApChannel;
                   if (channelArg.length() && !parseApChannel(channelArg, nextChannel))
                   {
                     gWebServer->send(400, "text/plain", "channel must be 1-13");
                     return;
                   }

                   if (!persistApConfigToSettingIni(ssid, password, nextChannel))
                   {
                     gWebServer->send(500, "text/plain", "save /setting.ini failed");
                     return;
                   }

                   copyStringToBuf(ssid, gApSsid, sizeof(gApSsid));
                   if (!password.length())
                   {
                     gApPassword[0] = '\0';
                   }
                   else
                   {
                     copyStringToBuf(password, gApPassword, sizeof(gApPassword));
                   }
                   gApChannel = nextChannel;

                   String out = "{\"ssid\":\"";
                   out += jsonEscape(String(gApSsid));
                   out += "\",\"password\":\"";
                   out += jsonEscape(String(gApPassword));
                   out += "\",\"passwordSet\":";
                   out += gApPassword[0] ? "true" : "false";
                   out += ",\"channel\":";
                   out += String(static_cast<unsigned int>(gApChannel));
                   out += ",\"rebootRequired\":true}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/staconfig", HTTP_GET,
                 []()
                 {
                   String staSsid;
                   String staPassword;
                   String staNet;
                   loadStaConfigFromSettingIni(staSsid, staPassword, staNet);

                   String out = "{\"ssid\":\"";
                   out += jsonEscape(staSsid);
                   out += "\",\"password\":\"";
                   out += jsonEscape(staPassword);
                   out += "\",\"passwordSet\":";
                   out += staPassword.length() ? "true" : "false";
                   out += ",\"net\":\"";
                   out += jsonEscape(staNet);
                   out += "\"}";
                   gWebServer->send(200, "application/json", out);
                 });

  gWebServer->on("/api/staconfig", HTTP_POST,
                 []()
                 {
                   String ssid = gWebServer->arg("ssid");
                   String password = gWebServer->arg("password");
                   const bool hasNetArg = gWebServer->hasArg("net") || gWebServer->hasArg("Net");
                   String net = gWebServer->arg("net");
                   if (!net.length() && gWebServer->hasArg("Net"))
                   {
                     net = gWebServer->arg("Net");
                   }
                   if (!ssid.length() && gWebServer->hasArg("plain"))
                   {
                     ssid = gWebServer->arg("plain");
                   }
                   ssid.trim();
                   password.trim();
                   net.trim();
                   if (!hasNetArg)
                   {
                     String currentSsid;
                     String currentPassword;
                     String currentNet;
                     loadStaConfigFromSettingIni(currentSsid, currentPassword, currentNet);
                     net = currentNet;
                   }

                   if (!ssid.length())
                   {
                     gWebServer->send(400, "text/plain", "ssid is empty");
                     return;
                   }
                   if (ssid.length() > 32)
                   {
                     gWebServer->send(400, "text/plain", "ssid too long");
                     return;
                   }
                   if (password.length() && (password.length() < 8 || password.length() > 63))
                   {
                     gWebServer->send(400, "text/plain", "password must be empty or 8-63 chars");
                     return;
                   }
                   if (net.length())
                   {
                     String netLower = net;
                     netLower.toLowerCase();
                     if (!netLower.startsWith("http://") && !netLower.startsWith("https://"))
                     {
                       gWebServer->send(400, "text/plain", "net must start with http:// or https://");
                       return;
                     }
                   }
                   if (!persistStaConfigToSettingIni(ssid, password, net))
                   {
                     gWebServer->send(500, "text/plain", "save /setting.ini failed");
                     return;
                   }

                   String out = "{\"ssid\":\"";
                   out += jsonEscape(ssid);
                   out += "\",\"password\":\"";
                   out += jsonEscape(password);
                   out += "\",\"passwordSet\":";
                   out += password.length() ? "true" : "false";
                   out += ",\"net\":\"";
                   out += jsonEscape(net);
                   out += "\"}";
                   gWebServer->send(200, "application/json", out);
                 });
}

// 注册 CSV、日程、消息和图片上传接口。
void registerDataRoutes()
{
  gWebServer->on("/api/csv", HTTP_GET,
                 []()
                 {
                   if (!fatMounted)
                   {
                     gWebServer->send(503, "text/plain", "FAT not mounted");
                     return;
                   }
                   const String csvText = readDataCsvText();
                   gWebServer->send(200, "text/plain", csvText);
                 });

  gWebServer->on("/api/csv", HTTP_POST,
                 []()
                 {
                   String content = gWebServer->arg("content");
                   if (!content.length() && gWebServer->hasArg("plain"))
                   {
                     content = gWebServer->arg("plain");
                   }
                   if (!content.length())
                   {
                     gWebServer->send(400, "text/plain", "content is empty");
                     return;
                   }
                   if (!saveDataCsvText(content))
                   {
                     gWebServer->send(500, "text/plain", "save failed");
                     return;
                   }
                   gWebServer->send(200, "text/plain", "saved /data.csv, reload scheduled");
                 });

  gWebServer->on("/api/schedule", HTTP_GET,
                 []()
                 {
                   if (!fatMounted)
                   {
                     gWebServer->send(503, "text/plain", "FAT not mounted");
                     return;
                   }
                   gWebServer->send(200, "text/plain", readScheduleCsvText());
                 });

  gWebServer->on("/api/schedule", HTTP_POST,
                 []()
                 {
                   String content = gWebServer->arg("content");
                   if (!content.length() && gWebServer->hasArg("plain"))
                   {
                     content = gWebServer->arg("plain");
                   }
                   if (!saveScheduleCsvText(content))
                   {
                     gWebServer->send(500, "text/plain", "save failed");
                     return;
                   }
                   gWebServer->send(200, "text/plain", "saved /schedule.csv");
                 });

  gWebServer->on("/api/send", HTTP_POST,
                 []()
                 {
                   String error;
                   String text = gWebServer->arg("text");
                   if (!text.length() && gWebServer->hasArg("plain"))
                   {
                     text = gWebServer->arg("plain");
                   }
                   bool immediateMode = gInstantRefreshNoKey;
                   String immediateArg = gWebServer->arg("immediate");
                   if (!immediateArg.length() && gWebServer->hasArg("instant"))
                   {
                     immediateArg = gWebServer->arg("instant");
                   }
                   if (immediateArg.length())
                   {
                     bool parsed = immediateMode;
                     if (!parseBoolString(immediateArg, parsed))
                     {
                       gWebServer->send(400, "text/plain", "invalid immediate");
                       return;
                     }
                     immediateMode = parsed;
                   }
                   const bool ok =
                       immediateMode ? enqueueImmediateMessage(text, error) : enqueueRegularMessage(text, error);
                   if (!ok)
                   {
                     const int code = (error == "queue is full") ? 503 : 400;
                     gWebServer->send(code, "text/plain", error);
                     return;
                   }
                   const UBaseType_t regularQueued = gMessageQueue ? uxQueueMessagesWaiting(gMessageQueue) : 0;
                   const UBaseType_t immediateQueued = immediatePendingCount();
                   String reply = immediateMode ? "queued(immediate), waiting=" : "queued(regular), waiting=";
                   reply += String(static_cast<unsigned int>(regularQueued + immediateQueued));
                   reply += " (regular=";
                   reply += String(static_cast<unsigned int>(regularQueued));
                   reply += ", immediate=";
                   reply += String(static_cast<unsigned int>(immediateQueued));
                   reply += ")";
                   gWebServer->send(200, "text/plain", reply);
                 });

  gWebServer->on("/api/image", HTTP_POST, []() { handleImageUploadFinalize(); }, []() { handleImageUpload(); });
}

// 注册强制门户探测和兜底静态文件/重定向。
void registerFallbackRoutes()
{
  auto captiveRedirect = []() { redirectToPortal(); };

  gWebServer->on("/generate_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/gen_204", HTTP_GET, captiveRedirect);
  gWebServer->on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
  gWebServer->on("/connecttest.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/ncsi.txt", HTTP_GET, captiveRedirect);
  gWebServer->on("/fwlink", HTTP_GET, captiveRedirect);

  gWebServer->onNotFound(
      []()
      {
        const String uri = gWebServer->uri();
        if (uri.startsWith("/api/"))
        {
          gWebServer->send(404, "text/plain", "not found");
          return;
        }

        // Captive mini-browsers may probe odd private paths; skip filesystem lookup.
        if (uri.length() > 96 || uri.startsWith("/mmtls/") || uri.startsWith("/group/") || uri.startsWith("/cgi-bin/"))
        {
          gWebServer->send(204, "text/plain", "");
          return;
        }

        if (LittleFS.exists(uri))
        {
          handleStaticFile(uri);
          return;
        }

        redirectToPortal();
      });
}

void registerRoutes()
{
  registerStaticRoutes();
  registerRtcRoutes();
  registerBatteryRoutes();
  registerVolumeRoutes();
  registerEffectRoutes();
  registerConfigRoutes();
  registerDataRoutes();
  registerFallbackRoutes();
}
