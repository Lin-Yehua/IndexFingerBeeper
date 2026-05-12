/*
 * 文件说明: 网页图片上传实现。
 * 文件功能: 解析图片上传参数，接收上传数据，并转换为待显示图片帧。
 *
 * 函数表:
 * - resetImageUploadStateLocked: 停止、释放或清理对应状态。
 * - setImageUploadErrorLocked: 保存、写入或更新对应数据。
 * - ensureImageUploadCapacityLocked: 初始化或确保对应资源可用。
 * - ensurePendingImageCapacityLocked: 初始化或确保对应资源可用。
 * - parseImageArgs: 解析、规范化或格式化对应内容。
 * - handleImageUpload: 处理对应业务流程或事件分支。
 * - handleImageUploadFinalize: 处理对应业务流程或事件分支。
 */
void resetImageUploadStateLocked()
{
  if (gImageUpload.bytes)
  {
    free(gImageUpload.bytes);
  }
  gImageUpload.bytes = nullptr;
  gImageUpload.size = 0;
  gImageUpload.capacity = 0;
  gImageUpload.failed = false;
  gImageUpload.error[0] = '\0';
}

void setImageUploadErrorLocked(const char *msg)
{
  gImageUpload.failed = true;
  if (!msg)
    msg = "upload failed";
  snprintf(gImageUpload.error, sizeof(gImageUpload.error), "%s", msg);
  gImageUpload.error[sizeof(gImageUpload.error) - 1] = '\0';
}

bool ensureImageUploadCapacityLocked(size_t required)
{
  if (required <= gImageUpload.capacity)
    return true;
  size_t nextCap = gImageUpload.capacity ? gImageUpload.capacity : 1024;
  while (nextCap < required && nextCap < kImageUploadByteLimit)
  {
    const size_t doubled = nextCap * 2;
    if (doubled <= nextCap)
      break;
    nextCap = doubled;
  }
  if (nextCap < required)
    nextCap = required;
  if (nextCap > kImageUploadByteLimit)
    return false;

  uint8_t *next = static_cast<uint8_t *>(realloc(gImageUpload.bytes, nextCap));
  if (!next)
    return false;
  gImageUpload.bytes = next;
  gImageUpload.capacity = nextCap;
  return true;
}

bool ensurePendingImageCapacityLocked(size_t requiredPixels)
{
  if (requiredPixels == 0)
    return false;
  if (requiredPixels <= gPendingImage.pixelCapacity && gPendingImage.pixels)
    return true;

  uint16_t *next = static_cast<uint16_t *>(realloc(gPendingImage.pixels, requiredPixels * sizeof(uint16_t)));
  if (!next)
    return false;
  gPendingImage.pixels = next;
  gPendingImage.pixelCapacity = requiredPixels;
  return true;
}

bool parseImageArgs(uint16_t &outWidth, uint16_t &outHeight, int16_t &outCenterX, int16_t &outCenterY, String &errorOut)
{
  outWidth = kImageDefaultWidth;
  outHeight = kImageDefaultHeight;
  outCenterX = kImageDefaultCenterX;
  outCenterY = kImageDefaultCenterY;
  errorOut = "";

  String widthRaw = gWebServer->arg("width");
  if (!widthRaw.length() && gWebServer->hasArg("w"))
    widthRaw = gWebServer->arg("w");
  widthRaw.trim();
  if (widthRaw.length())
  {
    int parsed = 0;
    if (!parseIntString(widthRaw, parsed) || parsed <= 0 || parsed > static_cast<int>(kImageMaxWidth))
    {
      errorOut = "width must be 1-320";
      return false;
    }
    outWidth = static_cast<uint16_t>(parsed);
  }

  String heightRaw = gWebServer->arg("height");
  if (!heightRaw.length() && gWebServer->hasArg("h"))
    heightRaw = gWebServer->arg("h");
  heightRaw.trim();
  if (heightRaw.length())
  {
    int parsed = 0;
    if (!parseIntString(heightRaw, parsed) || parsed <= 0 || parsed > static_cast<int>(kImageMaxHeight))
    {
      errorOut = "height must be 1-240";
      return false;
    }
    outHeight = static_cast<uint16_t>(parsed);
  }

  String centerXRaw = gWebServer->arg("centerX");
  if (!centerXRaw.length() && gWebServer->hasArg("cx"))
    centerXRaw = gWebServer->arg("cx");
  centerXRaw.trim();
  if (centerXRaw.length())
  {
    int parsed = 0;
    if (!parseIntString(centerXRaw, parsed) || parsed < -1024 || parsed > 1024)
    {
      errorOut = "centerX must be -1024..1024";
      return false;
    }
    outCenterX = static_cast<int16_t>(parsed);
  }

  String centerYRaw = gWebServer->arg("centerY");
  if (!centerYRaw.length() && gWebServer->hasArg("cy"))
    centerYRaw = gWebServer->arg("cy");
  centerYRaw.trim();
  if (centerYRaw.length())
  {
    int parsed = 0;
    if (!parseIntString(centerYRaw, parsed) || parsed < -1024 || parsed > 1024)
    {
      errorOut = "centerY must be -1024..1024";
      return false;
    }
    outCenterY = static_cast<int16_t>(parsed);
  }

  return true;
}

