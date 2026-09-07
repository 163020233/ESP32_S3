/*
 * network_monitor.h - 网络状态监控与测试统计
 *
 * 职责：
 *   1. 统一的网络测试统计结构（第十二阶段要求）
 *   2. 周期性打印：链路状态 / 速度 / 双工 / IP / 统计 / 内存 / 任务状态
 *   3. 长时间稳定性测试（第八阶段：1h / 8h / 24h）
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 统一测试统计结构（第十二阶段） */
typedef struct {
    uint64_t tx_packets;
    uint64_t rx_packets;

    uint64_t tx_bytes;
    uint64_t rx_bytes;

    uint64_t lost_packets;
    uint64_t duplicate_packets;
    uint64_t out_of_order_packets;
    uint64_t data_error_packets;
} network_test_stats_t;

/**
 * @brief 启动监控任务
 */
esp_err_t network_monitor_start(void);

/**
 * @brief 获取共享统计结构指针（配合 network_monitor_stats_lock/unlock 使用）
 */
network_test_stats_t *network_monitor_get_stats(void);

/**
 * @brief 统计互斥锁（多任务更新统计时使用）
 */
void network_monitor_stats_lock(void);
void network_monitor_stats_unlock(void);

/**
 * @brief 清零全部统计
 */
void network_monitor_stats_reset(void);

/**
 * @brief 打印完整统计（供菜单"显示统计"使用）
 */
void network_monitor_print_stats(void);

/**
 * @brief 启动长时间稳定性测试
 * @param duration_s 持续时间（秒）
 */
void network_monitor_start_stability_test(uint32_t duration_s);

/**
 * @brief 停止长时间稳定性测试
 */
void network_monitor_stop_stability_test(void);

/**
 * @brief 长稳测试是否进行中
 */
bool network_monitor_is_stability_active(void);

/**
 * @brief 将 uint64 格式化为带千分位字符串（线程局部缓冲区，勿重复使用）
 * @return 静态缓冲区指针
 */
const char *network_monitor_format_u64(uint64_t value);

#ifdef __cplusplus
}
#endif
