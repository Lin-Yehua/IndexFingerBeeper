/*
 * 文件说明: USB MSC、开机动画和资源初始化私有实现。
 * 文件功能: 管理 USB 扇区读写、FAT 挂载切换、开机动画、运行资源加载和音频配置读取。
 *
 * 函数表:
 * - wlWriteRmw: 模块内部辅助函数。
 * - ensureDisplayReady: 初始化或确保对应资源可用。
 * - showUsbModeScreen: 绘制界面、输出内容或响应请求。
 * - preloadSettingIniForBootAnimation: 模块内部辅助函数。
 * - ensureMixerInitialized: 初始化或确保对应资源可用。
 * - runBootAnimationTaskStart: FreeRTOS 任务入口或任务控制函数。
 * - runBootAnimationTaskWait: FreeRTOS 任务入口或任务控制函数。
 * - runBootAnimationTaskAndWait: FreeRTOS 任务入口或任务控制函数。
 * - notifyBacklightActivity: 模块内部辅助函数。
 * - setBacklightTimeSeconds: 保存、写入或更新对应数据。
 * - setBacklightLevel: 保存、写入或更新对应数据。
 * - onRead: 事件回调处理函数。
 * - onWrite: 事件回调处理函数。
 * - onStartStop: 事件回调处理函数。
 * - onUsbEvent: 事件回调处理函数。
 * - mountFat: 挂载或卸载对应文件系统。
 * - applyAudioGainsFromSettingIni: 应用配置或切换运行状态。
 * - unmountFat: 挂载或卸载对应文件系统。
 * - openRawBackend: 初始化或确保对应资源可用。
 * - closeRawBackend: 停止、释放或清理对应状态。
 * - enterUsbMode: 模块内部辅助函数。
 * - enterAppMode: 模块内部辅助函数。
 * - appConsumeUpdateRebootRequest: 模块内部辅助函数。
 * - appConsumeForceAppUpdateBoot: 模块内部辅助函数。
 * - applyPendingFatUpdatesFromUpdateDir: 应用配置或切换运行状态。
 * - initProjectResources: 初始化或确保对应资源可用。
 * - playMessageWithGlitch: 播放对应消息或音频流程。
 */
static bool wlWriteRmw(size_t addr, const uint8_t *src, size_t len)
{
  if (wlHandle == WL_INVALID_HANDLE)
    return false;

  const size_t wlSector = wl_sector_size(wlHandle);
  if (wlSector == 0)
    return false;

  std::vector<uint8_t> cache(wlSector);
  if (cache.empty())
    return false;

  while (len > 0)
  {
    const size_t base = (addr / wlSector) * wlSector;
    const size_t inSector = addr - base;
    size_t chunk = wlSector - inSector;
    if (chunk > len)
      chunk = len;

    if (wl_read(wlHandle, base, cache.data(), wlSector) != ESP_OK)
      return false;
    memcpy(cache.data() + inSector, src, chunk);
    if (wl_erase_range(wlHandle, base, wlSector) != ESP_OK)
      return false;
    if (wl_write(wlHandle, base, cache.data(), wlSector) != ESP_OK)
      return false;

    addr += chunk;
    src += chunk;
    len -= chunk;
  }

  return true;
}

void ensureDisplayReady()
{
  if (displayBootstrapped)
    return;
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  displayBootstrapped = true;
}

void showUsbModeScreen()
{
  ensureDisplayReady();

  spriteBoot.createSprite(320, 120);
  tft.fillScreen(TFT_BLACK);
  spriteBoot.setTextWrap(false, false);
  spriteBoot.setTextColor(0xff36, TFT_BLACK);
  spriteBoot.setTextSize(2);
  delay(300);
  ledcWrite(0, backlightDutyBrightFromLevel(gBacklightLevel));

  const char *line1 = "USB MODE";
  const char *line2 = "Mass Storage Connected";
  const char *line3 = "Edit files on your PC...";

  auto typeLine = [&](int x, int y, const char *line, int stepDelayMs)
  {
    String buf;
    for (int i = 0; line[i] != '\0'; ++i)
    {
      buf += line[i];
      spriteBoot.setCursor(x, y);
      spriteBoot.print(buf);
      spriteBoot.pushSprite(0, 100);
      delay(stepDelayMs);
    }
  };

  typeLine(18, 20, line1, 10);
  typeLine(18, 60, line2, 10);
  typeLine(18, 100, line3, 10);
}

