/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - showGlitchEffectUTF8: 绘制界面、输出内容或响应请求。
 * - task_LogoFadeInAndMove: FreeRTOS 任务入口或任务控制函数。
 * - generateUniqueRandomNumbers: 模块内部辅助函数。
 */
#pragma once

void showGlitchEffectUTF8(const char *text);
void task_LogoFadeInAndMove(void *pvParameters);
void generateUniqueRandomNumbers(int low, int high, int count, int *result);
