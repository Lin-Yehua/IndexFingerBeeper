/*
 * 文件说明: FAT 存储恢复与更新私有实现。
 * 文件功能: 处理 FAT 路径、备份恢复、Update 目录扫描和固件/LittleFS 镜像更新。
 *
 * 函数表:
 * - pickFirstExistingUpdatePath: 模块内部辅助函数。
 * - leafNameFromPath: 模块内部辅助函数。
 * - probeFatPathKind: 计算、判断或转换对应结果。
 * - writeTextFileAtomicallyToFat: 保存、写入或更新对应数据。
 * - chooseUpdateDirPath: 计算、判断或转换对应结果。
 * - joinFsPath: 模块内部辅助函数。
 * - ensureFatDirectoryRecursive: 初始化或确保对应资源可用。
 * - copyLittleFsFileToFat: 模块内部辅助函数。
 * - restoreFatFromLittleFsBackupTree: 模块内部辅助函数。
 * - restoreFatFromLittleFsBackup: 模块内部辅助函数。
 * - fatPathMatchesExpectedType: 模块内部辅助函数。
 * - collectMissingFatRecoveryTargets: 模块内部辅助函数。
 * - restoreSingleFatRecoveryTarget: 模块内部辅助函数。
 * - restoreFatByMissingTargets: 模块内部辅助函数。
 * - setPostRecoveryManualMessage: 保存、写入或更新对应数据。
 * - hasPostRecoveryManualMessage: 计算、判断或转换对应结果。
 * - playPostRecoveryManualMessageByKey: 播放对应消息或音频流程。
 * - logUpdateDirFiles: 模块内部辅助函数。
 * - logFatRootFiles: 模块内部辅助函数。
 * - detectUpdateFilesInDir: 计算、判断或转换对应结果。
 * - logUpdatePartitionState: 模块内部辅助函数。
 * - drawUpdateStatusText: 绘制界面、输出内容或响应请求。
 * - updatePartTitle: 模块内部辅助函数。
 * - drawUpdateProgressUi: 绘制界面、输出内容或响应请求。
 * - drawUpdateResultUi: 绘制界面、输出内容或响应请求。
 * - showLittleFsCorruptHint: 绘制界面、输出内容或响应请求。
 * - showFatFsMountFailedHint: 绘制界面、输出内容或响应请求。
 * - applySingleFatBinUpdate: 应用配置或切换运行状态。
 */
String pickFirstExistingUpdatePath(const char *const *paths, size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    if (!paths[i])
      continue;
    if (FFat.exists(paths[i]))
    {
      return String(paths[i]);
    }
  }
  return String();
}

String leafNameFromPath(const String &path)
{
  const int slash = path.lastIndexOf('/');
  if (slash < 0)
    return path;
  return path.substring(slash + 1);
}

enum class FatPathKind : uint8_t
{
  Missing = 0,
  File = 1,
  Directory = 2,
};

FatPathKind probeFatPathKind(const char *path)
{
  if (!path || !path[0])
    return FatPathKind::Missing;
  fs::File f = FFat.open(path, FILE_READ);
  if (!f)
    return FatPathKind::Missing;
  const bool isDir = f.isDirectory();
  f.close();
  return isDir ? FatPathKind::Directory : FatPathKind::File;
}

bool writeTextFileAtomicallyToFat(const char *path, const char *tmpPath, const String &content)
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

String chooseUpdateDirPath()
{
  if (probeFatPathKind(kUpdateDirPath) == FatPathKind::Directory)
    return String(kUpdateDirPath);
  if (probeFatPathKind(kUpdateDirPathLower) == FatPathKind::Directory)
    return String(kUpdateDirPathLower);
  if (FFat.mkdir(kUpdateDirPath))
  {
    Serial.printf("[UPDATE] created missing dir: %s\n", kUpdateDirPath);
    return String(kUpdateDirPath);
  }
  Serial.println("[UPDATE] create /Update failed");
  return String();
}

String joinFsPath(const String &base, const String &name)
{
  String out = base.length() ? base : String("/");
  if (!out.endsWith("/"))
    out += "/";
  out += name;
  return out;
}

