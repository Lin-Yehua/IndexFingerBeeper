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
  //弃用
  static bool incorrectNow[kMaxChars];

  uint8_t incorrectRange = 0b00000000;

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

  auto charUnit = [&](int idx) -> float 
  {
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

  //随机英文闪烁的字符
  const char *kEnChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  
  
  //冻结行数量
  int FreezentLineNum = 0;
  auto drawWrapped = [&](String shown[], int progressI) {
    // 三段区模型（按 i 的推进方向）：
    // decoded(已破译区) | disturbed(扰动区) | junk(乱码区)
    // kDisturbWidth 决定扰动区宽度（字符数）。
    const int kDisturbWidth = 5;  // zones: decoded | disturbed | junk
    const int lineH = 18;   //行高
    const int lineShift = lineH/2;    //行高:移位用
    const int unitPerLine = 32;     //行最高字符单元数
    const int maxLine = 8;
    int gobleXmiddle = 160;         //屏幕中点
    int gobleYmiddle = 150;
    int spriteXmiddle = 160;         //屏幕中点
    int spriteYmiddle = 50;
    bool isGobalReflush = false;    
    int FreezentLineNum_last = 0;
    String shownLine[maxLine];
    
    //判断一个字符是不是英文并且返回宽度
    auto tokenUnit = [&](const String &s) -> uint16_t 
    {
      if (!s.length()) return 0.0f;
      bool allAscii = true;
      for (int i = 0; i < s.length(); ++i) {
        if (((uint8_t)s[i]) & 0x80) {
          allAscii = false;
          break;
        }
      }
      if (allAscii) return 1 * s.length();
      return 2;
    };

    //计算将要显示的字符串的单位长度
    auto getShowStringLength = [&](const String show[]) -> uint16_t 
    {
      uint16_t Temp = 0;
      for(uint16_t i = 0; i < charCount; i++)
      {
        Temp += tokenUnit(show[i]);
      }
      return Temp;
    };
    
    //计算将要显示的字符串的视觉长度
     auto getShowLength = [&](const String show[]) -> uint16_t 
    {
      return getShowStringLength(show)< 32 ? getShowStringLength(show) * 8 : unitPerLine * 8;
    };
    
    //计算当前进度的冻结行数
    auto getFreezentLineNum = [&](const String show[], int I) -> uint16_t 
    {
      uint16_t Temp = 0;
      for(uint16_t i = 0; i < I - 5; i++)
      {
        Temp += tokenUnit(show[i]);
      }
      return Temp / unitPerLine;
    };
    
    //计算列绘制起始坐标
    auto getBaseXShift = [&](const String show[]) -> uint16_t 
    {
      uint16_t temp = 0;
      temp = getShowStringLength(show);
      if (temp <unitPerLine)
      {
        return ((float)getShowLength(show))/2;
      }
      
      return ((float)unitPerLine * 8)/2;
      
    };
        
    //合并单元行到字符串
    auto mergeLine = [&](const String show[], String* output) -> uint16_t
    {
      uint16_t lenIdx = 0;
      uint16_t lineTemp = 0;
      for (uint16_t i = 0; i < charCount; i++)
      {
        lineTemp += tokenUnit(show[i]);
        if (lineTemp >= 31)
        {
          i--;
          lineTemp = 0;
          lenIdx++;
        }
        else
        {
          output[lenIdx] += show[i];
        }
      }
      
      return lenIdx + 1;
    };

    

    //获取当前的冻结行数量
    FreezentLineNum_last = FreezentLineNum;
    FreezentLineNum = getFreezentLineNum(shown,progressI);
    //计算行数量 = 字符串长度/每行字符串数量+1
    uint16_t lineNow = getShowStringLength(shown) / unitPerLine + 1;
    
    //计算行绘制起始坐标（用于整体更新）
    uint16_t baseY = gobleYmiddle - (lineNow * lineShift);
    //计算列绘制起始坐标（用于整体更新）
    uint16_t baseX = gobleXmiddle- getBaseXShift(shown);
    //计算精灵内的X起始截取坐标
    uint16_t baseX_sprite = baseX;
    //计算精灵内的Y起始截取坐标
    uint16_t baseY_sprite = FreezentLineNum * lineH;
    //计算行绘制局部坐标（用于局部刷新）
    uint16_t shiftY = baseY + (FreezentLineNum * lineH);
    
    uint16_t viewlen = getShowLength(shown);

    uint8_t shownLine_len = mergeLine(shown,shownLine);

    if(progressI == charCount - 1)
    {
      isGobalReflush = true;
    }
    //定向清除屏幕
    
    for (uint8_t i = FreezentLineNum; i < shownLine_len; i++)
    {
      if (i == 0)
      {
        Text.fillRect(spriteXmiddle - viewlen/2 - 2,i * lineH,viewlen + 4,lineH,0x00FF);/*这里的-2和+4是为了安全保证多加的*/
      }
      else
      {
        Text.fillRect(baseX,i * lineH,320-(baseX * 2),lineH,0x0000);
      }
    }
    //绘制背景
    Text.pushImage(gobleXmiddle - 60, (int)(gobleYmiddle-60) - (int)baseY ,120,120,(uint16_t*)Index_B);


    //绘制文字
    for (uint8_t i = FreezentLineNum; i < shownLine_len; i++)
    {
      if (i == 0)
      {
        Text.setTextDatum(TC_DATUM);
        Text.drawString(shownLine[i], gobleXmiddle,i * lineH + 2);
        Text.setTextDatum(TL_DATUM);
      }
      else
      {
        Text.drawString(shownLine[i], baseX,i * lineH  + 2);
      }
    }
    if(isGobalReflush)
    {
      Text.pushSprite(baseX,baseY,baseX_sprite,0,viewlen,(lineNow * lineH));
    }
    else
    {
      Text.pushSprite(baseX,shiftY,baseX_sprite,baseY_sprite,viewlen,((lineNow - FreezentLineNum) * lineH));
    }

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

  
  /*
  填充字符函数
  构建当前帧字符序列：
   - 光标右侧填充乱码
   - 光标左侧做扰动，并可选统计 incorrectNow（用于下一帧回滚判定）
   - 光标位置显示原字符

   参数:光标位置
  */
  auto buildFrame = [&](int cursorI, bool collectIncorrect) 
  {
    if (collectIncorrect) {
      memset(incorrectNow, 0, sizeof(incorrectNow));
      incorrectRange = 0;
    }
    //仅仅对5格内的扰动区做扫描
    int posStart = cursorI - 6 > 0 ? cursorI - 6 : 0;
    for (int j = posStart; j < charCount; ++j) 
    {
      bool isIncorrect = false;

      if (j < cursorI) {
        const int dist = cursorI - j;       //计算距离
        
        if (dist <= 5)                       //距离小于5:扰动区
        {
          //设置错误概率
          int wrongProb = (dist < 3) ? gWrongProb3 : gWrongProb5;
          //添加扰动
          bool isUtf8 = chars[j].length() > 1;
          if (cursorI < charCount) 
          {
            tryActivateWrong(j, wrongProb);
          }
          //判断是否需要替换英文
          if (isUtf8 && !engFlickerActive[j] && random(100) < 15) 
          {
            engFlickerActive[j] = true;
            engFlickerLeft[j] = (uint8_t)random(1, 4);
            engFlickerChar[j] = kEnChars[random((int)strlen(kEnChars))];
          }
          //如果当前需要替换英文,那就换英文,否则换回中文
          if (engFlickerActive[j]) 
          {
            shown[j] = String(engFlickerChar[j]);
            isIncorrect = true;
            if (engFlickerLeft[j] > 0) engFlickerLeft[j]--;
            if (engFlickerLeft[j] == 0) engFlickerActive[j] = false;
          } 
          else if (wrongActive[j]) 
          {
            shown[j] = wrongChars[j];
            isIncorrect = true;
          } 
          else 
          {
            shown[j] = chars[j];
          }

          if (collectIncorrect && dist <= 3) 
          {
            incorrectNow[j] = isIncorrect;
            
            //incorrectRange |= 0b00000001<<dist;
          }

        } 
        else 
        {
          wrongActive[j] = false;
          engFlickerActive[j] = false;
          engFlickerLeft[j] = 0;
          shown[j] = chars[j];
        }

      } 
      //当前光标处总是正确的
      else if (j == cursorI) 
      {
        shown[j] = chars[j];
      } 
      //前面的部分填充乱码
      else 
      {
        char junk = junkChars[random(strlen(junkChars))];
        shown[j] = String(junk);
      }

      
    }
  };

  int i = 0;
  bool rollbackPending = false;
  while (i < charCount) {
    bool didRollbackThisFrame = false;

    // [生成字符序列]：一个跳动就是一帧
    buildFrame(i, false);
    // [带有回滚处理的绘制]
    // 如果上一帧已标记回滚，本帧先执行回退，再重建并绘制。
    if (rollbackEnabled && rollbackPending) {
      rollbackPending = false;
      didRollbackThisFrame = true;

      i -= 5;
      if (i < 0) i = 0;

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

      // 回退后重算当前帧：右侧重新填充乱码，并清空本帧错误标志统计。
      memset(incorrectNow, 0, sizeof(incorrectNow));
      buildFrame(i, false);
    }
    drawWrapped(shown, i);
    delay(10);

    // [检测回滚条件]
    // 本帧执行过回滚则跳过检测，避免刚回退就再次触发。
    if (!didRollbackThisFrame && rollbackEnabled && i >= 2) 
    {
      bool allWrong3 = true;
      for (int j = i - 2; j <= i; ++j) {
        if (j < 0 || j >= charCount || !incorrectNow[j]) {
          allWrong3 = false;
          break;
        }
      }
      if (allWrong3) {
        // 非阻塞：只打标志，在下一帧执行回滚
        rollbackPending = true;
      }
    }
    // [按键处理]
    Key_loop();
    keycode = get_Keycode();
    if (keycode == 2 && !keyLatch) 
    {
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

    // [音效处理]
    if (random(1, 100) <= 30 + Sound_count) {
      Sound_count = 0;
      mixer.playInsert("/BB2.wav");
    } else {
      Sound_count += 5;
    }

    // 正常推进；若已打回滚标志，则停在当前位置，下一帧执行回滚。
    if (!didRollbackThisFrame && !rollbackPending) {
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

