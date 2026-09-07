/*
 * ethernet_manager.h - 以太网链路管理
 *
 * 职责：
 *   1. esp_netif 初始化（静态 IP）
 *   2. esp_eth 驱动安装与启动
 *   3. Link Up / Link Down / Got IP 事件处理
 *   4. 链路状态查询（供测试模块与监控使用）
 *   5. 以太网重启（断网恢复/手动重启）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 链路状态事件位（EventGroup） */
#define ETH_EV_LINK_UP_BIT      (1 << 0)    /* Ethernet Link UP */
#define ETH_EV_NET_READY_BIT    (1 << 1)    /* 链路 + IP 就绪，可进行网络通信 */

/**
 * @brief 初始化并启动以太网（静态 IP）
 *
 * 内部流程：创建 netif -> 静态 IP -> 创建 W5500 MAC/PHY -> 安装驱动
 *           -> netif 绑定 -> 注册事件 -> esp_eth_start
 *
 * @return esp_err_t
 */
esp_err_t ethernet_manager_start(void);

/**
 * @brief 以太网重启（停止并重新启动驱动，netif 保留）
 *
 * 用于断网恢复与手动重启测试。调用后 LINK/NET_READY 位会被清除，
 * 链路恢复后事件处理器会重新置位。
 *
 * @return esp_err_t
 */
esp_err_t ethernet_manager_restart(void);

/**
 * @brief 应用 NVS 中的网络参数到当前 netif（无需重启/重烧）
 *
 * 配合 net_set 指令：改 IP/掩码/网关后即时生效。
 * 已建立的 TCP 连接会因地址变更断开，客户端需重连。
 *
 * @return esp_err_t
 */
esp_err_t ethernet_manager_reapply_config(void);

/**
 * @brief 获取链路状态事件组句柄
 */
EventGroupHandle_t ethernet_manager_get_event_group(void);

/**
 * @brief 当前是否 Link UP
 */
bool ethernet_manager_is_link_up(void);

/**
 * @brief 当前网络是否就绪（Link UP + IP 配置完成）
 */
bool ethernet_manager_is_net_ready(void);

/**
 * @brief 获取当前链路速度字符串，如 "100 Mbps" / "10 Mbps" / "Unknown"
 */
const char *ethernet_manager_get_speed_str(void);

/**
 * @brief 获取当前链路双工字符串，如 "FULL" / "HALF" / "Unknown"
 */
const char *ethernet_manager_get_duplex_str(void);

/**
 * @brief 获取当前 IP 信息
 * @return true 表示成功
 */
bool ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info);

/**
 * @brief 获取 esp_eth 句柄（供底层 ioctl 等使用）
 */
void *ethernet_manager_get_eth_handle(void);

/**
 * @brief 获取 esp_netif 句柄
 */
esp_netif_t *ethernet_manager_get_netif(void);

#ifdef __cplusplus
}
#endif
