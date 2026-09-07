/*
 * udp_test.h - UDP 测试模块（第五、六阶段）
 *
 * ESP32-S3 作为 UDP Server：
 *   UDP PORT = 5001
 *
 * 数据完整性检测（第六阶段）：
 *   Sequence（丢包/重复/乱序）+ CRC（数据损坏）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 UDP 测试任务（幂等）
 */
esp_err_t udp_test_start(void);

/**
 * @brief 停止 UDP 测试任务
 */
esp_err_t udp_test_stop(void);

/**
 * @brief 测试是否启用
 */
bool udp_test_is_enabled(void);

/**
 * @brief 设置是否回显收到的测试包（默认开）
 * 回显用于验证 ESP32 -> PC 方向与往返时延
 */
void udp_test_set_echo(bool enable);
bool udp_test_get_echo(void);

/**
 * @brief 获取 UDP 任务句柄（供监控打印水位）
 */
void *udp_test_get_task_handle(void);

/**
 * @brief CRC16-CCITT (FALSE) 计算（与 PC 端工具一致的测试包校验算法）
 * @param data 数据指针
 * @param len  数据长度
 */
uint16_t udp_test_crc16(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
