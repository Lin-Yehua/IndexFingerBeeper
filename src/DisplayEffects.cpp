#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include "AppGlobals.h"
#include "DisplayEffects.h"
#include "Logo_Moon_B.h"
#include "Index_B.h"
#include "Hanchi_Index.h"
#include "Key_Drv.h"

void showGlitchEffectUTF8(const char *text) {
  // 单条文本的最大字符数（按 UTF-8 字符分割后的“逻辑字符”计数）。
  // 这里不是字节数，中文通常会占 3 字节，但仍计为 1 个逻辑字符。
  static constexpr int kMaxChars = 128;
  // 为避免 loopTask 栈溢出，以下大数组全部使用静态工作区。
  // 每次进入函数时会重置内容，不依赖上次状态。
  static String chars[kMaxChars];
  static bool wrongActive[kMaxChars];
  static String wrongChars[kMaxChars];
  static bool engFlickerActive[kMaxChars];
  static uint8_t engFlickerLeft[kMaxChars];
  static char engFlickerChar[kMaxChars];
  static String displayToken[kMaxChars];
  static String shown[kMaxChars];
  static bool incorrectNow[kMaxChars];
  static int prevCurrentLineCount = -1;
  static int prevFrozenVisiblePrefix = -1;
  static int prevBaseY = -1;
  int charCount = 0;
  int keycode = 255;
  bool rollbackEnabled = gEnableReprint;
  bool forceFinishNow = false;
  bool keyLatch = false;
  prevCurrentLineCount = -1;
  prevFrozenVisiblePrefix = -1;
  prevBaseY = -1;

  // UTF-8 按字符切分：把 text 拆成 chars[]（每个元素是一个完整 UTF-8 字符串）
  for (int i = 0; text[i] != '\0' && charCount < kMaxChars;) {
    uint8_t c = (uint8_t)text[i];
    int charLen = 1;

    if ((c & 0x80) == 0x00) charLen = 1;
    else if ((c & 0xE0) == 0xC0) charLen = 2;
    else if ((c & 0xF0) == 0xE0) charLen = 3;
    else if ((c & 0xF8) == 0xF0) charLen = 4;

    chars[charCount] = "";
    for (int j = 0; j < charLen; j++) {
      chars[charCount] += text[i + j];
    }

    i += charLen;
    charCount++;
  }

  // 换行宽度单位：
  // - 中文（非 ASCII）记 1.0 单位
  // - 英文/数字/符号记 0.5 单位
  // 每行超过 16.0 单位就换行（即 16 汉字或约 32 英文）。
  const float kWrapUnits = 16.0f;
  int fullLineStart[8] = {0};
  int fullLineEnd[8] = {0};
  bool lineFrozen[8] = {false};
  String frozenLineText[8];
  // fullLine*: 基于“原文 chars[]”计算出的最终分行（用于冻结判定）
  // currentLine*: 基于“当前显示 token（可能含乱码/已冻结）”计算的动态分行（用于当前帧绘制）
  int fullLineCount = 0;
  int currentLineCount = 1;
  int currentLineStart[8] = {0};
  int currentLineEnd[8] = {0};
  auto charUnit = [&](int idx) -> float {
    return (chars[idx].length() > 1) ? 1.0f : 0.5f;
  };
  {
    // 先预计算“最终分行”，后续冻结逻辑依赖它：
    // 只有某条 final line 完全进入破译区，才允许整行冻结。
    int start = 0;
    float units = 0.0f;
    for (int j = 0; j < charCount && fullLineCount < 8; ++j) {
      float u = charUnit(j);
      if (j > start && units + u > kWrapUnits) {
        fullLineStart[fullLineCount] = start;
        fullLineEnd[fullLineCount] = j;
        fullLineCount++;
        start = j;
        units = 0.0f;
      }
      units += u;
    }
    if (start < charCount && fullLineCount < 8) {
      fullLineStart[fullLineCount] = start;
      fullLineEnd[fullLineCount] = charCount;
      fullLineCount++;
    }
    if (fullLineCount <= 0) {
      fullLineStart[0] = 0;
      fullLineEnd[0] = charCount;
      fullLineCount = 1;
    }
  }

  auto mutateCharNearBoundary = [&](const String &src) -> String {
    (void)src;
    uint32_t cp = 0;
    if (random(100) < 15) {
      cp = 0x1234;
    } else {
      const int idx = random(0, 4001);
      cp = (uint32_t)Index_Han[idx];
    }

    char out[5] = {0};
    if (cp <= 0x7F) {
      out[0] = (char)cp;
    } else if (cp <= 0x7FF) {
      out[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
      out[1] = (char)(0x80 | (cp & 0x3F));
    } else {
      out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
    }
    return String(out);
  };

  for (int j = 0; j < kMaxChars; ++j) {
    wrongActive[j] = false;
    wrongChars[j] = "";
    engFlickerActive[j] = false;
    engFlickerLeft[j] = 0;
    engFlickerChar[j] = 0;
  }
  int rollbackCooldown = 0;
  const char *kEnChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  auto drawWrapped = [&](String shown[], int progressI) {
    // 统一绘制参数：
    // lineH 是文本逻辑行高；areaH 是本次从 Text 精灵中参与推送的区域高度。
    const int lineH = 18;
    const int y0 = 0;
    const int areaH = 100;
    const int areaW = 320;
    // 三段区模型（按 i 的推进方向）：
    // decoded(已破译区) | disturbed(扰动区) | junk(乱码区)
    // kDisturbWidth 决定扰动区宽度（字符数）。
    const int kDisturbWidth = 5;  // zones: decoded | disturbed | junk
    // 行数变化时，精灵整体按半行位移，保持视觉中心相对稳定。
    const int kLayoutShiftPerLine = lineH / 2;

    auto tokenUnit = [&](const String &s) -> float {
      if (!s.length()) return 0.0f;
      bool allAscii = true;
      for (int i = 0; i < s.length(); ++i) {
        if (((uint8_t)s[i]) & 0x80) {
          allAscii = false;
          break;
        }
      }
      if (allAscii) return 0.5f * s.length();
      return 1.0f;
    };

    auto finalLineOf = [&](int idx) -> int {
      for (int ln = 0; ln < fullLineCount; ++ln) {
        if (idx >= fullLineStart[ln] && idx < fullLineEnd[ln]) return ln;
      }
      return fullLineCount - 1;
    };

    // decodeFront 之前（含）可视作“完全进入破译区”的候选边界。
    const int decodeFront = progressI - kDisturbWidth;
    for (int ln = 0; ln < fullLineCount; ++ln) {
      // 冻结条件（核心）：
      // 只有当 final line 的最后一个字符都进入破译区，才整行冻结。
      // 冻结后该行文本固定为原文，不再参与扰动刷新。
      if (!lineFrozen[ln] && (fullLineEnd[ln] - 1) <= decodeFront) {
        String fix = "";
        for (int j = fullLineStart[ln]; j < fullLineEnd[ln]; ++j) fix += chars[j];
        frozenLineText[ln] = fix;
        lineFrozen[ln] = true;
      }
    }

    // displayToken: 本帧最终用于排版/绘制的 token 序列。
    // 如果某字符所在 final line 已冻结，则强制用原文 chars[j]；
    // 否则使用外部传入的 shown[j]（可能是乱码、扰动字或已破译字）。
    for (int j = 0; j < charCount; ++j) {
      int ln = finalLineOf(j);
      displayToken[j] = lineFrozen[ln] ? chars[j] : shown[j];
    }

    currentLineCount = 0;
    if (charCount <= 0) {
      currentLineStart[0] = 0;
      currentLineEnd[0] = 0;
      currentLineCount = 1;
    } else {
      int start = 0;
      float units = 0.0f;
      for (int j = 0; j < charCount && currentLineCount < 8; ++j) {
        float u = tokenUnit(displayToken[j]);
        if (j > start && units + u > kWrapUnits) {
          currentLineStart[currentLineCount] = start;
          currentLineEnd[currentLineCount] = j;
          currentLineCount++;
          start = j;
          units = 0.0f;
        }
        units += u;
      }
      if (start < charCount && currentLineCount < 8) {
        currentLineStart[currentLineCount] = start;
        currentLineEnd[currentLineCount] = charCount;
        currentLineCount++;
      }
      if (currentLineCount <= 0) {
        currentLineStart[0] = 0;
        currentLineEnd[0] = charCount;
        currentLineCount = 1;
      }
    }

    // frozenPrefixFinal：从第 0 行开始连续已冻结的 final line 数量
    int frozenPrefixFinal = 0;
    while (frozenPrefixFinal < fullLineCount && lineFrozen[frozenPrefixFinal]) {
      frozenPrefixFinal++;
    }

    // frozenVisiblePrefix：在“当前动态换行”视图里，前缀连续完全冻结的可见行数量。
    // 用它决定局部刷新起点，冻结前缀行可直接跳过绘制以节省刷新成本。
    int frozenVisiblePrefix = 0;
    for (int ln = 0; ln < currentLineCount; ++ln) {
      bool allFrozen = true;
      for (int j = currentLineStart[ln]; j < currentLineEnd[ln]; ++j) {
        if (finalLineOf(j) >= frozenPrefixFinal) {
          allFrozen = false;
          break;
        }
      }
      if (allFrozen) {
        frozenVisiblePrefix++;
      } else {
        break;
      }
    }

    // layoutChanged：仅在“行数变化”时触发布局级重绘。
    // 冻结前缀变化不再视为布局变化，避免活动区转冻结时整块重刷导致位移观感。
    const bool layoutChanged = (currentLineCount != prevCurrentLineCount);

    const int baseY = 150 - (currentLineCount - 1) * kLayoutShiftPerLine;
    const int imageY = (currentLineCount - 1) * kLayoutShiftPerLine - 60;
    // drawStartLn：本帧重绘起始行。
    // 为了覆盖“活动行 -> 冻结行”的交界过渡，使用上次/本次冻结前缀的较小值作为起点，
    // 确保边界行至少重绘一帧，避免视觉跳变。
    int prevPrefix = prevFrozenVisiblePrefix;
    if (prevPrefix < 0) prevPrefix = frozenVisiblePrefix;
    int drawStartLn = layoutChanged ? 0 : min(prevPrefix, frozenVisiblePrefix);
    if (drawStartLn < 0) drawStartLn = 0;
    if (drawStartLn >= currentLineCount) drawStartLn = currentLineCount - 1;

    // dirtyTop/dirtyBottom：本帧局部脏矩形（精灵内坐标）
    // 给顶部和底部留少量安全边距，避免字体抗锯齿像素被裁掉。
    const int kDirtyPadTop = 3;
    const int kDirtyPadBottom = 3;
    int dirtyTop = y0;
    int dirtyBottom = y0 + areaH - 1;
    if (!layoutChanged && drawStartLn > 0 && drawStartLn < currentLineCount) {
      int firstDynamicTextTop = 12 + drawStartLn * lineH - 8;  // 与 TL 绘制基线一致
      int lastDynamicTextBottom = 12 + (currentLineCount - 1) * lineH - 8 + (lineH - 1);
      dirtyTop = firstDynamicTextTop - kDirtyPadTop;
      dirtyBottom = lastDynamicTextBottom + kDirtyPadBottom;
    }
    const int clippedTop = constrain(dirtyTop, y0, y0 + areaH);
    const int clippedBottom = constrain(dirtyBottom, y0, y0 + areaH - 1);
    const int clippedH = (clippedBottom >= clippedTop) ? (clippedBottom - clippedTop + 1) : 0;

    // baseY 变化（通常是回退导致行数减少）时：
    // 先清理新旧锚点之间“暴露出来的细条区域”，防止旧帧残留。
    // 注意这里只清条带，不做整块清屏，避免黑闪。
    if (prevBaseY >= 0 && baseY != prevBaseY) {
      if (baseY < prevBaseY) {
        const int stripY = baseY + areaH;
        const int stripH = prevBaseY - baseY;
        if (stripH > 0) tft.fillRect(0, stripY, areaW, stripH, TFT_BLACK);
      } else {
        const int stripY = prevBaseY;
        const int stripH = baseY - prevBaseY;
        if (stripH > 0) tft.fillRect(0, stripY, areaW, stripH, TFT_BLACK);
      }
    }

    // 精灵内部清理策略：
    // - 布局变动：清整个参与区域（y0..areaH）
    // - 布局不变：只清活动脏矩形 clippedTop..clippedTop+clippedH
    if (layoutChanged) {
      Text.fillRect(0, y0, areaW, areaH, TFT_BLACK);
      Text.pushImage(160 - 60, imageY, 120, 120, (uint16_t *)Index_B);
    } else {
      if (clippedH > 0) {
        Text.fillRect(0, clippedTop, areaW, clippedH, TFT_BLACK);
      }
      Text.pushImage(160 - 60, imageY, 120, 120, (uint16_t *)Index_B);
    }

    String firstLineText = "";
    for (int j = currentLineStart[0]; j < currentLineEnd[0]; ++j) firstLineText += displayToken[j];
    int firstLineWidth = Text.textWidth(firstLineText);
    int leftAlignedX = 160 - firstLineWidth / 2;
    if (leftAlignedX < 0) leftAlignedX = 0;

    // 绘制起点：
    // - 布局变动：从 0 行重画（保证结构一致）
    // - 布局稳定：从动态首行开始画，冻结前缀行跳过
    for (int ln = drawStartLn; ln < currentLineCount; ++ln) {
      String lineText = "";
      for (int j = currentLineStart[ln]; j < currentLineEnd[ln]; ++j) lineText += displayToken[j];

      int y = 12 + ln * lineH;
      if (ln == 0) {
        Text.setTextDatum(MC_DATUM);
        Text.drawString(lineText, 160, y);
      } else {
        Text.setTextDatum(TL_DATUM);
        Text.drawString(lineText, leftAlignedX, y - 8);
      }
    }

    // 推送策略（脏矩形）：
    // - 布局变动：整块推送（当前精灵可见区域）
    // - 布局稳定：仅推送活动脏矩形
    if (layoutChanged) {
      Text.pushSprite(0, baseY, 0, y0, areaW, areaH);
    } else if (clippedH > 0) {
      Text.pushSprite(0, baseY + (clippedTop - y0), 0, clippedTop-2, areaW, clippedH);
    }

    prevCurrentLineCount = currentLineCount;
    prevFrozenVisiblePrefix = frozenVisiblePrefix;
    prevBaseY = baseY;
  };

  auto tryActivateWrong = [&](int j, int wrongProb) {
    // 在扰动区内，按概率给已显示字符注入“错误字形”扰动。
    if (j < 0 || j >= charCount) return;
    if (wrongActive[j]) return;
    if (random(100) >= wrongProb) return;

    String candidate = chars[j];
    for (int t = 0; t < 6; ++t) {
      candidate = mutateCharNearBoundary(chars[j]);
      if (candidate != chars[j] && candidate != wrongChars[j]) break;
    }
    if (candidate != chars[j]) {
      wrongChars[j] = candidate;
      wrongActive[j] = true;
    }
  };

  int i = 0;
  while (i < charCount) {
    // i 是“破译前沿”索引。每次循环推进一个逻辑字符，
    // 中间穿插若干帧小步动画（steps）来制造故障/抖动感。
    int steps = 2 + random(3);

    for (int s = 0; s < steps; s++) {
      for (int j = 0; j < charCount; j++) {
        if (j < i) {
          int dist = i - j;
          if (dist < 5) {
            int wrongProb = (dist < 3) ? gWrongProb3 : gWrongProb5;
            bool isUtf8 = chars[j].length() > 1;
            if (i < charCount) {
              tryActivateWrong(j, wrongProb);
            }
            if (isUtf8 && !engFlickerActive[j] && random(100) < 15) {
              engFlickerActive[j] = true;
              engFlickerLeft[j] = (uint8_t)random(1, 4);
              engFlickerChar[j] = kEnChars[random((int)strlen(kEnChars))];
            }
            if (engFlickerActive[j]) {
              shown[j] = String(engFlickerChar[j]);
              if (engFlickerLeft[j] > 0) engFlickerLeft[j]--;
              if (engFlickerLeft[j] == 0) engFlickerActive[j] = false;
            } else {
              shown[j] = wrongActive[j] ? wrongChars[j] : chars[j];
            }
          } else {
            wrongActive[j] = false;
            engFlickerActive[j] = false;
            engFlickerLeft[j] = 0;
            shown[j] = chars[j];
          }
        } else if (j == i) {
          // 前沿字符：立即显示原文（增强“正在破译”的感受）
          shown[j] = chars[j];
        } else {
          // 未来区域：乱码区
          char junk = junkChars[random(strlen(junkChars))];
          shown[j] = String(junk);
        }
      }
      drawWrapped(shown, i);
      delay(1);
      Key_loop();
      keycode = get_Keycode();
      if (keycode == 2 && !keyLatch) {
        keyLatch = true;
        if (!gEnableReprint) {
          forceFinishNow = true;
        } else if (rollbackEnabled) {
          rollbackEnabled = false;
        } else {
          forceFinishNow = true;
        }
      }
      if (keycode != 2) {
        keyLatch = false;
      }
      if (forceFinishNow) break;
    }
    if (forceFinishNow) break;

    // incorrectNow 记录本帧各字符是否“错误显示”，用于触发回退逻辑。
    memset(incorrectNow, 0, sizeof(incorrectNow));
    for (int j = 0; j < charCount; j++) {
      if (j <= i) {
        const int dist = i - j;
        if (dist < 5) {
          int wrongProb = (dist < 3) ? gWrongProb3 : gWrongProb5;
          bool isUtf8 = chars[j].length() > 1;
          if (i < charCount) {
            tryActivateWrong(j, wrongProb);
          }
          if (isUtf8 && !engFlickerActive[j] && random(100) < 15) {
            engFlickerActive[j] = true;
            engFlickerLeft[j] = (uint8_t)random(1, 4);
            engFlickerChar[j] = kEnChars[random((int)strlen(kEnChars))];
          }
          if (engFlickerActive[j]) {
            shown[j] = String(engFlickerChar[j]);
            incorrectNow[j] = true;
            if (engFlickerLeft[j] > 0) engFlickerLeft[j]--;
            if (engFlickerLeft[j] == 0) engFlickerActive[j] = false;
          } else if (wrongActive[j]) {
            shown[j] = wrongChars[j];
            incorrectNow[j] = true;
          } else {
            shown[j] = chars[j];
          }
        } else {
          wrongActive[j] = false;
          engFlickerActive[j] = false;
          engFlickerLeft[j] = 0;
          shown[j] = chars[j];
        }
      } else {
        char junk = junkChars[random(strlen(junkChars))];
        shown[j] = String(junk);
      }
    }

    if (random(1, 100) <= 30 + Sound_count) {
      Sound_count = 0;
      mixer.playInsert("/BB2.wav");
    } else {
      Sound_count += 5;
    }

    drawWrapped(shown, i);
    delay(2);
    Key_loop();
    keycode = get_Keycode();
    if (keycode == 2 && !keyLatch) {
      keyLatch = true;
      if (!gEnableReprint) {
        forceFinishNow = true;
      } else if (rollbackEnabled) {
        rollbackEnabled = false;
      } else {
        forceFinishNow = true;
      }
    }
    if (keycode != 2) {
      keyLatch = false;
    }
    if (forceFinishNow) break;

    // 回退冷却：避免连续触发回退造成抖动死循环。
    if (rollbackCooldown > 0) {
      rollbackCooldown--;
      i++;
      continue;
    }

    if (rollbackEnabled && i >= 2) {
      bool allWrong3 = true;
      for (int j = i - 2; j <= i; ++j) {
        if (j < 0 || j >= charCount || !incorrectNow[j]) {
          allWrong3 = false;
          break;
        }
      }
      // 最近 3 个字符都处于错误态 -> 回退 5 个字符，制造“解码失败重试”效果。
      if (allWrong3) {
        i -= 5;
        if (i < 0) i = 0;
        rollbackCooldown = 5;
        int clearL = i - 2;
        if (clearL < 0) clearL = 0;
        int clearR = i + 6;
        if (clearR >= charCount) clearR = charCount - 1;
        for (int j = clearL; j <= clearR; ++j) {
          wrongActive[j] = false;
          wrongChars[j] = "";
          engFlickerActive[j] = false;
          engFlickerLeft[j] = 0;
        }
      } else {
        i++;
      }
    } else {
      i++;
    }
  }

  for (int j = 0; j < charCount; ++j) shown[j] = chars[j];
  drawWrapped(shown, charCount + 8);
}

void task_LogoFadeInAndMove(void *pvParameters) {
  (void)pvParameters;
  tft.pushImage(160 - 45, 150 - 45, 90, 90, (uint16_t *)Logo_Moon_B);
  for (uint8_t N = 0; N < 48; N++) {
    ledcWrite(0, N * 5);
    delay(30);
  }
  delay(500);

  uint8_t steps = 80;
  float progress;
  float eased;
  int dx;
  int d1;
  for (uint8_t i = 0; i <= steps; i++) {
    progress = (float)i / steps;
    eased = 0.5f * (1.0f - cosf(progress * 3.1415926f));
    dx = (int)(eased * 85);
    d1 = (int)(eased * 160);
    Text.pushSprite(160 + 40 - dx, 145, 100, 50, d1, 20);
    tft.pushImage(160 - 45 - dx, 150 - 45, 90, 90, (uint16_t *)Logo_Moon_B);
    delay(30);
  }
  delay(2000);
  for (uint8_t N = 0; N < 48; N++) {
    ledcWrite(0, (48 - (N + 1)) * 5);
    delay(30);
  }
  tft.fillRect(0, 50, 320, 140, 0x0000);
  delay(50);
  ledcWrite(0, 255);
  tft.pushImage(160 - 60, 150 - 60, 120, 120, (uint16_t *)Index_B);
  vTaskDelete(NULL);
}

void generateUniqueRandomNumbers(int low, int high, int count, int *result) {
  if (!result) return;
  if (low > high) return;
  if (count <= 0) return;

  const int range = high - low + 1;
  int need = count;
  if (need > range) need = range;
  if (need <= 0) return;

  // Reservoir sampling:
  // Keep `need` unique ids selected uniformly from [low, high]
  // without allocating O(range) temporary memory.
  for (int i = 0; i < need; ++i) {
    result[i] = low + i;
  }

  for (int seen = need; seen < range; ++seen) {
    int j = random(0, seen + 1);  // [0, seen]
    if (j < need) {
      result[j] = low + seen;
    }
  }

  // Shuffle selected ids to random playback order.
  for (int i = need - 1; i > 0; --i) {
    int j = random(0, i + 1);  // [0, i]
    int tmp = result[i];
    result[i] = result[j];
    result[j] = tmp;
  }
}
