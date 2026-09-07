/*
 * w5500_port.h - W5500 硬件端口层
 *
 * 职责：
 *   1. GPIO 初始化（RST / INT）
 *   2. W5500 硬件复位
 *   3. SPI Bus 初始化 + 阶段一（VERSIONR 与 SPI 双向读写）验证
 *   4. 创建 esp_eth 框架所需的 W5500 MAC/PHY 实例
 *      （组件 esp_eth_driver_w5500 v2.x：MAC 自行创建 SPI 设备，支持 INT 引脚）
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 阶段一 SPI 测试结果 */
typedef struct {
    uint8_t  versionr;          /* VERSIONR 寄存器值，期望 0x04 */
    bool     version_ok;
    uint8_t  mr;                /* MR 寄存器读回值 */
    uint8_t  shar[6];           /* SHAR 写回验证缓冲区 */
    bool     rw_test_ok;        /* SHAR 双向读写验证 */
} w5500_spi_test_result_t;

/**
 * @brief 初始化 W5500 硬件并执行阶段一 SPI 验证
 *
 * 流程：GPIO -> RST 低 -> 延时 -> RST 高 -> 等待 -> SPI Bus -> VERSIONR 检查
 *
 * @param[out] result  可空；非空时回填 SPI 测试结果
 * @return
 *      - ESP_OK: 初始化成功且 VERSIONR = 0x04
 *      - 其他:   初始化失败（硬件/SPI 问题）
 */
esp_err_t w5500_port_init(w5500_spi_test_result_t *result);

/**
 * @brief 创建 esp_eth 框架的 W5500 MAC / PHY 实例
 *
 * 在 w5500_port_init() 之后调用。实例所有权归调用者（ethernet_manager），
 * 由 esp_eth_driver_install() 接管。
 *
 * @param[out] mac_out  W5500 MAC 实例
 * @param[out] phy_out  W5500 PHY 实例
 * @return esp_err_t
 */
esp_err_t w5500_port_create_mac_phy(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out);

/**
 * @brief 读取 W5500 INT 引脚电平（预留调试用）
 */
int w5500_port_get_int_level(void);

#ifdef __cplusplus
}
#endif
