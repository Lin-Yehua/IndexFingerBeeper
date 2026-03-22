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
  static constexpr int kMaxChars = 128;
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

  const float kWrapUnits = 16.0f;
  int fullLineStart[8] = {0};
  int fullLineEnd[8] = {0};
  bool lineFrozen[8] = {false};
  String frozenLineText[8];
  int fullLineCount = 0;
  int currentLineCount = 1;
  int currentLineStart[8] = {0};
  int currentLineEnd[8] = {0};
  auto charUnit = [&](int idx) -> float {
    return (chars[idx].length() > 1) ? 1.0f : 0.5f;
  };
  {
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
    const int lineH = 18;
    const int y0 = 0;
    const int areaH = 100;
    const int areaW = 320;
    const int kDisturbWidth = 5;  // zones: decoded | disturbed | junk
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

    const int decodeFront = progressI - kDisturbWidth;
    for (int ln = 0; ln < fullLineCount; ++ln) {
      if (!lineFrozen[ln] && (fullLineEnd[ln] - 1) <= decodeFront) {
        String fix = "";
        for (int j = fullLineStart[ln]; j < fullLineEnd[ln]; ++j) fix += chars[j];
        frozenLineText[ln] = fix;
        lineFrozen[ln] = true;
      }
    }

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

    int frozenPrefixFinal = 0;
    while (frozenPrefixFinal < fullLineCount && lineFrozen[frozenPrefixFinal]) {
      frozenPrefixFinal++;
    }

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

    const bool layoutChanged =
        (currentLineCount != prevCurrentLineCount) ||
        (frozenVisiblePrefix != prevFrozenVisiblePrefix);

    const int baseY = 150 - (currentLineCount - 1) * kLayoutShiftPerLine;
    const int imageY = (currentLineCount - 1) * kLayoutShiftPerLine - 60;
    const int clearTop = (frozenVisiblePrefix <= 0) ? y0 : (12 + frozenVisiblePrefix * lineH - 10);
    const int clippedTop = constrain(clearTop, y0, y0 + areaH);
    const int clippedH = (y0 + areaH) - clippedTop;

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

    const int drawStartLn = layoutChanged ? 0 : frozenVisiblePrefix;
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

    if (layoutChanged) {
      Text.pushSprite(0, baseY, 0, y0, areaW, areaH);
    } else if (clippedH > 0) {
      Text.pushSprite(0, baseY + (clippedTop - y0), 0, clippedTop, areaW, clippedH);
    }

    // If vertical anchor moved due line-count change/rollback, clear only uncovered strip.
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

    prevCurrentLineCount = currentLineCount;
    prevFrozenVisiblePrefix = frozenVisiblePrefix;
    prevBaseY = baseY;
  };

  auto tryActivateWrong = [&](int j, int wrongProb) {
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
          shown[j] = chars[j];
        } else {
          char junk = junkChars[random(strlen(junkChars))];
          shown[j] = String(junk);
        }
      }
      drawWrapped(shown, i);
      delay(10);
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
    delay(20);
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

  if (range > 64) {
    return;
  }

  bool used[64] = {false};

  int generated = 0;
  while (generated < need) {
    int r = random(low, high + 1);
    int idx = r - low;
    if (!used[idx]) {
      used[idx] = true;
      result[generated++] = r;
    }
  }
}