static void preloadSettingIniForBootAnimation()
{
  if (gSettingsPreloadedAtBoot)
    return;

  bool mountedTemp = false;
  if (!FFat.begin(false, kFatMountPoint, 10, kFatPartitionLabel))
  {
    Serial.println("[BOOT] skip setting.ini preload (FAT not ready)");
    return;
  }
  mountedTemp = true;

  applyAudioGainsFromSettingIni();
  gSettingsPreloadedAtBoot = true;
  Serial.printf("[BOOT] setting.ini preloaded, backlight=%.3f\n", gBacklightLevel);

  if (mountedTemp)
  {
    FFat.end();
  }
}

static bool ensureMixerInitialized(const char *contextTag)
{
  if (mixer.isRunning())
    return true;

  if (!mountFat())
  {
    Serial.printf("[%s] mount FAT failed for mixer init\n", contextTag);
    showFatFsMountFailedHint("while init mixer");
    return false;
  }

  WavMixerI2S::I2SPinConfig pins = {.bck = 40, .ws = 39, .dout = 41};
  if (!mixer.begin(pins))
  {
    Serial.printf("[%s] Mixer init failed\n", contextTag);
    return false;
  }
  mixer.startOnCore(0);
  Serial.printf("[%s] mixer initialized\n", contextTag);
  return true;
}

void runBootAnimationTaskStart()
{
  if (gBootAnimRunning)
    return;
  gBootAnimErrorHintSuppressed = true;

  ensureDisplayReady();
  tft.fillScreen(TFT_BLACK);

  // Preload config first so boot animation brightness follows setting.ini BackLight.
  preloadSettingIniForBootAnimation();

  bool littleFsReady = false;
  if (LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel))
  {
    littleFsReady = true;
  }
  else
  {
    Serial.println("[BOOT] LittleFS mount failed during boot preload");
  }

  Text.createSprite(320, 120);
  Text.fillSprite(TFT_BLACK);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0xff36, TFT_BLACK);

  bool usedOxta = false;
  if (littleFsReady && LittleFS.exists("/Oxta14.vlw"))
  {
    Text.loadFont("Oxta14", LittleFS);
    usedOxta = true;
    Serial.println("[BOOT] Oxta14 preloaded for boot animation");
  }
  else
  {
    Text.setTextSize(2);
    if (littleFsReady)
    {
      Serial.println("[BOOT] Oxta14.vlw missing, fallback to default font");
    }
  }

  Text.drawString("PROJECT MOON", 180, 60);
  if (usedOxta)
  {
    Text.unloadFont();
  }
  Text.setTextWrap(true, true);

  gBootAnimWaiter = xTaskGetCurrentTaskHandle();
  gBootAnimUsbDetected = usbHostActive;

  if (ensureMixerInitialized("BOOT"))
  {
    mixer.setInsertGain(gInsertGain);
    mixer.setBgGain(gBgGain);
    mixer.playBGnoLoop("/Boot.wav");
  }
  else
  {
    Serial.println("[BOOT] mixer not ready, skip Boot.wav");
  }

  const BaseType_t taskOk = xTaskCreate(task_LogoFadeInAndMove, "LogoFadeMove", 20480, gBootAnimWaiter, 1, nullptr);
  if (taskOk != pdPASS)
  {
    Serial.println("[BOOT] animation task create failed");
    mixer.stopBG();
    gBootAnimWaiter = nullptr;
    gBootAnimRunning = false;
    gBootAnimErrorHintSuppressed = false;
    return;
  }
  if (!usbHostActive && littleFsReady && !gSimheiFontPreloaded)
  {
    if (LittleFS.exists("/simhei15.vlw"))
    {
      Text.loadFont("simhei15", LittleFS);
      gSimheiFontPreloaded = true;
      Serial.println("[BOOT] simhei15 preloaded before animation end");
    }
    else
    {
      Serial.println("[BOOT] simhei15.vlw missing during boot preload");
    }
  }
  gBootAnimRunning = true;
}

