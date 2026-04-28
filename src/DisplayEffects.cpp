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

namespace {
uint8_t scaledBacklightDuty(uint8_t rawDuty) {
  float level = gBacklightLevel;
  if (level != level) level = 1.0f;  // NaN fallback
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  const int duty = static_cast<int>(static_cast<float>(rawDuty) * level + 0.5f);
  if (duty < 0) return 0;
  if (duty > 255) return 255;
  return static_cast<uint8_t>(duty);
}
}

void showGlitchEffectUTF8(const char *text) {
  if (!text) return;

  // Normalize incoming text so display pipeline ignores line breaks.
  // This keeps wrapped rendering fully controlled by our own layout logic.
  String normalized;
  normalized.reserve(strlen(text));
  for (size_t i = 0; text[i] != '\0'; ++i) {
    const char ch = text[i];
    if (ch == '\r' || ch == '\n') continue;
    if (ch == '\\' && text[i + 1] != '\0') {
      const char next = text[i + 1];
      if (next == 'r' || next == 'n') {
        ++i;
        continue;
      }
    }
    normalized += ch;
  }
  text = normalized.c_str();

  static constexpr int kMaxChars = 128;
  static constexpr int kMaxLines = 8;
  static constexpr int kDisturbWidth = 5;
  static constexpr int kJunkWidth = 20;
  static constexpr int kLineUnitCap = 32;  // 中文=2, ASCII=1
  static constexpr int kLineH = 18;
  static constexpr int kSpriteW = 320;
  static constexpr int kSpriteH = 120;
  static constexpr int kSpriteScreenY = 100;
  static constexpr int kGobalYmiddle = 160;  // 显示区域中心（屏幕绝对坐标）
  static constexpr int kCenterX = 160;
  static constexpr int kLogoX = 160 - 60;
  static constexpr int kLogoY = 50 - 60;
  static constexpr int kLogoW = 120;
  static constexpr int kLogoH = 120;

  static String chars[kMaxChars];
  static uint8_t srcUnits[kMaxChars];
  static String shown[kMaxChars];
  static bool wrongActive[kMaxChars];
  static String wrongChars[kMaxChars];
  static bool engFlickerActive[kMaxChars];
  static uint8_t engFlickerLeft[kMaxChars];
  static char engFlickerChar[kMaxChars];
  static bool incorrectNow[kMaxChars];

  static int fullLineStart[kMaxLines];
  static int fullLineEnd[kMaxLines];
  static bool lineFrozen[kMaxLines];
  static String frozenLineText[kMaxLines];
  static String frameLineText[kMaxLines];
  static String lastFrameLineText[kMaxLines];

  int charCount = 0;
  int fullLineCount = 0;
  int prevCurrentLineCount = -1;
  int keycode = 255;
  bool rollbackEnabled = gEnableReprint;
  bool forceFinishNow = false;
  bool keyLatch = false;

  const int junkLen = (int)strlen(junkChars);
  const char *kEnChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  const int kEnCharsLen = (int)strlen(kEnChars);

  for (int i = 0; i < kMaxChars; ++i) {
    chars[i] = "";
    srcUnits[i] = 0;
    shown[i] = "";
    wrongActive[i] = false;
    wrongChars[i] = "";
    engFlickerActive[i] = false;
    engFlickerLeft[i] = 0;
    engFlickerChar[i] = 0;
    incorrectNow[i] = false;
  }
  for (int i = 0; i < kMaxLines; ++i) {
    fullLineStart[i] = 0;
    fullLineEnd[i] = 0;
    lineFrozen[i] = false;
    frozenLineText[i] = "";
    frameLineText[i] = "";
    lastFrameLineText[i] = "";
  }

  // UTF-8 按“逻辑字符”切分。
  for (int i = 0; text[i] != '\0' && charCount < kMaxChars;) {
    uint8_t c = (uint8_t)text[i];
    int charLen = 1;
    if ((c & 0x80) == 0x00) charLen = 1;
    else if ((c & 0xE0) == 0xC0) charLen = 2;
    else if ((c & 0xF0) == 0xE0) charLen = 3;
    else if ((c & 0xF8) == 0xF0) charLen = 4;

    int validLen = 0;
    while (validLen < charLen && text[i + validLen] != '\0') validLen++;
    if (validLen <= 0) break;

    chars[charCount] = "";
    for (int j = 0; j < validLen; ++j) chars[charCount] += text[i + j];
    srcUnits[charCount] = (chars[charCount].length() > 1) ? 2 : 1;

    i += validLen;
    charCount++;
  }

  if (charCount <= 0) {
    Text.fillRect(0, 0, kSpriteW, kSpriteH, TFT_BLACK);
    Text.pushImage(kLogoX, kLogoY, kLogoW, kLogoH, (uint16_t *)Index_B);
    Text.pushSprite(0, kSpriteScreenY);
    return;
  }

  // 预计算最终分行：冻结判定严格按这组边界。
  {
    int start = 0;
    int units = 0;
    for (int j = 0; j < charCount && fullLineCount < kMaxLines; ++j) {
      const int u = srcUnits[j];
      if (j > start && units + u > kLineUnitCap) {
        fullLineStart[fullLineCount] = start;
        fullLineEnd[fullLineCount] = j;
        ++fullLineCount;
        start = j;
        units = 0;
      }
      units += u;
    }
    if (start < charCount && fullLineCount < kMaxLines) {
      fullLineStart[fullLineCount] = start;
      fullLineEnd[fullLineCount] = charCount;
      ++fullLineCount;
    }
    if (fullLineCount <= 0) {
      fullLineStart[0] = 0;
      fullLineEnd[0] = charCount;
      fullLineCount = 1;
    }
  }

  auto mutateCharNearBoundary = [&]() -> String {
    uint32_t cp = 0;
    if (random(100) < 15) {
      cp = 0x1234;
    } else {
      cp = (uint32_t)Index_Han[random(0, 4001)];
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

  auto tryActivateWrong = [&](int j, int wrongProb) {
    if (j < 0 || j >= charCount) return;
    if (wrongActive[j]) return;
    if (random(100) >= wrongProb) return;

    String candidate = chars[j];
    for (int t = 0; t < 6; ++t) {
      candidate = mutateCharNearBoundary();
      if (candidate != chars[j] && candidate != wrongChars[j]) break;
    }
    if (candidate != chars[j]) {
      wrongChars[j] = candidate;
      wrongActive[j] = true;
    }
  };

  auto clearTransientState = [&](int l, int r) {
    if (l < 0) l = 0;
    if (r >= charCount) r = charCount - 1;
    for (int j = l; j <= r; ++j) {
      wrongActive[j] = false;
      wrongChars[j] = "";
      engFlickerActive[j] = false;
      engFlickerLeft[j] = 0;
      engFlickerChar[j] = 0;
      incorrectNow[j] = false;
    }
  };

  auto tokenUnits = [&](const String &token) -> int {
    if (!token.length()) return 0;
    return (token.length() > 1) ? 2 : 1;
  };

  // 构建当前帧 token：
  // decoded | disturbed(5) | junk(20) | hidden
  auto buildFrame = [&](int cursorI, bool collectIncorrect) {
    if (collectIncorrect) memset(incorrectNow, 0, sizeof(incorrectNow));

    const int disturbStart = (cursorI > kDisturbWidth) ? (cursorI - kDisturbWidth) : 0;
    const int junkEnd = cursorI + kJunkWidth;

    for (int j = 0; j < charCount; ++j) {
      if (j < disturbStart) {
        // 已破译区：固定正确，不抖动。
        shown[j] = chars[j];
        wrongActive[j] = false;
        wrongChars[j] = "";
        engFlickerActive[j] = false;
        engFlickerLeft[j] = 0;
        engFlickerChar[j] = 0;
        continue;
      }

      if (j < cursorI) {
        // 扰动区：错误字形 + 英文闪烁。
        const int dist = cursorI - j;
        const int wrongProb = (dist <= 3) ? gWrongProb3 : gWrongProb5;
        bool isIncorrect = false;

        if (cursorI < charCount) tryActivateWrong(j, wrongProb);

        const bool isUtf8 = chars[j].length() > 1;
        if (isUtf8 && !engFlickerActive[j] && random(100) < 15) {
          engFlickerActive[j] = true;
          engFlickerLeft[j] = (uint8_t)random(1, 4);  // 1~3 帧
          engFlickerChar[j] = kEnChars[random(0, kEnCharsLen)];
        }

        if (engFlickerActive[j]) {
          shown[j] = String(engFlickerChar[j]);
          isIncorrect = true;
          if (engFlickerLeft[j] > 0) engFlickerLeft[j]--;
          if (engFlickerLeft[j] == 0) engFlickerActive[j] = false;
        } else if (wrongActive[j]) {
          shown[j] = wrongChars[j];
          isIncorrect = true;
        } else {
          shown[j] = chars[j];
        }

        if (collectIncorrect && dist <= 3) incorrectNow[j] = isIncorrect;
        continue;
      }

      if (j == cursorI) {
        // 光标位始终正确。
        shown[j] = chars[j];
        wrongActive[j] = false;
        wrongChars[j] = "";
        engFlickerActive[j] = false;
        engFlickerLeft[j] = 0;
        engFlickerChar[j] = 0;
        continue;
      }

      if (j <= junkEnd) {
        // 乱码区长度固定为 20。
        char junk = junkChars[random(0, junkLen)];
        char s[2] = {junk, '\0'};
        shown[j] = String(s);
      } else {
        // 右侧隐藏：减少每帧处理/绘制负担。
        shown[j] = "";
      }

      wrongActive[j] = false;
      wrongChars[j] = "";
      engFlickerActive[j] = false;
      engFlickerLeft[j] = 0;
      engFlickerChar[j] = 0;
    }
  };

  auto drawWrapped = [&](int progressI, bool forceGlobalRefresh) {
    for (int i = 0; i < kMaxLines; ++i) frameLineText[i] = "";

    // 冻结线：只有最终分行完整进入 decoded 区才冻结。
    int decodedEnd = progressI - kDisturbWidth;
    if (decodedEnd < 0) decodedEnd = 0;
    if (decodedEnd > charCount) decodedEnd = charCount;

    int frozenPrefix = 0;
    while (frozenPrefix < fullLineCount && fullLineEnd[frozenPrefix] <= decodedEnd) {
      ++frozenPrefix;
    }

    for (int li = 0; li < fullLineCount; ++li) {
      const bool shouldFreeze = (li < frozenPrefix);
      if (shouldFreeze && !lineFrozen[li]) {
        frozenLineText[li] = "";
        for (int j = fullLineStart[li]; j < fullLineEnd[li]; ++j) {
          frozenLineText[li] += chars[j];
        }
      } else if (!shouldFreeze && lineFrozen[li]) {
        frozenLineText[li] = "";
      }
      lineFrozen[li] = shouldFreeze;
    }

    int lineCount = 0;
    for (int li = 0; li < frozenPrefix && lineCount < kMaxLines; ++li) {
      frameLineText[lineCount++] = frozenLineText[li];
    }

    // 活动区按“单位宽度”动态换行：
    // 中文单位=2，ASCII单位=1。随破译抖动实时重排，保证可见行宽稳定。
    const int activeStart = (frozenPrefix < fullLineCount) ? fullLineStart[frozenPrefix] : charCount;
    if (lineCount < kMaxLines) {
      int li = lineCount;
      int usedUnits = 0;

      for (int j = activeStart; j < charCount; ++j) {
        const String &token = shown[j];
        const int u = tokenUnits(token);
        if (u <= 0) {
          // shown 的右侧隐藏区是连续空串，遇到后可直接停止扫描。
          if (j > activeStart) break;
          continue;
        }

        if (usedUnits + u > kLineUnitCap && frameLineText[li].length() > 0) {
          ++li;
          usedUnits = 0;
          if (li >= kMaxLines) break;
        }

        frameLineText[li] += token;
        usedUnits += u;
      }

      lineCount = li + 1;
    }

    while (lineCount > 1 && frameLineText[lineCount - 1].length() == 0) {
      lineCount--;
    }
    if (lineCount <= 0) {
      lineCount = 1;
      frameLineText[0] = "";
    }

    int localMiddleY = kGobalYmiddle - kSpriteScreenY;
    if (localMiddleY < (kLineH / 2)) localMiddleY = (kLineH / 2);
    if (localMiddleY > (kSpriteH - (kLineH / 2))) localMiddleY = kSpriteH - (kLineH / 2);

    int baseY = localMiddleY - ((lineCount - 1) * kLineH) / 2;
    int baseYMin = kLineH / 2;
    int baseYMax = kSpriteH - (lineCount - 1) * kLineH - (kLineH / 2);
    if (baseY < baseYMin) baseY = baseYMin;
    if (baseY > baseYMax) baseY = baseYMax;

    const bool globalRefresh = forceGlobalRefresh || (prevCurrentLineCount != lineCount);

    int firstChangedLine = -1;
    if (!globalRefresh) {
      for (int li = 0; li < lineCount; ++li) {
        if (frameLineText[li] != lastFrameLineText[li]) {
          firstChangedLine = li;
          break;
        }
      }
      if (firstChangedLine < 0) return;
    }

    int drawStartLine = 0;
    int dirtyY0 = 0;
    int dirtyY1 = kSpriteH;
    if (!globalRefresh) {
      drawStartLine = firstChangedLine;
      dirtyY0 = baseY + drawStartLine * kLineH - (kLineH / 2);
      dirtyY1 = baseY + (lineCount - 1) * kLineH + (kLineH / 2);
    }

    if (dirtyY0 < 0) dirtyY0 = 0;
    if (dirtyY1 > kSpriteH) dirtyY1 = kSpriteH;
    if (dirtyY1 <= dirtyY0) {
      prevCurrentLineCount = lineCount;
      for (int i = 0; i < kMaxLines; ++i) lastFrameLineText[i] = frameLineText[i];
      return;
    }

    Text.fillRect(0, dirtyY0, kSpriteW, dirtyY1 - dirtyY0, TFT_BLACK);
    const bool hitLogo = !(dirtyY1 <= kLogoY || dirtyY0 >= (kLogoY + kLogoH));
    if (hitLogo) {
      Text.pushImage(kLogoX, kLogoY, kLogoW, kLogoH, (uint16_t *)Index_B);
    }

    const int firstLineWidth = Text.textWidth(frameLineText[0].c_str());
    int leftStartX = kCenterX - (firstLineWidth / 2);
    if (leftStartX < 0) leftStartX = 0;
    if (leftStartX > (kSpriteW - 1)) leftStartX = kSpriteW - 1;

    int drawFrom = globalRefresh ? 0 : drawStartLine;
    if (lineCount <= 1) {
      Text.setTextDatum(MC_DATUM);
      for (int li = drawFrom; li < lineCount; ++li) {
        if (!frameLineText[li].length()) continue;
        const int y = baseY + li * kLineH;
        Text.drawString(frameLineText[li], kCenterX, y);
      }
    } else {
      if (drawFrom == 0) {
        Text.setTextDatum(MC_DATUM);
        if (frameLineText[0].length()) Text.drawString(frameLineText[0], kCenterX, baseY);
        drawFrom = 1;
      }
      Text.setTextDatum(ML_DATUM);
      int liStart = (drawFrom > 1) ? drawFrom : 1;
      for (int li = liStart; li < lineCount; ++li) {
        if (!frameLineText[li].length()) continue;
        const int y = baseY + li * kLineH;
        Text.drawString(frameLineText[li], leftStartX, y);
      }
    }
    Text.setTextDatum(MC_DATUM);

    Text.pushSprite(0, kSpriteScreenY + dirtyY0, 0, dirtyY0, kSpriteW, dirtyY1 - dirtyY0);

    prevCurrentLineCount = lineCount;
    for (int i = 0; i < kMaxLines; ++i) lastFrameLineText[i] = frameLineText[i];
  };

  int i = 0;
  bool rollbackPending = false;
  while (i < charCount) {
    bool didRollbackThisFrame = false;

    // 下一帧执行回滚，避免同帧逻辑分叉过重。
    if (rollbackEnabled && rollbackPending) {
      rollbackPending = false;
      didRollbackThisFrame = true;

      i -= kDisturbWidth;
      if (i < 0) i = 0;
      clearTransientState(i - 8, i + 12);
    }

    buildFrame(i, true);
    drawWrapped(i, false);
    delay(10);

    // 回滚触发：仅检查光标左侧 3 个字符（dist=1,2,3）。
    if (!didRollbackThisFrame && rollbackEnabled && i >= 3) {
      const bool allWrong3 = incorrectNow[i - 1] && incorrectNow[i - 2] && incorrectNow[i - 3];
      if (allWrong3) rollbackPending = true;
    }

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
    if (keycode != 2) keyLatch = false;
    if (forceFinishNow) break;

    const int beepProbability =
        constrain(gInsertSoundBaseProbability + static_cast<int>(Sound_count), 0, 100);
    if (random(1, 100) <= beepProbability) {
      Sound_count = 0;
      mixer.playInsert("/BB2.wav");
    } else {
      const int nextCount =
          static_cast<int>(Sound_count) + constrain(gInsertSoundIncreaseProbability, 0, 100);
      Sound_count = static_cast<uint8_t>(nextCount > 255 ? 255 : nextCount);
    }

    if (!didRollbackThisFrame && !rollbackPending) i++;
  }

  for (int j = 0; j < charCount; ++j) shown[j] = chars[j];
  drawWrapped(charCount + kDisturbWidth + kJunkWidth, true);
}

void task_LogoFadeInAndMove(void *pvParameters) {
  TaskHandle_t notifyTask = static_cast<TaskHandle_t>(pvParameters);
  tft.pushImage(160 - 45, 150 - 45, 90, 90, (uint16_t *)Logo_Moon_B);
  for (uint8_t N = 0; N < 48; N++) {
    const uint16_t raw = static_cast<uint16_t>(N + 1) * 255U / 48U;
    ledcWrite(0, scaledBacklightDuty(static_cast<uint8_t>(raw)));
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
    const uint16_t raw = static_cast<uint16_t>(47 - N) * 255U / 48U;
    ledcWrite(0, scaledBacklightDuty(static_cast<uint8_t>(raw)));
    delay(30);
  }
  ledcWrite(0, 0);
  
  tft.fillRect(0, 50, 320, 160, 0x0000);
  tft.pushImage(160 - 60, 150 - 60, 120, 120, (uint16_t *)Index_B);
  delay(50);

  // Release boot wait first so APP init / warning UI can run in parallel.
  if (notifyTask) {
    xTaskNotifyGive(notifyTask);
    notifyTask = nullptr;
  }

  const uint16_t targetDuty = scaledBacklightDuty(255);
  uint16_t duty = static_cast<uint16_t>(ledcRead(0));
  if (duty > targetDuty) {
    duty = targetDuty;
  }
  while (duty < targetDuty) {
    const uint16_t liveDuty = static_cast<uint16_t>(ledcRead(0));
    if (liveDuty > duty) {
      duty = (liveDuty > targetDuty) ? targetDuty : liveDuty;
    }
    if (duty >= targetDuty) break;
    ++duty;
    ledcWrite(0, static_cast<uint8_t>(duty));
    delay(2);
  }
  
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

