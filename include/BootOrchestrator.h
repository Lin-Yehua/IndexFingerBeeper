/*
 * 文件说明: 预启动总控接口头文件。
 * 文件功能: 声明启动路径路由入口，供 main.cpp 调用。
 *
 * 函数表:
 * - routeBootPath: 选择冷启动、USB 模式或深睡快恢复启动路径。
 */
#pragma once

// 预启动总控：初始化外设并选择 USB/APP/深睡快恢复启动路径。
void routeBootPath();