void runBootAnimationTaskWait()
{
  if (!gBootAnimRunning)
  {
    gBootAnimErrorHintSuppressed = false;
    return;
  }

  bool animDone = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < kBootAnimMaxWaitMs)
  {
    if (usbHostActive)
    {
      gBootAnimUsbDetected = true;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kBootAnimPollMs)) > 0)
    {
      animDone = true;
      break;
    }
  }

  if (!animDone)
  {
    Serial.println("[BOOT] animation wait timeout");
  }
  if (gBootAnimUsbDetected)
  {
    Serial.println("[BOOT] USB detected during boot animation");
  }

  // Ensure boot BGM does not leak into USB/App mode transition.
  mixer.stopBG();

  gBootAnimRunning = false;
  gBootAnimWaiter = nullptr;
  gBootAnimErrorHintSuppressed = false;
}

void runBootAnimationTaskAndWait()
{
  runBootAnimationTaskStart();
  runBootAnimationTaskWait();
}

void notifyBacklightActivity()
{
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightLastActivityMs = millis();
  const bool wasNotBright = (gBacklightState != kBacklightBright);
  gBacklightState = kBacklightBright;
  portEXIT_CRITICAL(&gBacklightMux);
  if (wasNotBright)
  {
    applyBacklightState(kBacklightBright);
  }
}

void setBacklightTimeSeconds(int seconds)
{
  portENTER_CRITICAL(&gBacklightMux);
  gBacklightTimeSec = seconds;
  portEXIT_CRITICAL(&gBacklightMux);
  notifyBacklightActivity();
}

void setBacklightLevel(float level)
{
  gBacklightLevel = clampUnitFloat(level);

  BacklightState stateNow = kBacklightBright;
  portENTER_CRITICAL(&gBacklightMux);
  stateNow = gBacklightState;
  portEXIT_CRITICAL(&gBacklightMux);

  applyBacklightState(stateNow);
}

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
  if (wlHandle == WL_INVALID_HANDLE)
    return -1;
  const size_t addr = static_cast<size_t>(lba) * mscBlockSize + offset;
  if (wl_read(wlHandle, addr, buffer, bufsize) != ESP_OK)
    return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
  if (wlHandle == WL_INVALID_HANDLE)
    return -1;
  const size_t addr = static_cast<size_t>(lba) * mscBlockSize + offset;
  if (!wlWriteRmw(addr, buffer, bufsize))
    return -1;
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t power_condition, bool start, bool load_eject)
{
  (void)power_condition;
  Serial.printf("[MSC] start=%d eject=%d\n", start, load_eject);
  if (load_eject)
  {
    gForceAppUpdateBootTag = kForceAppUpdateBootMagic;
    gUpdateRebootRequested = true;
    Serial.println("[UPDATE] USB eject detected, schedule reboot for update scan");
  }
  return true;
}

void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  (void)arg;
  (void)event_data;
  if (event_base != ARDUINO_USB_EVENTS)
    return;

  switch (event_id)
  {
  case ARDUINO_USB_RESUME_EVENT:
    usbHostActive = true;
    Serial.println("[USB] host active");
    break;
  case ARDUINO_USB_SUSPEND_EVENT:
    Serial.println("[USB] host suspended");
    break;
  case ARDUINO_USB_STOPPED_EVENT:
    usbHostActive = false;
    if (!usbDisconnectedLogged)
    {
      Serial.println("[USB] host disconnected (cable removed or host detached)");
      usbDisconnectedLogged = true;
    }
    break;
  case ARDUINO_USB_STARTED_EVENT:
    usbHostActive = true;
    usbDisconnectedLogged = false;
    Serial.println("[USB] device started");
    break;
  default:
    break;
  }
}

bool mountFat()
{
  if (fatMounted)
    return true;
  if (!FFat.begin(false, kFatMountPoint, 10, kFatPartitionLabel))
  {
    Serial.println("[APP] FFat.begin failed");
    return false;
  }
  fatMounted = true;
  Serial.println("[APP] FAT mounted");
  return true;
}

