/*
 * console_menu.h - 以太网验证串口菜单（从原 main.c 抽取）
 *
 * 由 Kconfig 项 CONFIG_AIRNODE_TEST_TOOLS_ENABLE 控制编译；
 * 生产版（裁剪验证工具）下本模块整体不参与编译。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动控制台菜单任务（USB-Serial/JTAG 或 UART 控制台）
 */
esp_err_t console_menu_start(void);

#ifdef __cplusplus
}
#endif
