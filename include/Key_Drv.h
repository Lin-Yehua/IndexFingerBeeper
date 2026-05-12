/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - Key_init: 模块内部辅助函数。
 * - Key_loop: 模块内部辅助函数。
 * - get_Keycode: 读取、获取或消费对应数据。
 * - get_Keystate: 读取、获取或消费对应数据。
 * - get_Keystate_B: 读取、获取或消费对应数据。
 * - checkKey: 模块内部辅助函数。
 */
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