void handleImageUpload()
{
  if (!gImageMutex)
    return;

  HTTPUpload &upload = gWebServer->upload();
  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(250)) != pdTRUE)
  {
    return;
  }

  if (upload.status == UPLOAD_FILE_START)
  {
    resetImageUploadStateLocked();
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (!gImageUpload.failed)
    {
      const size_t nextSize = gImageUpload.size + upload.currentSize;
      if (nextSize > kImageUploadByteLimit)
      {
        setImageUploadErrorLocked("image exceeds 1MB");
      }
      else if (!ensureImageUploadCapacityLocked(nextSize))
      {
        setImageUploadErrorLocked("no memory for image");
      }
      else
      {
        memcpy(gImageUpload.bytes + gImageUpload.size, upload.buf, upload.currentSize);
        gImageUpload.size = nextSize;
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    setImageUploadErrorLocked("upload aborted");
  }

  xSemaphoreGive(gImageMutex);
}

void handleImageUploadFinalize()
{
  if (!gImageMutex)
  {
    gWebServer->send(500, "text/plain", "image mutex not ready");
    return;
  }

  uint16_t width = kImageDefaultWidth;
  uint16_t height = kImageDefaultHeight;
  int16_t centerX = kImageDefaultCenterX;
  int16_t centerY = kImageDefaultCenterY;
  String argError;
  if (!parseImageArgs(width, height, centerX, centerY, argError))
  {
    if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(250)) == pdTRUE)
    {
      resetImageUploadStateLocked();
      xSemaphoreGive(gImageMutex);
    }
    gWebServer->send(400, "text/plain", argError);
    return;
  }

  if (xSemaphoreTake(gImageMutex, pdMS_TO_TICKS(500)) != pdTRUE)
  {
    gWebServer->send(503, "text/plain", "image buffer busy");
    return;
  }

  const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2U;
  if (gImageUpload.failed)
  {
    const String err = gImageUpload.error[0] ? String(gImageUpload.error) : String("upload failed");
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", err);
    return;
  }

  if (!gImageUpload.bytes || gImageUpload.size == 0)
  {
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", "image body is empty");
    return;
  }

  if (gImageUpload.size != expectedBytes)
  {
    String err = "invalid RGB565 payload bytes: got ";
    err += String(static_cast<unsigned int>(gImageUpload.size));
    err += ", expect ";
    err += String(static_cast<unsigned int>(expectedBytes));
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(400, "text/plain", err);
    return;
  }

  const size_t pixelCount = expectedBytes / 2U;
  if (!ensurePendingImageCapacityLocked(pixelCount))
  {
    resetImageUploadStateLocked();
    xSemaphoreGive(gImageMutex);
    gWebServer->send(500, "text/plain", "no memory for pending frame");
    return;
  }

  for (size_t i = 0; i < pixelCount; ++i)
  {
    const size_t b = i * 2U;
    gPendingImage.pixels[i] = static_cast<uint16_t>(gImageUpload.bytes[b]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(gImageUpload.bytes[b + 1]) << 8);
  }
  gPendingImage.pixelCount = pixelCount;
  gPendingImage.width = width;
  gPendingImage.height = height;
  gPendingImage.centerX = centerX;
  gPendingImage.centerY = centerY;
  gPendingImage.ready = true;

  resetImageUploadStateLocked();
  xSemaphoreGive(gImageMutex);

  String out = "{\"ok\":true,\"width\":";
  out += String(static_cast<unsigned int>(width));
  out += ",\"height\":";
  out += String(static_cast<unsigned int>(height));
  out += ",\"centerX\":";
  out += String(centerX);
  out += ",\"centerY\":";
  out += String(centerY);
  out += "}";
  gWebServer->send(200, "application/json", out);
}
