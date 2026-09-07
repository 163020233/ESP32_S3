/*
 * config_store.h - AirNode 参数存储
 *
 * 使用 ESP32 NVS 保存每个通道的 closed/released PWM。
 * 替代原 STM32F407 内部 Flash 存储实现。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t closed_pwm;    /* 闭合 PWM，us */
    uint16_t released_pwm;  /* 投掷/释放 PWM，us */
} airnode_channel_cfg_t;

/**
 * @brief 初始化配置：从 NVS 加载；无数据则写默认值
 */
esp_err_t config_store_init(void);

/**
 * @brief 获取通道配置指针
 * @note 指针在写入后仍稳定，内部使用静态数组
 */
const airnode_channel_cfg_t *config_store_get(uint8_t ch);

/**
 * @brief 设置通道配置并写入 NVS
 */
esp_err_t config_store_set(uint8_t ch, uint16_t closed_pwm, uint16_t released_pwm);

#ifdef __cplusplus
}
#endif
