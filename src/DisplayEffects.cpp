/*
 * Display effects implementation.
 */
#include "DisplayEffects.h"
#include "AppGlobals.h"
#include "Hanchi_Index.h"
#include "Index_B.h"
#include "Key_Drv.h"
#include "Logo_Moon_B.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>

namespace
{
constexpr int kGlitchMaxChars = 128;
constexpr int kGlitchMaxLines = 8;
constexpr int kGlitchDisturbWidth = 5;
constexpr int kGlitchJunkWidth = 20;
constexpr int kGlitchLineUnitCap = 32;
constexpr int kGlitchLineH = 18;
constexpr int kGlitchSpriteW = 320;
constexpr int kGlitchSpriteH = 120;
constexpr int kGlitchSpriteScreenY = 100;
constexpr int kGlitchGlobalMiddleY = 160;
constexpr int kGlitchCenterX = 160;
constexpr int kGlitchLogoX = 160 - 60;
constexpr int kGlitchLogoY = 50 - 60;
constexpr int kGlitchLogoW = 120;
constexpr int kGlitchLogoH = 120;
constexpr int kGlitchRollbackClearLeft = 8;
constexpr int kGlitchRollbackClearRight = 12;
constexpr int kGlitchMaxRollbackCount = 8;
constexpr const char *kGlitchEnglishChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
bool gLowBatteryWarningVisible = false;
constexpr int kLowBatteryIconX = 8;
constexpr int kLowBatteryIconScreenY = 105;
constexpr int kLowBatteryIconY = kLowBatteryIconScreenY - kGlitchSpriteScreenY;
constexpr int kLowBatteryIconW = 15;
constexpr int kLowBatteryIconH = 6;

struct GlitchEffectState
{
  String chars[kGlitchMaxChars];
  uint8_t srcUnits[kGlitchMaxChars];
  String shown[kGlitchMaxChars];
  bool wrongActive[kGlitchMaxChars];
  String wrongChars[kGlitchMaxChars];
  bool engFlickerActive[kGlitchMaxChars];
  uint8_t engFlickerLeft[kGlitchMaxChars];
  char engFlickerChar[kGlitchMaxChars];
  bool incorrectNow[kGlitchMaxChars];

  int fullLineStart[kGlitchMaxLines];
  int fullLineEnd[kGlitchMaxLines];
  bool lineFrozen[kGlitchMaxLines];
  String frozenLineText[kGlitchMaxLines];
  String frameLineText[kGlitchMaxLines];
  String lastFrameLineText[kGlitchMaxLines];

  int charCount;
  int fullLineCount;
  int prevLineCount;

