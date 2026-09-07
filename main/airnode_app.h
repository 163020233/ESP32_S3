/*
 * airnode_app.h - AirNode 功能应用
 *
 * 在 ESP32-S3 + W5500 网络底座上启动：
 *   - TCP JSON Server (:13550，兼容 Android App ServoManager)
 *   - UART1 JSON 配置口（兼容 PC 工具 thrower_config.py）
 *   - 舵机 PWM
 *   - NVS 参数存储（通道 PWM / 网络参数）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 AirNode 业务功能（NVS 配置 + 舵机 + TCP + UART 任务）
 *
 * 幂等。内部会自动等待以太网就绪后再监听 TCP 端口。
 */
esp_err_t airnode_app_start(void);

/**
 * @brief AirNode 是否已启动
 */
bool airnode_app_is_running(void);

#ifdef __cplusplus
}
#endif