void applyAudioGainsFromSettingIni()
{
  static constexpr float kDefaultInsertGain = 0.2f;
  static constexpr float kDefaultBgGain = 0.2f;
  static constexpr int kDefaultWrongProb3 = 25;
  static constexpr int kDefaultWrongProb5 = 12;
  static constexpr int kDefaultInsertSoundBaseProbability = 10;
  static constexpr int kDefaultInsertSoundIncreaseProbability = 5;
  static constexpr bool kDefaultEnableReprint = true;
  static constexpr float kDefaultBacklightLevel = 1.0f;
  static constexpr int kDefaultBacklightTimeSec = -1;
  static constexpr int kDefaultBacklightCloseTime = kDefaultBacklightCloseTimeSec;
  static constexpr int kDefaultSleepTimeMinutes = kDefaultSleepTimeMin;

  gInsertGain = kDefaultInsertGain;
  gBgGain = kDefaultBgGain;
  gBacklightLevel = kDefaultBacklightLevel;
  gWrongProb3 = kDefaultWrongProb3;
  gWrongProb5 = kDefaultWrongProb5;
  gInsertSoundBaseProbability = kDefaultInsertSoundBaseProbability;
  gInsertSoundIncreaseProbability = kDefaultInsertSoundIncreaseProbability;
  gEnableReprint = kDefaultEnableReprint;
  gBacklightTimeSec = kDefaultBacklightTimeSec;
  gBacklightCloseTimeSec = kDefaultBacklightCloseTime;
  gSleepTimeMin = kDefaultSleepTimeMinutes;

  fs::File f = FFat.open("/setting.ini", FILE_READ);
  if (!f)
  {
    Serial.println("[APP] /setting.ini not found, using default gains");
    return;
  }

  bool gotInsert = false;
  bool gotBg = false;
  bool gotWrong3 = false;
  bool gotWrong5 = false;
  bool gotInsertSoundBase = false;
  bool gotInsertSoundIncrease = false;
  bool gotReprint = false;
  bool gotBacklight = false;
  bool gotBacklightTime = false;
  bool gotBacklightCloseTime = false;
  bool gotSleepTime = false;
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
    value.trim();
    value.replace(";", "");
    key.toLowerCase();
    value.toLowerCase();
    value.trim();
    while (value.length() && !isalnum((unsigned char)value[value.length() - 1]))
    {
      value.remove(value.length() - 1);
    }

    const float parsed = value.toFloat();
    if (key == "insertgain")
    {
      gInsertGain = parsed;
      gotInsert = true;
    }
    else if (key == "backgroundgain")
    {
      gBgGain = parsed;
      gotBg = true;
    }
    else if (key == "backlight")
    {
      gBacklightLevel = clampUnitFloat(parsed);
      gotBacklight = true;
    }
    else if (key == "testwrongindexpersent_3area")
    {
      gWrongProb3 = constrain(value.toInt(), 0, 100);
      gotWrong3 = true;
    }
    else if (key == "testwrongindexpersent_5area")
    {
      gWrongProb5 = constrain(value.toInt(), 0, 100);
      gotWrong5 = true;
    }
    else if (key == "insertsoundbaseprobability")
    {
      gInsertSoundBaseProbability = constrain(value.toInt(), 0, 100);
      gotInsertSoundBase = true;
    }
    else if (key == "insertsoundincreaseprobability")
    {
      gInsertSoundIncreaseProbability = constrain(value.toInt(), 0, 100);
      gotInsertSoundIncrease = true;
    }
    else if (key == "enablereprint")
    {
      gEnableReprint = (value == "1" || value == "true" || value == "on" || value == "yes");
      gotReprint = true;
    }
    else if (key == "backlighttime")
    {
      gBacklightTimeSec = value.toInt();
      gotBacklightTime = true;
    }
    else if (key == "backlightclosetime")
    {
      const int parsedCloseSec = static_cast<int>(value.toInt());
      gBacklightCloseTimeSec = (parsedCloseSec < 0) ? 0 : parsedCloseSec;
      gotBacklightCloseTime = true;
    }
    else if (key == "sleeptime" || key == "sleepafteroffmin" || key == "backlightoffsleepmin")
    {
      const int parsedSleepMin = static_cast<int>(value.toInt());
      gSleepTimeMin = (parsedSleepMin < 0) ? 0 : parsedSleepMin;
      gotSleepTime = true;
    }
  }
  f.close();

  if (!gotInsert)
    Serial.printf("[APP] InsertGain missing, default=%.3f\n", gInsertGain);
  if (!gotBg)
    Serial.printf("[APP] BackGroundGain missing, default=%.3f\n", gBgGain);
  if (!gotWrong3)
    Serial.printf("[APP] TestWrongIndexPersent_3Area missing, default=%d\n", gWrongProb3);
  if (!gotWrong5)
    Serial.printf("[APP] TestWrongIndexPersent_5Area missing, default=%d\n", gWrongProb5);
  if (!gotInsertSoundBase)
  {
    Serial.printf("[APP] InsertSoundBaseProbability missing, default=%d\n", gInsertSoundBaseProbability);
  }
  if (!gotInsertSoundIncrease)
  {
    Serial.printf("[APP] InsertSoundIncreaseProbability missing, default=%d\n", gInsertSoundIncreaseProbability);
  }
  if (!gotReprint)
    Serial.printf("[APP] EnableReprint missing, default=%d\n", gEnableReprint ? 1 : 0);
  if (!gotBacklight)
    Serial.printf("[APP] BackLight missing, default=%.3f\n", gBacklightLevel);
  if (!gotBacklightTime)
    Serial.printf("[APP] BacklightTime missing, default=%d\n", gBacklightTimeSec);
  if (!gotBacklightCloseTime)
    Serial.printf("[APP] BacklightCloseTime missing, default=%d\n", gBacklightCloseTimeSec);
  if (!gotSleepTime)
    Serial.printf("[APP] SleepTime missing, default=%d\n", gSleepTimeMin);
  Serial.printf("[APP] gains: insert=%.3f bg=%.3f backlight=%.3f\n", gInsertGain, gBgGain, gBacklightLevel);
  Serial.printf(
      "[APP] glitch: p3=%d p5=%d insertBase=%d insertInc=%d reprint=%d backlightTime=%d closeTime=%d sleepTimeMin=%d\n",
      gWrongProb3, gWrongProb5, gInsertSoundBaseProbability, gInsertSoundIncreaseProbability, gEnableReprint ? 1 : 0,
      gBacklightTimeSec, gBacklightCloseTimeSec, gSleepTimeMin);
}