bool ensureFatDirectoryRecursive(const String &dirPath)
{
  if (!dirPath.length() || dirPath == "/")
    return true;

  String normalized = dirPath;
  if (!normalized.startsWith("/"))
    normalized = "/" + normalized;

  int cursor = 1;
  while (cursor < normalized.length())
  {
    const int slash = normalized.indexOf('/', cursor);
    const String partial = (slash >= 0) ? normalized.substring(0, slash) : normalized;
    if (partial.length())
    {
      const FatPathKind partialKind = probeFatPathKind(partial.c_str());
      if (partialKind == FatPathKind::File)
      {
        Serial.printf("[RECOVERY] path exists as file, not dir: %s\n", partial.c_str());
        return false;
      }
      if (partialKind == FatPathKind::Missing && !FFat.mkdir(partial.c_str()))
      {
        Serial.printf("[RECOVERY] mkdir failed: %s\n", partial.c_str());
        return false;
      }
    }
    if (slash < 0)
      break;
    cursor = slash + 1;
  }
  return true;
}

bool copyLittleFsFileToFat(const String &srcPath, const String &dstPath)
{
  fs::File src = LittleFS.open(srcPath, FILE_READ);
  if (!src || src.isDirectory())
  {
    Serial.printf("[RECOVERY] backup source open failed: %s\n", srcPath.c_str());
    if (src)
      src.close();
    return false;
  }

  const int slash = dstPath.lastIndexOf('/');
  if (slash > 0)
  {
    const String parentDir = dstPath.substring(0, slash);
    if (!ensureFatDirectoryRecursive(parentDir))
    {
      src.close();
      return false;
    }
  }

  fs::File dst = FFat.open(dstPath, "w");
  if (!dst)
  {
    Serial.printf("[RECOVERY] backup target open failed: %s\n", dstPath.c_str());
    src.close();
    return false;
  }

  uint8_t buffer[1024];
  bool ok = true;
  while (true)
  {
    const size_t readBytes = src.read(buffer, sizeof(buffer));
    if (readBytes == 0)
      break;
    if (dst.write(buffer, readBytes) != readBytes)
    {
      ok = false;
      break;
    }
  }

  dst.flush();
  dst.close();
  src.close();

  if (!ok)
  {
    Serial.printf("[RECOVERY] write failed: %s -> %s\n", srcPath.c_str(), dstPath.c_str());
    (void)FFat.remove(dstPath.c_str());
  }
  return ok;
}

bool restoreFatFromLittleFsBackupTree(const String &srcDirPath, const String &dstDirPath, size_t &outCopiedFiles)
{
  fs::File srcDir = LittleFS.open(srcDirPath, FILE_READ);
  if (!srcDir || !srcDir.isDirectory())
  {
    Serial.printf("[RECOVERY] backup dir missing or invalid: %s\n", srcDirPath.c_str());
    if (srcDir)
      srcDir.close();
    return false;
  }

  if (dstDirPath.length() && dstDirPath != "/")
  {
    if (!ensureFatDirectoryRecursive(dstDirPath))
    {
      srcDir.close();
      return false;
    }
  }

  bool ok = true;
  fs::File item = srcDir.openNextFile();
  while (item)
  {
    String leafName = item.name();
    const int slash = leafName.lastIndexOf('/');
    if (slash >= 0)
      leafName = leafName.substring(slash + 1);

    if (leafName.length())
    {
      const bool isDir = item.isDirectory();
      item.close();

      const String srcChild = joinFsPath(srcDirPath, leafName);
      const String dstChild = joinFsPath(dstDirPath, leafName);
      if (isDir)
      {
        if (!restoreFatFromLittleFsBackupTree(srcChild, dstChild, outCopiedFiles))
        {
          ok = false;
          break;
        }
      }
      else
      {
        if (!copyLittleFsFileToFat(srcChild, dstChild))
        {
          ok = false;
          break;
        }
        ++outCopiedFiles;
      }
    }
    else
    {
      item.close();
    }
    item = srcDir.openNextFile();
  }

  srcDir.close();
  return ok;
}

