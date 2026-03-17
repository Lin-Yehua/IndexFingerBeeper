#ifndef _KEY_DRV_
#define _KEY_DRV_
#include <Arduino.h>

void Key_init();
void Key_loop();
uint8_t get_Keycode();
uint8_t get_Keystate();
uint8_t get_Keystate_B();
bool checkKey(uint8_t keycode);
#endif