  void reset()
  {
    charCount = 0;
    fullLineCount = 0;
    prevLineCount = -1;

    for (int i = 0; i < kGlitchMaxChars; ++i)
    {
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

    for (int i = 0; i < kGlitchMaxLines; ++i)
    {
      fullLineStart[i] = 0;
      fullLineEnd[i] = 0;
      lineFrozen[i] = false;
      frozenLineText[i] = "";
      frameLineText[i] = "";
      lastFrameLineText[i] = "";
    }
  }
};

struct GlitchRuntime
{
  int cursor = 0;
  int rollbackCount = 0;
  bool rollbackEnabled = false;
  bool rollbackPending = false;
  bool forceFinishNow = false;
  bool keyLatch = false;
};

uint8_t scaledBacklightDuty(uint8_t rawDuty)
{
  float level = gBacklightLevel;
  if (level != level)
    level = 1.0f;
  if (level < 0.0f)
    level = 0.0f;
  if (level > 1.0f)
    level = 1.0f;

  const int duty = static_cast<int>(static_cast<float>(rawDuty) * level + 0.5f);
  if (duty < 0)
    return 0;
  if (duty > 255)
    return 255;
  return static_cast<uint8_t>(duty);
}

uint32_t displayIntervalDelayMs()
{
  return static_cast<uint32_t>(constrain(gDisplayIntervalMs, 0, 1000));
}

void drawLowBatteryWarningIconOnTextSprite(bool status)
{
  Text.fillRect(kLowBatteryIconX, kLowBatteryIconY, kLowBatteryIconW, kLowBatteryIconH, TFT_BLACK);
  if (!status)
    return;

  Text.drawRect(kLowBatteryIconX, kLowBatteryIconY, 12, 6, TFT_YELLOW);
  Text.fillRect(kLowBatteryIconX + 12, kLowBatteryIconY + 2, 2, 2, TFT_YELLOW);
  Text.fillRect(kLowBatteryIconX + 1, kLowBatteryIconY + 1, 3, 4, TFT_RED);
}

void drawLowBatteryWarningIconOnTft(bool status)
{
  tft.fillRect(kLowBatteryIconX, kLowBatteryIconScreenY, kLowBatteryIconW, kLowBatteryIconH, TFT_BLACK);
  if (!status)
    return;

  tft.drawRect(kLowBatteryIconX, kLowBatteryIconScreenY, 12, 6, TFT_YELLOW);
  tft.fillRect(kLowBatteryIconX + 12, kLowBatteryIconScreenY + 2, 2, 2, TFT_YELLOW);
  tft.fillRect(kLowBatteryIconX + 1, kLowBatteryIconScreenY + 1, 3, 4, TFT_RED);
}

String normalizeGlitchInput(const char *text)
{
  String normalized;
  if (!text)
    return normalized;

  normalized.reserve(strlen(text));
  for (size_t i = 0; text[i] != '\0'; ++i)
  {
    const char ch = text[i];
    if (ch == '\r' || ch == '\n')
      continue;
    if (ch == '\\' && text[i + 1] != '\0')
    {
      const char next = text[i + 1];
      if (next == 'r' || next == 'n')
      {
        ++i;
        continue;
      }
    }
    normalized += ch;
  }
  return normalized;
}

int utf8CodeUnitLength(uint8_t c)
{
  if ((c & 0x80) == 0x00)
    return 1;
  if ((c & 0xE0) == 0xC0)
    return 2;
  if ((c & 0xF0) == 0xE0)
    return 3;
  if ((c & 0xF8) == 0xF0)
    return 4;
  return 1;
}

void splitUtf8Chars(const char *text, GlitchEffectState &state)
{
  if (!text)
    return;

  for (int i = 0; text[i] != '\0' && state.charCount < kGlitchMaxChars;)
  {
    const int charLen = utf8CodeUnitLength(static_cast<uint8_t>(text[i]));
    int validLen = 0;
    while (validLen < charLen && text[i + validLen] != '\0')
      ++validLen;
    if (validLen <= 0)
      break;

    String token;
    for (int j = 0; j < validLen; ++j)
      token += text[i + j];

    state.chars[state.charCount] = token;
    state.srcUnits[state.charCount] = (token.length() > 1) ? 2 : 1;
    ++state.charCount;
    i += validLen;
  }
}

void drawLogoOnlyGlitchFrame()
{
  Text.fillRect(0, 0, kGlitchSpriteW, kGlitchSpriteH, TFT_BLACK);
  Text.pushImage(kGlitchLogoX, kGlitchLogoY, kGlitchLogoW, kGlitchLogoH, (uint16_t *)Index_B);
  overlayLowBatteryWarningOnTextSprite();
  Text.pushSprite(0, kGlitchSpriteScreenY);
}

void buildFinalLineMap(GlitchEffectState &state)
{
  int start = 0;
  int units = 0;

  for (int j = 0; j < state.charCount && state.fullLineCount < kGlitchMaxLines; ++j)
  {
    const int u = state.srcUnits[j];
    if (j > start && units + u > kGlitchLineUnitCap)
    {
      state.fullLineStart[state.fullLineCount] = start;
      state.fullLineEnd[state.fullLineCount] = j;
      ++state.fullLineCount;
      start = j;
      units = 0;
    }
    units += u;
  }

  if (start < state.charCount && state.fullLineCount < kGlitchMaxLines)
  {
    state.fullLineStart[state.fullLineCount] = start;
    state.fullLineEnd[state.fullLineCount] = state.charCount;
    ++state.fullLineCount;
  }

  if (state.fullLineCount <= 0)
  {
    state.fullLineStart[0] = 0;
    state.fullLineEnd[0] = state.charCount;
    state.fullLineCount = 1;
  }
}

bool prepareGlitchText(const char *text, GlitchEffectState &state)
{
  state.reset();
  const String normalized = normalizeGlitchInput(text);
  splitUtf8Chars(normalized.c_str(), state);
  if (state.charCount <= 0)
    return false;
  buildFinalLineMap(state);
  return true;
}

String mutateCharNearBoundary()
{
  uint32_t cp = 0;
  if (random(100) < 15)
    cp = 0x1234;
  else
    cp = static_cast<uint32_t>(Index_Han[random(0, 4001)]);

  char out[5] = {0};
  if (cp <= 0x7F)
  {
    out[0] = static_cast<char>(cp);
  }
  else if (cp <= 0x7FF)
  {
    out[0] = static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  else
  {
    out[0] = static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return String(out);
}

void clearTransientChar(GlitchEffectState &state, int j)
{
  if (j < 0 || j >= state.charCount)
    return;

  state.wrongActive[j] = false;
  state.wrongChars[j] = "";
  state.engFlickerActive[j] = false;
  state.engFlickerLeft[j] = 0;
  state.engFlickerChar[j] = 0;
  state.incorrectNow[j] = false;
}

void clearTransientRange(GlitchEffectState &state, int l, int r)
{
  if (l < 0)
    l = 0;
  if (r >= state.charCount)
    r = state.charCount - 1;

  for (int j = l; j <= r; ++j)
    clearTransientChar(state, j);
}

void tryActivateWrong(GlitchEffectState &state, int j, int wrongProb)
{
  if (j < 0 || j >= state.charCount)
    return;
  if (state.wrongActive[j])
    return;
  if (random(100) >= wrongProb)
    return;

  String candidate = state.chars[j];
  for (int t = 0; t < 6; ++t)
  {
    candidate = mutateCharNearBoundary();
    if (candidate != state.chars[j] && candidate != state.wrongChars[j])
      break;
  }

  if (candidate != state.chars[j])
  {
    state.wrongChars[j] = candidate;
    state.wrongActive[j] = true;
  }
}

void buildSettledToken(GlitchEffectState &state, int j)
{
  state.shown[j] = state.chars[j];
  clearTransientChar(state, j);
}

void buildDisturbedToken(GlitchEffectState &state, int j, int cursorI, bool collectIncorrect)
{
  const int dist = cursorI - j;
  const int wrongProb = (dist <= 3) ? gWrongProb3 : gWrongProb5;
  bool isIncorrect = false;

  if (cursorI < state.charCount)
    tryActivateWrong(state, j, wrongProb);

  const bool isUtf8 = state.chars[j].length() > 1;
  if (isUtf8 && !state.engFlickerActive[j] && random(100) < 15)
  {
    state.engFlickerActive[j] = true;
    state.engFlickerLeft[j] = static_cast<uint8_t>(random(1, 4));
    state.engFlickerChar[j] = kGlitchEnglishChars[random(0, static_cast<int>(strlen(kGlitchEnglishChars)))];
  }

  if (state.engFlickerActive[j])
  {
    state.shown[j] = String(state.engFlickerChar[j]);
    isIncorrect = true;
    if (state.engFlickerLeft[j] > 0)
      --state.engFlickerLeft[j];
    if (state.engFlickerLeft[j] == 0)
      state.engFlickerActive[j] = false;
  }
  else if (state.wrongActive[j])
  {
    state.shown[j] = state.wrongChars[j];
    isIncorrect = true;
  }
  else
  {
    state.shown[j] = state.chars[j];
  }

  if (collectIncorrect && dist <= 3)
    state.incorrectNow[j] = isIncorrect;
}

void buildJunkToken(GlitchEffectState &state, int j)
{
  const int junkLen = static_cast<int>(strlen(junkChars));
  const char junk = junkChars[random(0, junkLen)];
  char s[2] = {junk, '\0'};
  state.shown[j] = String(s);
  clearTransientChar(state, j);
}

void hideToken(GlitchEffectState &state, int j)
{
  state.shown[j] = "";
  clearTransientChar(state, j);
}

void buildGlitchFrame(GlitchEffectState &state, int cursorI, bool collectIncorrect)
{
  if (collectIncorrect)
    memset(state.incorrectNow, 0, sizeof(state.incorrectNow));

  const int disturbStart = (cursorI > kGlitchDisturbWidth) ? (cursorI - kGlitchDisturbWidth) : 0;
  const int junkEnd = cursorI + kGlitchJunkWidth;

  for (int j = 0; j < state.charCount; ++j)
  {
    if (j < disturbStart)
      buildSettledToken(state, j);
    else if (j < cursorI)
      buildDisturbedToken(state, j, cursorI, collectIncorrect);
    else if (j == cursorI)
      buildSettledToken(state, j);
    else if (j <= junkEnd)
      buildJunkToken(state, j);
    else
      hideToken(state, j);
  }
}

int tokenUnits(const String &token)
{
  if (!token.length())
    return 0;
  return (token.length() > 1) ? 2 : 1;
}

int freezeDecodedLines(GlitchEffectState &state, int progressI)
{
  int decodedEnd = progressI - kGlitchDisturbWidth;
  if (decodedEnd < 0)
    decodedEnd = 0;
  if (decodedEnd > state.charCount)
    decodedEnd = state.charCount;

  int frozenPrefix = 0;
  while (frozenPrefix < state.fullLineCount && state.fullLineEnd[frozenPrefix] <= decodedEnd)
    ++frozenPrefix;

  for (int li = 0; li < state.fullLineCount; ++li)
  {
    const bool shouldFreeze = (li < frozenPrefix);
    if (shouldFreeze && !state.lineFrozen[li])
    {
      state.frozenLineText[li] = "";
      for (int j = state.fullLineStart[li]; j < state.fullLineEnd[li]; ++j)
        state.frozenLineText[li] += state.chars[j];
    }
    else if (!shouldFreeze && state.lineFrozen[li])
    {
      state.frozenLineText[li] = "";
    }
    state.lineFrozen[li] = shouldFreeze;
  }

  return frozenPrefix;
}

int buildFrameLines(GlitchEffectState &state, int frozenPrefix)
{
  for (int i = 0; i < kGlitchMaxLines; ++i)
    state.frameLineText[i] = "";

  int lineCount = 0;
  for (int li = 0; li < frozenPrefix && lineCount < kGlitchMaxLines; ++li)
    state.frameLineText[lineCount++] = state.frozenLineText[li];

  const int activeStart = (frozenPrefix < state.fullLineCount) ? state.fullLineStart[frozenPrefix] : state.charCount;
  if (lineCount < kGlitchMaxLines)
  {
    int li = lineCount;
    int usedUnits = 0;

    for (int j = activeStart; j < state.charCount; ++j)
    {
      const String &token = state.shown[j];
      const int u = tokenUnits(token);
      if (u <= 0)
      {
        if (j > activeStart)
          break;
        continue;
      }

      if (usedUnits + u > kGlitchLineUnitCap && state.frameLineText[li].length() > 0)
      {
        ++li;
        usedUnits = 0;
        if (li >= kGlitchMaxLines)
          break;
      }

      state.frameLineText[li] += token;
      usedUnits += u;
    }

    lineCount = li + 1;
  }

  while (lineCount > 1 && state.frameLineText[lineCount - 1].length() == 0)
    --lineCount;

  if (lineCount <= 0)
  {
    lineCount = 1;
    state.frameLineText[0] = "";
  }
  return lineCount;
}

int computeTextBaseY(int lineCount)
{
  int localMiddleY = kGlitchGlobalMiddleY - kGlitchSpriteScreenY;
  if (localMiddleY < (kGlitchLineH / 2))
    localMiddleY = (kGlitchLineH / 2);
  if (localMiddleY > (kGlitchSpriteH - (kGlitchLineH / 2)))
    localMiddleY = kGlitchSpriteH - (kGlitchLineH / 2);

  int baseY = localMiddleY - ((lineCount - 1) * kGlitchLineH) / 2;
  const int baseYMin = kGlitchLineH / 2;
  const int baseYMax = kGlitchSpriteH - (lineCount - 1) * kGlitchLineH - (kGlitchLineH / 2);
  if (baseY < baseYMin)
    baseY = baseYMin;
  if (baseY > baseYMax)
    baseY = baseYMax;
  return baseY;
}

int firstChangedLine(const GlitchEffectState &state, int lineCount)
{
  for (int li = 0; li < lineCount; ++li)
  {
    if (state.frameLineText[li] != state.lastFrameLineText[li])
      return li;
  }
  return -1;
}

bool computeDirtyRegion(const GlitchEffectState &state, int lineCount, int baseY, bool forceGlobalRefresh,
                        int &drawStartLine, int &dirtyY0, int &dirtyY1)
{
  const bool globalRefresh = forceGlobalRefresh || (state.prevLineCount != lineCount);
  if (!globalRefresh)
  {
    drawStartLine = firstChangedLine(state, lineCount);
    if (drawStartLine < 0)
      return false;
    dirtyY0 = baseY + drawStartLine * kGlitchLineH - (kGlitchLineH / 2);
    dirtyY1 = baseY + (lineCount - 1) * kGlitchLineH + (kGlitchLineH / 2);
  }
  else
  {
    drawStartLine = 0;
    dirtyY0 = 0;
    dirtyY1 = kGlitchSpriteH;
  }

  if (dirtyY0 < 0)
    dirtyY0 = 0;
  if (dirtyY1 > kGlitchSpriteH)
    dirtyY1 = kGlitchSpriteH;
  return dirtyY1 > dirtyY0;
}

void redrawLogoIfDirty(int dirtyY0, int dirtyY1)
{
  const bool hitLogo = !(dirtyY1 <= kGlitchLogoY || dirtyY0 >= (kGlitchLogoY + kGlitchLogoH));
  if (hitLogo)
    Text.pushImage(kGlitchLogoX, kGlitchLogoY, kGlitchLogoW, kGlitchLogoH, (uint16_t *)Index_B);
}

void drawSingleLineText(const GlitchEffectState &state, int lineCount, int baseY, int drawFrom)
{
  Text.setTextDatum(MC_DATUM);
  for (int li = drawFrom; li < lineCount; ++li)
  {
    if (!state.frameLineText[li].length())
      continue;
    Text.drawString(state.frameLineText[li], kGlitchCenterX, baseY + li * kGlitchLineH);
  }
}

void drawMultiLineText(const GlitchEffectState &state, int lineCount, int baseY, int drawFrom)
{
  const int firstLineWidth = Text.textWidth(state.frameLineText[0].c_str());
  int leftStartX = kGlitchCenterX - (firstLineWidth / 2);
  if (leftStartX < 0)
    leftStartX = 0;
  if (leftStartX > (kGlitchSpriteW - 1))
    leftStartX = kGlitchSpriteW - 1;

  if (drawFrom == 0)
  {
    Text.setTextDatum(MC_DATUM);
    if (state.frameLineText[0].length())
      Text.drawString(state.frameLineText[0], kGlitchCenterX, baseY);
    drawFrom = 1;
  }

  Text.setTextDatum(ML_DATUM);
  const int liStart = (drawFrom > 1) ? drawFrom : 1;
  for (int li = liStart; li < lineCount; ++li)
  {
    if (!state.frameLineText[li].length())
      continue;
    Text.drawString(state.frameLineText[li], leftStartX, baseY + li * kGlitchLineH);
  }
}

void rememberFrameLines(GlitchEffectState &state, int lineCount)
{
  state.prevLineCount = lineCount;
  for (int i = 0; i < kGlitchMaxLines; ++i)
    state.lastFrameLineText[i] = state.frameLineText[i];
}

void drawWrappedFrame(GlitchEffectState &state, int progressI, bool forceGlobalRefresh)
{
  const int frozenPrefix = freezeDecodedLines(state, progressI);
  const int lineCount = buildFrameLines(state, frozenPrefix);
  const int baseY = computeTextBaseY(lineCount);

  int drawStartLine = 0;
  int dirtyY0 = 0;
  int dirtyY1 = kGlitchSpriteH;
  if (!computeDirtyRegion(state, lineCount, baseY, forceGlobalRefresh, drawStartLine, dirtyY0, dirtyY1))
  {
    rememberFrameLines(state, lineCount);
    return;
  }

  const bool globalRefresh = forceGlobalRefresh || (state.prevLineCount != lineCount);
  Text.fillRect(0, dirtyY0, kGlitchSpriteW, dirtyY1 - dirtyY0, TFT_BLACK);
  redrawLogoIfDirty(dirtyY0, dirtyY1);

  const int drawFrom = globalRefresh ? 0 : drawStartLine;
  if (lineCount <= 1)
    drawSingleLineText(state, lineCount, baseY, drawFrom);
  else
    drawMultiLineText(state, lineCount, baseY, drawFrom);
  Text.setTextDatum(MC_DATUM);

  overlayLowBatteryWarningOnTextSprite();
  Text.pushSprite(0, kGlitchSpriteScreenY + dirtyY0, 0, dirtyY0, kGlitchSpriteW, dirtyY1 - dirtyY0);
  rememberFrameLines(state, lineCount);
}

bool applyPendingRollback(GlitchEffectState &state, GlitchRuntime &runtime)
{
  if (!runtime.rollbackEnabled || !runtime.rollbackPending)
    return false;

  if (runtime.rollbackCount >= kGlitchMaxRollbackCount)
  {
    runtime.rollbackPending = false;
    runtime.rollbackEnabled = false;
    return false;
  }

  runtime.rollbackPending = false;
  ++runtime.rollbackCount;

  runtime.cursor -= kGlitchDisturbWidth;
  if (runtime.cursor < 0)
    runtime.cursor = 0;
  clearTransientRange(state, runtime.cursor - kGlitchRollbackClearLeft, runtime.cursor + kGlitchRollbackClearRight);
  return true;
}

void scheduleRollbackIfNeeded(const GlitchEffectState &state, GlitchRuntime &runtime, bool didRollbackThisFrame)
{
  if (didRollbackThisFrame || !runtime.rollbackEnabled || runtime.cursor < 3)
    return;

  const bool allWrong3 = state.incorrectNow[runtime.cursor - 1] && state.incorrectNow[runtime.cursor - 2] &&
                         state.incorrectNow[runtime.cursor - 3];
  if (!allWrong3)
    return;

  if (runtime.rollbackCount < kGlitchMaxRollbackCount)
    runtime.rollbackPending = true;
  else
    runtime.rollbackEnabled = false;
}

void handleGlitchAnimationKey(GlitchRuntime &runtime)
{
  Key_loop();
  const uint8_t keycode = get_Keycode();
  if (keycode == 2 && !runtime.keyLatch)
  {
    runtime.keyLatch = true;
    if (runtime.rollbackEnabled)
    {
      runtime.rollbackEnabled = false;
      runtime.rollbackPending = false;
    }
    else
    {
      runtime.forceFinishNow = true;
    }
  }
  if (keycode != 2)
    runtime.keyLatch = false;
}

void serviceGlitchInsertSound()
{
  const int beepProbability = constrain(gInsertSoundBaseProbability + static_cast<int>(Sound_count), 0, 100);
  if (random(1, 100) <= beepProbability)
  {
    Sound_count = 0;
    mixer.playInsert("/BB2.wav");
  }
  else
  {
    const int nextCount = static_cast<int>(Sound_count) + constrain(gInsertSoundIncreaseProbability, 0, 100);
    Sound_count = static_cast<uint8_t>(nextCount > 255 ? 255 : nextCount);
  }
}

void advanceGlitchCursor(GlitchRuntime &runtime, bool didRollbackThisFrame)
{
  if (!didRollbackThisFrame && !runtime.rollbackPending)
    ++runtime.cursor;
}

bool runGlitchAnimationStep(GlitchEffectState &state, GlitchRuntime &runtime)
{
  const bool didRollbackThisFrame = applyPendingRollback(state, runtime);

  buildGlitchFrame(state, runtime.cursor, true);
  drawWrappedFrame(state, runtime.cursor, false);
  delay(displayIntervalDelayMs());

  scheduleRollbackIfNeeded(state, runtime, didRollbackThisFrame);
  handleGlitchAnimationKey(runtime);
  if (runtime.forceFinishNow)
    return false;

  serviceGlitchInsertSound();
  advanceGlitchCursor(runtime, didRollbackThisFrame);
  return true;
}

void finalizeGlitchFrame(GlitchEffectState &state)
{
  for (int j = 0; j < state.charCount; ++j)
    state.shown[j] = state.chars[j];
  drawWrappedFrame(state, state.charCount + kGlitchDisturbWidth + kGlitchJunkWidth, true);
}

void finishGlitchEffect(GlitchEffectState &state)
{
  finalizeGlitchFrame(state);
}

} // namespace

void showGlitchEffectUTF8(const char *text)
{
  if (!text)
    return;

  static GlitchEffectState state;
  if (!prepareGlitchText(text, state))
  {
    drawLogoOnlyGlitchFrame();
    return;
  }

  GlitchRuntime runtime;
  runtime.rollbackEnabled = gEnableReprint;
  while (runtime.cursor < state.charCount)
  {
    if (!runGlitchAnimationStep(state, runtime))
      break;
  }

  finishGlitchEffect(state);
}

void task_LogoFadeInAndMove(void *pvParameters)
{
  TaskHandle_t notifyTask = static_cast<TaskHandle_t>(pvParameters);
  tft.pushImage(160 - 45, 150 - 45, 90, 90, (uint16_t *)Logo_Moon_B);
  for (uint8_t N = 0; N < 48; N++)
  {
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
  for (uint8_t i = 0; i <= steps; i++)
  {
    progress = (float)i / steps;
    eased = 0.5f * (1.0f - cosf(progress * 3.1415926f));
    dx = (int)(eased * 85);
    d1 = (int)(eased * 160);
    Text.pushSprite(160 + 40 - dx, 145, 100, 50, d1, 20);
    tft.pushImage(160 - 45 - dx, 150 - 45, 90, 90, (uint16_t *)Logo_Moon_B);
    delay(30);
  }
  delay(2000);
  for (uint8_t N = 0; N < 48; N++)
  {
    const uint16_t raw = static_cast<uint16_t>(47 - N) * 255U / 48U;
    ledcWrite(0, scaledBacklightDuty(static_cast<uint8_t>(raw)));
    delay(30);
  }
  ledcWrite(0, 0);

  tft.fillRect(0, 50, 320, 160, 0x0000);
  tft.pushImage(160 - 60, 150 - 60, 120, 120, (uint16_t *)Index_B);
  delay(50);

  if (notifyTask)
  {
    xTaskNotifyGive(notifyTask);
    notifyTask = nullptr;
  }

  const uint16_t targetDuty = scaledBacklightDuty(255);
  uint16_t duty = static_cast<uint16_t>(ledcRead(0));
  if (duty > targetDuty)
    duty = targetDuty;
  while (duty < targetDuty)
  {
    const uint16_t liveDuty = static_cast<uint16_t>(ledcRead(0));
    if (liveDuty > duty)
      duty = (liveDuty > targetDuty) ? targetDuty : liveDuty;
    if (duty >= targetDuty)
      break;
    ++duty;
    ledcWrite(0, static_cast<uint8_t>(duty));
    delay(2);
  }

  vTaskDelete(NULL);
}

void generateUniqueRandomNumbers(int low, int high, int count, int *result)
{
  if (!result)
    return;
  if (low > high)
    return;
  if (count <= 0)
    return;

  const int range = high - low + 1;
  int need = count;
  if (need > range)
    need = range;
  if (need <= 0)
    return;

  for (int i = 0; i < need; ++i)
    result[i] = low + i;

  for (int seen = need; seen < range; ++seen)
  {
    int j = random(0, seen + 1);
    if (j < need)
      result[j] = low + seen;
  }

  for (int i = need - 1; i > 0; --i)
  {
    int j = random(0, i + 1);
    int tmp = result[i];
    result[i] = result[j];
    result[j] = tmp;
  }
}

void lowBatteryWarning(bool status)
{
  if (gLowBatteryWarningVisible == status)
    return;
  gLowBatteryWarningVisible = status;
  drawLowBatteryWarningIconOnTextSprite(status);
  drawLowBatteryWarningIconOnTft(status);
}

void overlayLowBatteryWarningOnTextSprite()
{
  if (!gLowBatteryWarningVisible)
    return;
  drawLowBatteryWarningIconOnTextSprite(true);
}