bool restoreFatFromLittleFsBackup(size_t &outCopiedFiles)
{
  outCopiedFiles = 0;
  if (!LittleFS.exists(kLittleFsBackupDirPath))
  {
    Serial.printf("[RECOVERY] LittleFS backup dir not found: %s\n", kLittleFsBackupDirPath);
    return false;
  }
  fs::File backupRoot = LittleFS.open(kLittleFsBackupDirPath, FILE_READ);
  if (!backupRoot || !backupRoot.isDirectory())
  {
    Serial.printf("[RECOVERY] LittleFS backup dir invalid: %s\n", kLittleFsBackupDirPath);
    if (backupRoot)
      backupRoot.close();
    return false;
  }
  backupRoot.close();
  return restoreFatFromLittleFsBackupTree(kLittleFsBackupDirPath, "/", outCopiedFiles);
}

bool fatPathMatchesExpectedType(const FatRecoveryTarget &target)
{
  const FatPathKind kind = probeFatPathKind(target.fatPath);
  return target.isDirectory ? (kind == FatPathKind::Directory) : (kind == FatPathKind::File);
}

void collectMissingFatRecoveryTargets(std::vector<size_t> &outMissing)
{
  outMissing.clear();
  for (size_t i = 0; i < (sizeof(kFatRecoveryTargets) / sizeof(kFatRecoveryTargets[0])); ++i)
  {
    if (!fatPathMatchesExpectedType(kFatRecoveryTargets[i]))
    {
      outMissing.push_back(i);
    }
  }
}

bool restoreSingleFatRecoveryTarget(const FatRecoveryTarget &target, size_t &outCopiedFiles)
{
  outCopiedFiles = 0;
  if (!target.backupPath || !target.backupPath[0])
    return false;
  if (target.isDirectory)
  {
    return restoreFatFromLittleFsBackupTree(String(target.backupPath), String(target.fatPath), outCopiedFiles);
  }
  if (!copyLittleFsFileToFat(String(target.backupPath), String(target.fatPath)))
  {
    return false;
  }
  outCopiedFiles = 1;
  return true;
}

bool restoreFatByMissingTargets(const std::vector<size_t> &missingTargets, size_t &outCopiedFiles,
                                bool &outUsedFullRestore)
{
  outCopiedFiles = 0;
  outUsedFullRestore = false;
  if (missingTargets.empty())
    return true;

  if (missingTargets.size() == 1)
  {
    const FatRecoveryTarget &target = kFatRecoveryTargets[missingTargets[0]];
    Serial.printf("[RECOVERY] single target missing: %s\n", target.name ? target.name : "<unknown>");
    return restoreSingleFatRecoveryTarget(target, outCopiedFiles);
  }

  outUsedFullRestore = true;
  Serial.printf("[RECOVERY] multiple targets missing (%u), run full restore\n",
                static_cast<unsigned int>(missingTargets.size()));
  return restoreFatFromLittleFsBackup(outCopiedFiles);
}

void setPostRecoveryManualMessage(const char *text)
{
  gPostRecoveryManualMessage[0] = '\0';

  if (!text || !text[0])
    return;

  String normalized;
  if (!normalizeScheduleQueueMessage(String(text), normalized))
    return;
  normalized.toCharArray(gPostRecoveryManualMessage, sizeof(gPostRecoveryManualMessage));
  gPostRecoveryManualMessage[kSleepTextMaxLen] = '\0';
}

bool hasPostRecoveryManualMessage()
{
  return gPostRecoveryManualMessage[0] != '\0';
}

bool playPostRecoveryManualMessageByKey(uint8_t key)
{
  if (key != 2 || !hasPostRecoveryManualMessage())
    return false;
  playMessageWithGlitch(gPostRecoveryManualMessage);
  gPostRecoveryManualMessage[0] = '\0';
  // Manual recovery hint must exit to idle and wait for the next explicit key trigger.
  firstFlag = false;
  return true;
}

void logUpdateDirFiles(const String &dirPath)
{
  fs::File dir = FFat.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    Serial.printf("[UPDATE] open dir failed: %s\n", dirPath.c_str());
    if (dir)
      dir.close();
    return;
  }
  Serial.printf("[UPDATE] listing %s\n", dirPath.c_str());
  fs::File item = dir.openNextFile();
  if (!item)
  {
    Serial.println("[UPDATE] dir is empty");
  }
  while (item)
  {
    String name = item.name();
    if (name.length())
    {
      Serial.printf("[UPDATE]   %s (%u bytes)%s\n", name.c_str(), static_cast<unsigned int>(item.size()),
                    item.isDirectory() ? " [DIR]" : "");
    }
    item.close();
    item = dir.openNextFile();
  }
  dir.close();
}

