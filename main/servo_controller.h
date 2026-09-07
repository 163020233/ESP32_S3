/*
 * servo_controller.h - 舵机 LEDC PWM 控制器
 *
 * 替代原 STM32 TIM2_PWM 输出。
 * 扩展：记录每通道最后输出脉宽/角度，供 servo_get / servo_query 上报真实状态
 * （修正 STM32 版 servo_get 硬编码返回 90° 的问题）。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LEDC 定时器与所有 AirNode 舵机通道
 */
esp_err_t servo_controller_init(void);

/**
 * @brief 按角度设置舵机，0~180° 映射到 500~2500us
 */
esp_err_t servo_controller_set_angle(uint8_t ch, int angle);

/**
 * @brief 按原始脉宽设置舵机，单位 us（自动限幅 500~2500）
 */
esp_err_t servo_controller_set_pulse_us(uint8_t ch, uint16_t pulse_us);

/**
 * @brief 读取某通道当前输出脉宽（最后成功下发的值）
 * @param[out] pulse_us
 */
esp_err_t servo_controller_get_pulse_us(uint8_t ch, uint16_t *pulse_us);

/**
 * @brief 读取某通道当前角度（由当前脉宽反算，0~180）
 * @param[out] angle
 */
esp_err_t servo_controller_get_angle(uint8_t ch, int *angle);

#ifdef __cplusplus
}
#endif