void unmountFat()
{
  if (!fatMounted)
    return;
  FFat.end();
  fatMounted = false;
  Serial.println("[APP] FAT unmounted");
}

bool openRawBackend()
{
  if (wlHandle != WL_INVALID_HANDLE)
    return true;

  if (!fatPart)
  {
    fatPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, kFatPartitionLabel);
  }
  if (!fatPart)
  {
    Serial.printf("[MSC] FAT partition '%s' not found\n", kFatPartitionLabel);
    return false;
  }

  if (wl_mount(fatPart, &wlHandle) != ESP_OK)
  {
    Serial.println("[MSC] wl_mount failed");
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  flashBytes = wl_size(wlHandle);
  mscBlockSize = static_cast<uint32_t>(wl_sector_size(wlHandle));
  if (mscBlockSize == 0 || mscBlockSize > 65535)
  {
    Serial.printf("[MSC] invalid block size: %lu\n", static_cast<unsigned long>(mscBlockSize));
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  sectorCount = static_cast<uint32_t>(flashBytes / mscBlockSize);
  if (sectorCount == 0)
  {
    Serial.println("[MSC] invalid sector count");
    wl_unmount(wlHandle);
    wlHandle = WL_INVALID_HANDLE;
    return false;
  }

  Serial.printf("[MSC] backend ready: %lu sectors x %lu bytes\n", static_cast<unsigned long>(sectorCount),
                static_cast<unsigned long>(mscBlockSize));
  return true;
}

void closeRawBackend()
{
  if (wlHandle == WL_INVALID_HANDLE)
    return;
  wl_unmount(wlHandle);
  wlHandle = WL_INVALID_HANDLE;
  Serial.println("[MSC] backend closed");
}

bool enterUsbMode()
{
  if (usbModeActive)
    return true;
  showUsbModeScreen();
  unmountFat();
  if (!openRawBackend())
    return false;
  msc.mediaPresent(true);
  usbModeActive = true;
  Serial.println("[MSC] USB mode active");
  return true;
}

bool enterAppMode()
{
  if (!usbModeActive && fatMounted)
    return true;
  if (usbModeActive)
  {
    msc.mediaPresent(false);
    usbModeActive = false;
    delay(200);
  }
  closeRawBackend();
  if (!mountFat())
  {
    showFatFsMountFailedHint("enter app mode");
    return false;
  }
  return true;
}

bool appConsumeUpdateRebootRequest()
{
  const bool requested = gUpdateRebootRequested;
  gUpdateRebootRequested = false;
  return requested;
}

bool appConsumeForceAppUpdateBoot()
{
  if (gForceAppUpdateBootTag != kForceAppUpdateBootMagic)
    return false;
  gForceAppUpdateBootTag = 0;
  return true;
}

void applyPendingFatUpdatesFromUpdateDir()
{
  Serial.println("[UPDATE] scan begin");
  logUpdatePartitionState();
  if (!fatMounted && !mountFat())
  {
    Serial.println("[UPDATE] skip: FAT not mounted");
    showFatFsMountFailedHint("during update scan");
    return;
  }
  logFatRootFiles();

  const String updateDir = chooseUpdateDirPath();
  if (!updateDir.length())
  {
    Serial.println("[UPDATE] skip: update dir unavailable");
    return;
  }
  logUpdateDirFiles(updateDir);

  String firmwarePath;
  String littleFsPath;
  detectUpdateFilesInDir(updateDir, firmwarePath, littleFsPath);

  if (!firmwarePath.length() && !littleFsPath.length())
  {
    Serial.println("[UPDATE] no update files in /Update");
    return;
  }

  Serial.printf("[UPDATE] pending files: firmware=%s littlefs=%s\n",
                firmwarePath.length() ? firmwarePath.c_str() : "<none>",
                littleFsPath.length() ? littleFsPath.c_str() : "<none>");
  ensureDisplayReady();
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 94);
  tft.print("Update package found");
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 124);
  tft.printf("firmware: %s", firmwarePath.length() ? leafNameFromPath(firmwarePath).c_str() : "<none>");
  tft.setCursor(10, 140);
  tft.printf("littlefs: %s", littleFsPath.length() ? leafNameFromPath(littleFsPath).c_str() : "<none>");
  tft.setCursor(10, 156);
  tft.print("Do not power off...");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 178);
  tft.print(u8"警告：更新过程中请勿断电");
  delay(600);

  if (littleFsPath.length())
  {
    if (!applySingleFatBinUpdate(littleFsPath, U_SPIFFS, "littlefs"))
    {
      Serial.println("[UPDATE] littlefs update failed, keep current firmware");
      delay(1800);
      return;
    }
  }

  if (firmwarePath.length())
  {
    if (!applySingleFatBinUpdate(firmwarePath, U_FLASH, "firmware"))
    {
      Serial.println("[UPDATE] firmware update failed");
      delay(1800);
      return;
    }
  }

  if (littleFsPath.length())
  {
    if (!FFat.remove(littleFsPath))
    {
      Serial.printf("[UPDATE] warning: remove failed %s\n", littleFsPath.c_str());
    }
  }
  if (firmwarePath.length())
  {
    if (!FFat.remove(firmwarePath))
    {
      Serial.printf("[UPDATE] warning: remove failed %s\n", firmwarePath.c_str());
    }
  }

  Serial.println("[UPDATE] update done, restarting...");
  drawUpdateResultUi("Update Success", "Rebooting now...", true);
  delay(1200);
  delay(200);
  esp_restart();
}