void logFatRootFiles()
{
  fs::File root = FFat.open("/", FILE_READ);
  if (!root || !root.isDirectory())
  {
    Serial.println("[FAT] root open failed");
    if (root)
      root.close();
    return;
  }

  Serial.printf("[FAT] total=%u used=%u\n", static_cast<unsigned int>(FFat.totalBytes()),
                static_cast<unsigned int>(FFat.usedBytes()));
  fs::File item = root.openNextFile();
  if (!item)
  {
    Serial.println("[FAT] root is empty");
  }
  int count = 0;
  while (item && count < 64)
  {
    String name = item.name();
    Serial.printf("[FAT]   %s (%u bytes)%s\n", name.c_str(), static_cast<unsigned int>(item.size()),
                  item.isDirectory() ? " [DIR]" : "");
    item.close();
    ++count;
    item = root.openNextFile();
  }
  if (item)
  {
    Serial.println("[FAT]   ...");
    item.close();
  }
  root.close();
}

void detectUpdateFilesInDir(const String &dirPath, String &outFirmwarePath, String &outLittleFsPath)
{
  outFirmwarePath = "";
  outLittleFsPath = "";
  fs::File dir = FFat.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir)
      dir.close();
    return;
  }

  auto maybePick = [&](const String &fullPath, const String &nameLower)
  {
    if (!outFirmwarePath.length() &&
        (nameLower == "firmware.bin" || nameLower == "app.bin" || nameLower.indexOf("firmware") >= 0))
    {
      outFirmwarePath = fullPath;
    }
    if (!outLittleFsPath.length() &&
        (nameLower == "littlefs.bin" || nameLower == "littelfs.bin" || nameLower == "fs.bin" ||
         nameLower == "spiffs.bin" || nameLower.indexOf("littlefs") >= 0 || nameLower.indexOf("spiffs") >= 0))
    {
      outLittleFsPath = fullPath;
    }
  };

  fs::File item = dir.openNextFile();
  while (item)
  {
    if (!item.isDirectory())
    {
      String name = item.name();
      const int slash = name.lastIndexOf('/');
      if (slash >= 0)
        name = name.substring(slash + 1);
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".bin"))
      {
        String fullPath = dirPath;
        if (!fullPath.endsWith("/"))
          fullPath += "/";
        fullPath += name;
        maybePick(fullPath, lower);
      }
    }
    item.close();
    item = dir.openNextFile();
  }
  dir.close();
}

void logUpdatePartitionState()
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();

  auto partTypeText = [](esp_partition_subtype_t subtype, int &otaSlot) -> const char *
  {
    otaSlot = -1;
    if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
    {
      otaSlot = static_cast<int>(subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
      return "ota";
    }
    return "other";
  };

  auto logOne = [&](const char *tag, const esp_partition_t *part)
  {
    if (!part)
    {
      Serial.printf("[UPDATE] %s partition: <null>\n", tag);
      return;
    }
    int otaSlot = -1;
    const char *type = partTypeText(part->subtype, otaSlot);
    if (otaSlot >= 0)
    {
      Serial.printf("[UPDATE] %s partition: label=%s type=ota_%d addr=0x%06X size=0x%06X\n", tag, part->label, otaSlot,
                    static_cast<unsigned int>(part->address), static_cast<unsigned int>(part->size));
      return;
    }
    Serial.printf("[UPDATE] %s partition: label=%s type=%s subtype=0x%02X addr=0x%06X size=0x%06X\n", tag, part->label,
                  type, static_cast<unsigned int>(part->subtype), static_cast<unsigned int>(part->address),
                  static_cast<unsigned int>(part->size));
    Serial.printf("[UPDATE] warning: %s is not an OTA slot under dual-OTA layout\n", tag);
  };

  logOne("running", running);
  logOne("configured boot", boot);
}

