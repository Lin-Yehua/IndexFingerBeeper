/*
 * 文件说明: 网页图片中断显示私有实现。
 * 文件功能: 分配图片缓冲并把网页上传的 RGB565 图像绘制到屏幕。
 *
 * 函数表:
 * - ensureWebImageScratch: 初始化或确保对应资源可用。
 * - ensureWebImageLineBuffer: 初始化或确保对应资源可用。
 * - showWebInterruptImage: 绘制界面、输出内容或响应请求。
 */
static constexpr uint16_t kWebImageMaxWidth = 320;
static constexpr uint16_t kWebImageMaxHeight = 240;
static constexpr size_t kWebImageMaxPixels =
    static_cast<size_t>(kWebImageMaxWidth) * static_cast<size_t>(kWebImageMaxHeight);
static uint16_t *gWebImageScratch = nullptr;
static size_t gWebImageScratchPixels = 0;
static uint16_t *gWebImageLineBuffer = nullptr;
static size_t gWebImageLineBufferPixels = 0;

static bool ensureWebImageScratch(size_t pixelCount)
{
  if (pixelCount == 0 || pixelCount > kWebImageMaxPixels)
    return false;
  if (gWebImageScratch && gWebImageScratchPixels >= pixelCount)
    return true;

  const size_t bytes = pixelCount * sizeof(uint16_t);
  uint16_t *next = nullptr;
  if (psramFound())
  {
    next = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!next)
  {
    next = static_cast<uint16_t *>(malloc(bytes));
  }
  if (!next)
    return false;

  if (gWebImageScratch)
  {
    free(gWebImageScratch);
  }
  gWebImageScratch = next;
  gWebImageScratchPixels = pixelCount;
  return true;
}

static bool ensureWebImageLineBuffer(uint16_t width)
{
  if (width == 0 || width > kWebImageMaxWidth)
    return false;
  if (gWebImageLineBuffer && gWebImageLineBufferPixels >= width)
    return true;

  const size_t bytes = static_cast<size_t>(width) * sizeof(uint16_t);
  uint16_t *next = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (!next)
  {
    next = static_cast<uint16_t *>(malloc(bytes));
  }
  if (!next)
    return false;

  if (gWebImageLineBuffer)
  {
    free(gWebImageLineBuffer);
  }
  gWebImageLineBuffer = next;
  gWebImageLineBufferPixels = width;
  return true;
}

static void showWebInterruptImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX,
                                  int16_t centerY)
{
  if (!pixels || width == 0 || height == 0)
    return;
  if (width > tft.width() || height > tft.height())
    return;

  int drawX = static_cast<int>(centerX) - static_cast<int>(width) / 2;
  int drawY = static_cast<int>(centerY) - static_cast<int>(height) / 2;

  if (drawX < 0)
    drawX = 0;
  if (drawY < 0)
    drawY = 0;
  if (drawX + static_cast<int>(width) > tft.width())
  {
    drawX = tft.width() - static_cast<int>(width);
  }
  if (drawY + static_cast<int>(height) > tft.height())
  {
    drawY = tft.height() - static_cast<int>(height);
  }
  if (drawX < 0 || drawY < 0)
    return;
  if (!ensureWebImageLineBuffer(width))
  {
    Serial.println("[WEB] image line buffer alloc failed");
    return;
  }

  tft.fillScreen(TFT_BLACK);
  for (uint16_t row = 0; row < height; ++row)
  {
    memcpy(gWebImageLineBuffer, pixels + static_cast<size_t>(row) * static_cast<size_t>(width),
           static_cast<size_t>(width) * sizeof(uint16_t));
    tft.pushImage(drawX, drawY + row, width, 1, gWebImageLineBuffer);
  }
}