bool initProjectResources()
{
  if (appInitialized)
    return true;

  ensureDisplayReady();
  // tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  pinMode(42, OUTPUT);
  digitalWrite(42, HIGH);

  Key_init();
  clearScheduleInterruptQueue();
  serviceBatteryMonitor(true);

  if (!LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel))
  {
    Serial.println("[APP] LittleFS mount failed, trying format...");
    if (!LittleFS.format())
    {
      Serial.println("[APP] LittleFS format failed");
      showLittleFsCorruptHint("LittleFS format failed");
      return false;
    }
    if (!LittleFS.begin(false, "/littlefs", 10, kLittleFsPartitionLabel))
    {
      Serial.println("[APP] LittleFS init failed after format");
      showLittleFsCorruptHint("LittleFS mount failed");
      return false;
    }
    Serial.println("[APP] LittleFS formatted and mounted");
    Serial.println("[APP] note: LittleFS was rebuilt, run uploadfs to restore font/audio files");
  }

  if (!LittleFS.exists("/simhei15.vlw"))
  {
    Serial.println("[APP] LittleFS font files missing");
    Serial.println("[APP] run: pio run -t uploadfs -e 4d_systems_esp32s3_gen4_r8n16");
    showLittleFsCorruptHint("simhei15.vlw missing");
    return false;
  }
  if (!LittleFS.exists("/Oxta14.vlw"))
  {
    Serial.println("[APP] Oxta14.vlw missing, boot logo text falls back to default font");
  }

  if (!ensureMixerInitialized("APP"))
  {
    return false;
  }

  if (!mountFat())
  {
    Serial.println("[APP] mount FAT failed");
    showFatFsMountFailedHint("in app init");
    return false;
  }

  if (!gSettingsPreloadedAtBoot)
  {
    applyAudioGainsFromSettingIni();
  }
  else
  {
    Serial.printf("[APP] setting.ini already preloaded, backlight=%.3f\n", gBacklightLevel);
  }

  Text.createSprite(320, 120);
  Text.fillSprite(TFT_BLACK);
  Text.setTextDatum(MC_DATUM);
  Text.setTextColor(0x07ff, TFT_BLACK);
  Text.setTextWrap(true, true);
  if (!gSimheiFontPreloaded)
  {
    Text.loadFont("simhei15", LittleFS);
    gSimheiFontPreloaded = true;
    Serial.println("[APP] simhei15 loaded in app init");
  }
  else
  {
    Serial.println("[APP] simhei15 already preloaded");
  }

  std::vector<size_t> missingTargets;
  collectMissingFatRecoveryTargets(missingTargets);
  if (!missingTargets.empty())
  {
    Serial.printf("[APP] FAT important targets missing: %u\n", static_cast<unsigned int>(missingTargets.size()));
    for (size_t i = 0; i < missingTargets.size(); ++i)
    {
      const FatRecoveryTarget &target = kFatRecoveryTargets[missingTargets[i]];
      Serial.printf("[APP] missing: %s (%s)\n", target.fatPath ? target.fatPath : "<null>",
                    target.name ? target.name : "<unnamed>");
    }
    // Recovery flow runs during init phase before app loop, key actions won't trigger.
    playMessageWithGlitch(kFatRecoveryNoticeLine1);

    size_t recoveredFileCount = 0;
    bool usedFullRestore = false;
    if (restoreFatByMissingTargets(missingTargets, recoveredFileCount, usedFullRestore))
    {
      Serial.printf("[APP] FAT restore finished, copied files=%u\n", static_cast<unsigned int>(recoveredFileCount));
      Serial.printf("[APP] FAT restore mode=%s\n", usedFullRestore ? "full" : "single-target");
      playMessageWithGlitch(kFatRecoveryNoticeLine2);
      setPostRecoveryManualMessage(kFatRecoveryNoticeLine3);
    }
    else
    {
      Serial.println("[APP] FAT restore from LittleFS backup failed");
      playMessageWithGlitch(kFatRecoveryFailedLine);
      setPostRecoveryManualMessage(nullptr);
    }
  }
  else
  {
    setPostRecoveryManualMessage(nullptr);
  }

  if (!csv.load(FFat, "/data.csv"))
  {
    Serial.println("[APP] /data.csv load failed from FAT, fallback message enabled");
  }
  if (csv.size() <= 0)
  {
    Serial.println("[APP] /data.csv is empty, fallback message enabled");
  }
  (void)refreshTodayReminderSlotsFromRtc(true, true);
  message = csv.getTextById(1);
  if (!message || !message[0])
  {
    message = kCsvEmptyFallbackMessage;
  }

  ensureBacklightTaskStarted();
  setBacklightLevel(gBacklightLevel);
  setBacklightTimeSeconds(gBacklightTimeSec);

  mixer.setInsertGain(gInsertGain);
  mixer.setBgGain(gBgGain);
  firstFlag = true;
  appInitialized = true;
  Serial.println("[APP] project initialized");
  return true;
}

static void playMessageWithGlitch(const char *text)
{
  if (!text || !text[0])
    return;
  static int8_t sBbEndExists = -1;

  rememberLastDisplayedText(text);
  notifyBacklightActivity();
  mixer.playBG("/BG.wav");
  mixer.playInsert("/BGstart.wav");
  Text.fillRect(0, 0, tft.width(), 100, TFT_BLACK);
  Text.pushImage(160 - 60, 50 - 60, 120, 120, (uint16_t *)Index_B);
  Text.pushSprite(0, 150 - 50);
  showGlitchEffectUTF8(text);
  mixer.stopBG();
  mixer.playBGnoLoop("/BGend.wav");
  if (sBbEndExists < 0 && fatMounted)
  {
    sBbEndExists = FFat.exists("/sound/BBend.wav") ? 1 : 0;
    if (sBbEndExists == 0)
    {
      Serial.println("[AUDIO] /sound/BBend.wav missing, skip BBend insert");
    }
  }
  if (sBbEndExists != 0)
  {
    mixer.playInsert("/BBend.wav");
  }
}
