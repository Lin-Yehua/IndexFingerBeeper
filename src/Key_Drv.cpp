/*
 * 文件说明: 模块实现文件。
 * 文件功能: 实现对应模块的运行逻辑和内部辅助函数。
 *
 * 函数表:
 * - Key_init: 模块内部辅助函数。
 * - get_Keystate: 读取、获取或消费对应数据。
 * - Key_loop: 模块内部辅助函数。
 * - get_Keycode: 读取、获取或消费对应数据。
 */
#include <Key_Drv.h>

#define LEFT_PIN 2

uint8_t Keycode = 255;

void Key_init()
{
  pinMode(LEFT_PIN, INPUT);
  Keycode = 255;
}
uint8_t get_Keystate()
{
  uint8_t code = 255;
  if (digitalRead(LEFT_PIN) == LOW)
  {
    code = 2;
  }

  return code;
}
void Key_loop()
{
  static uint8_t now_State = 255;
  static uint16_t LongPressCount = 0;
  uint8_t last_State = 255;
  last_State = now_State;
  now_State = get_Keystate();

  if (last_State != now_State && now_State != 255)
  {
    Keycode = now_State;
  }
  else if (now_State == last_State && now_State != 255)
  {
    if (LongPressCount <= 400)
    {
      LongPressCount++;
    }
    if (LongPressCount == 50)
    {
      Keycode = 3;
    }
  }
  else if (now_State != last_State && now_State == 255)
  {
    LongPressCount = 0;
  }
}
uint8_t get_Keycode()
{
  uint8_t Temp;
  Temp = Keycode;
  Keycode = 255;
  return Temp;
}