void drawUpdateStatusText(const String &text, uint16_t color)
{
  tft.fillRect(0, 170, 320, 20, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(10, 176);
  tft.print(text);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

String updatePartTitle(const char *label)
{
  String lower = label ? String(label) : String("update");
  lower.toLowerCase();
  if (lower.indexOf("firmware") >= 0 || lower.indexOf("app") >= 0)
  {
    return String(u8"正在更新：固件");
  }
  if (lower.indexOf("littlefs") >= 0 || lower.indexOf("spiffs") >= 0 || lower.indexOf("fs") >= 0)
  {
    return String(u8"正在更新：文件系统");
  }
  return String(u8"正在更新：数据");
}

void drawUpdateProgressUi(const char *label, const String &path, size_t written, size_t total, bool forceRedraw)
{
  static int sLastPercent = -1;
  static int sLastFillW = 0;
  if (forceRedraw)
  {
    sLastPercent = -1;
    sLastFillW = 0;
  }

  int percent = 0;
  if (total > 0)
  {
    const uint64_t scaled = static_cast<uint64_t>(written) * 100ULL;
    percent = static_cast<int>(scaled / static_cast<uint64_t>(total));
  }
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  if (!forceRedraw && percent == sLastPercent)
    return;
  sLastPercent = percent;

  if (forceRedraw)
  {
    ensureDisplayReady();
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 92);
    tft.print(u8"更新中...");
    tft.setTextSize(1);
    tft.setCursor(10, 112);
    tft.print(leafNameFromPath(path));
    tft.drawRect(20, 126, 280, 20, TFT_WHITE);
    tft.fillRect(22, 128, 276, 16, TFT_DARKGREY);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 152);
    tft.print(updatePartTitle(label));

    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 178);
    tft.print(u8"警告：更新过程中请勿断电");
  }

  const int fillW = (276 * percent) / 100;
  if (fillW > sLastFillW)
  {
    tft.fillRect(22 + sLastFillW, 128, fillW - sLastFillW, 16, TFT_GREEN);
  }
  else if (fillW < sLastFillW)
  {
    // Fallback for unexpected backward progress.
    tft.fillRect(22 + fillW, 128, sLastFillW - fillW, 16, TFT_DARKGREY);
  }
  sLastFillW = fillW;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(230, 92);
  tft.printf("%3d%%", percent);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawUpdateResultUi(const String &title, const String &detail, bool success)
{
  ensureDisplayReady();
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(success ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(10, 98);
  tft.print(title);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 134);
  tft.print(detail);
}

void showLittleFsCorruptHint(const char *detail)
{
  if (gBootAnimErrorHintSuppressed)
  {
    Serial.println("[BOOT] suppress LittleFS hint before boot animation completes");
    return;
  }
  ensureDisplayReady();
  // Keep INDEX_B background visible; do not clear the screen.
  tft.pushImage(160 - 60, 150 - 60, 120, 120, (uint16_t *)Index_B);
  tft.setTextWrap(false, false);
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED);
  tft.setCursor(10, 96);
  tft.print("LittleFS CORRUPTED");
  tft.setCursor(10, 124);
  tft.print("Please reflash");
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW);
  tft.setCursor(10, 152);
  tft.print("Upload littlefs image");
  if (detail && detail[0])
  {
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 170);
    tft.print(detail);
  }
  tft.setTextColor(TFT_WHITE);
}

void showFatFsMountFailedHint(const char *detail)
{
  if (gBootAnimErrorHintSuppressed)
  {
    Serial.println("[BOOT] suppress FATFS hint before boot animation completes");
    return;
  }
  ensureDisplayReady();
  // Keep INDEX_B background visible; do not clear the screen.
  tft.pushImage(160 - 60, 150 - 60, 120, 120, (uint16_t *)Index_B);
  tft.setTextWrap(false, false);
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED);
  tft.setCursor(10, 98);
  tft.print("FATFS mount failed");
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW);
  tft.setCursor(10, 132);
  tft.print(u8"FATFS挂载失败");
  tft.setCursor(10, 152);
  tft.print("Please check storage");
  if (detail && detail[0])
  {
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 170);
    tft.print(detail);
  }
  tft.setTextColor(TFT_WHITE);
}

