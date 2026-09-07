/*
 * stress_test.h - UDP 压力测试模块（第七阶段）
 *
 * PC 连续发送大量 UDP 数据包（默认 1,000,000），
 * 分别测试 64 / 256 / 512 / 1024 / 1400 字节。
 *
 * 方向：
 *   RX    : PC -> ESP32
 *   TX    : ESP32 -> PC
 *   BIDIR : PC <-> ESP32 双向
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 压力测试方向 */
#define STRESS_DIR_RX        0
#define STRESS_DIR_TX        1
#define STRESS_DIR_BIDIR     2

/* 压力测试参数 */
typedef struct {
    uint8_t        size_idx;      /* 0..4 -> 64/256/512/1024/1400 */
    uint32_t       count;         /* 每方向包数 */
    uint8_t        direction;     /* STRESS_DIR_* */
    char           target_ip[16]; /* PC IP（点分十进制） */
    uint16_t       target_port;   /* PC UDP 端口（默认 5001） */
} stress_test_params_t;

/**
 * @brief 启动一次压力测试（异步，立即返回；结果完成后打印统计报告）
 */
esp_err_t stress_test_run(const stress_test_params_t *params);

/**
 * @brief 停止当前压力测试
 */
esp_err_t stress_test_stop(void);

/**
 * @brief 压力测试是否运行中
 */
bool stress_test_is_running(void);

/**
 * @brief 获取压力测试任务句柄（供监控打印水位）
 */
void *stress_test_get_task_handle(void);

#ifdef __cplusplus
}
#endif
