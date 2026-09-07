/*
 * tcp_test.h - TCP 测试模块（第四阶段）
 *
 * ESP32-S3 作为 TCP Server：
 *   IP   = 192.168.1.20
 *   PORT = 5000
 *
 * 验证：连接建立 / 双向数据 / 长连接 / 主动断开 / 被动断开 / 重连
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 TCP Server 测试任务（幂等）
 */
esp_err_t tcp_test_start(void);

/**
 * @brief 停止 TCP Server 测试任务
 */
esp_err_t tcp_test_stop(void);

/**
 * @brief 测试是否启用
 */
bool tcp_test_is_enabled(void);

/**
 * @brief 当前是否有客户端连接
 */
bool tcp_test_is_client_connected(void);

/**
 * @brief 获取 TCP 任务句柄（供监控打印水位）
 */
void *tcp_test_get_task_handle(void);

#ifdef __cplusplus
}
#endif