bool applySingleFatBinUpdate(const String &path, int command, const char *label)
{
  fs::File updateFile = FFat.open(path, FILE_READ);
  if (!updateFile || updateFile.isDirectory())
  {
    Serial.printf("[UPDATE] %s open failed: %s\n", label, path.c_str());
    drawUpdateResultUi("Update Failed", String(label ? label : "update") + " open failed", false);
    if (updateFile)
      updateFile.close();
    return false;
  }

  const size_t imageSize = static_cast<size_t>(updateFile.size());
  if (imageSize == 0)
  {
    Serial.printf("[UPDATE] %s file is empty: %s\n", label, path.c_str());
    drawUpdateResultUi("Update Failed", String(label ? label : "update") + " file empty", false);
    updateFile.close();
    return false;
  }

  if (command == U_SPIFFS)
  {
    LittleFS.end();
    const esp_partition_t *littleFsPart =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kLittleFsPartitionLabel);
    if (!littleFsPart)
    {
      littleFsPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    }
    if (littleFsPart && imageSize > littleFsPart->size)
    {
      Serial.printf("[UPDATE] %s image too large (%u > %u)\n", label, static_cast<unsigned int>(imageSize),
                    static_cast<unsigned int>(littleFsPart->size));
      drawUpdateResultUi("Update Failed", String(label ? label : "update") + " image too large", false);
      updateFile.close();
      return false;
    }
  }
  if (command == U_FLASH)
  {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    auto otaSlotOf = [](const esp_partition_t *p) -> int
    {
      if (!p)
        return -1;
      if (p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || p->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
      {
        return -1;
      }
      return static_cast<int>(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    };
    const int runningSlot = otaSlotOf(running);
    const int targetSlot = otaSlotOf(target);
    if (runningSlot >= 0)
    {
      Serial.printf("[UPDATE] running slot: ota_%d\n", runningSlot);
    }
    else
    {
      Serial.println("[UPDATE] warning: running slot is not OTA");
    }
    if (targetSlot >= 0)
    {
      Serial.printf("[UPDATE] target slot: ota_%d\n", targetSlot);
    }
    else
    {
      Serial.println("[UPDATE] warning: target slot is not OTA");
    }
  }

  Serial.printf("[UPDATE] start %s update: %s (%u bytes)\n", label, path.c_str(), static_cast<unsigned int>(imageSize));
  if (!Update.begin(imageSize, command))
  {
    Serial.printf("[UPDATE] %s begin failed\n", label);
    Update.printError(Serial);
    drawUpdateResultUi("Update Failed", String(label ? label : "update") + " begin failed", false);
    updateFile.close();
    return false;
  }

  drawUpdateProgressUi(label, path, 0, imageSize, true);
  static uint8_t writeBuf[4096];
  size_t written = 0;
  while (written < imageSize)
  {
    const size_t remaining = imageSize - written;
    const size_t want = (remaining > sizeof(writeBuf)) ? sizeof(writeBuf) : remaining;
    const size_t readBytes = updateFile.read(writeBuf, want);
    if (readBytes == 0)
    {
      break;
    }
    const size_t wrote = Update.write(writeBuf, readBytes);
    if (wrote != readBytes)
    {
      break;
    }
    written += wrote;
    drawUpdateProgressUi(label, path, written, imageSize, false);
    delay(1);
  }
  updateFile.close();
  if (written != imageSize)
  {
    Serial.printf("[UPDATE] %s write failed (%u/%u)\n", label, static_cast<unsigned int>(written),
                  static_cast<unsigned int>(imageSize));
    Update.printError(Serial);
    Update.abort();
    drawUpdateResultUi("Update Failed", String(label ? label : "update") + " write failed", false);
    return false;
  }

  if (!Update.end(true) || !Update.isFinished())
  {
    Serial.printf("[UPDATE] %s finalize failed\n", label);
    Update.printError(Serial);
    Update.abort();
    drawUpdateResultUi("Update Failed", String(label ? label : "update") + " finalize failed", false);
    return false;
  }

  Serial.printf("[UPDATE] %s update success\n", label);
  drawUpdateStatusText(String(label ? label : "update") + " OK", TFT_GREEN);
  return true;
}
