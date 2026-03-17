#include <Key_Drv.h>

#define UP_PIN 33
#define DOWN_PIN 35
#define LEFT_PIN 32
#define RIGHT_PIN 34

uint8_t Keycode = 255;

void Key_init()
{
    pinMode(UP_PIN,INPUT);
    pinMode(DOWN_PIN,INPUT);
    pinMode(LEFT_PIN,INPUT);
    pinMode(RIGHT_PIN,INPUT);
    Keycode = 255;
}
uint8_t get_Keystate()
{
    uint8_t code = 255;
    if (digitalRead(UP_PIN) == LOW)
    {
        code = 0;
    }
    else if (digitalRead(DOWN_PIN) == LOW)
    {
        code = 1;
    }
    else if (digitalRead(LEFT_PIN) == LOW)
    {
        code = 2;
    }
    else if (digitalRead(RIGHT_PIN) == LOW)
    {
        code = 3;
    }
    
    return code;
}
uint8_t get_Keystate_B()
{
    uint8_t code = 255;
    if (digitalRead(UP_PIN) == LOW)
    {
        code |= 0x01;
    }
    else
    {
        code &= 0xFE;
    }
     if (digitalRead(DOWN_PIN) == LOW)
    {
        code |= 0x02;
    }
    else
    {
        code &= 0xFD;
    }
     if (digitalRead(LEFT_PIN) == LOW)
    {
        code |= 0x04;
    }
    else
    {
        code &= 0xFB;
    }
     if (digitalRead(RIGHT_PIN) == LOW)
    {
        code |= 0x08;
    }
    else
    {
        code &= 0xF7;
    }
    return code;
}
void Key_loop()
{
    static uint8_t now_State =255;
    uint8_t last_State = 255;
    last_State = now_State;
    now_State = get_Keystate();

    if(last_State != now_State && now_State != 255)
    {
        Keycode = now_State;
    }
}
uint8_t get_Keycode()
{
    uint8_t Temp;
    Temp = Keycode;
    Keycode = 255;
    return Temp;
}

/*
单次检测,keycode为4时为任意按键，适用于单次触发
*/
bool checkKey(uint8_t keycode)
{
    if (keycode != 4)
    {
      if(get_Keystate() == keycode)
      {
        return true;
      }
    }
    else
    {
      if(get_Keystate() != 255)
      {
        return true;
      }
    }
    return false;
}