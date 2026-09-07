/*
 * net_config.h - 网络参数 NVS 存储
 *
 * 需求：网段默认 192.168.144.20/24（与 STM32 AirNode 一致），
 * 且修改后持久化到 NVS —— 之后改网段/网关只需发 net_set 指令，
 * 无需改代码重新烧录。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加载网络配置到缓存。
 *        NVS 无记录或损坏时，写入默认值（test_config.h 中 ETH_STATIC_*）。
 *        上电在 ethernet_manager_start() 前调用一次即可（幂等）。
 */
esp_err_t net_config_load(void);

/**
 * @brief 获取当前生效的网络参数（dotted 字符串）
 * @param ip_out   至少 16 字节
 * @param mask_out 至少 16 字节
 * @param gw_out   至少 16 字节
 */
esp_err_t net_config_get(char *ip_out, char *mask_out, char *gw_out);

/**
 * @brief 校验并保存网络参数到 NVS（校验失败不写）
 * @return ESP_OK / ESP_ERR_INVALID_ARG(格式错) / 存储错误码
 */
esp_err_t net_config_set(const char *ip, const char *mask, const char *gw);

#ifdef __cplusplus
}
#endif
