/*
 * airnode_config.h - AirNode 功能配置
 *
 * 在 ESP32-S3 + W5500 底座上实现原 STM32F407 AirNode 的主要功能：
 *   - TCP Server (:13550)，JSON 指令路由（Android App / PC 工具）
 *   - UART1 配置口（PC 工具 thrower_config.py，115200）
 *   - LEDC 舵机 PWM（双通道）
 *   - NVS 保存通道闭合/投掷 PWM 参数与网络参数
 *
 * 引脚说明：
 *   当前为按 ESP32-S3 常用开发板预定的引脚，后续以最终原理图为准。
 */
#pragma once

#include <stdint.h>
#include "driver/ledc.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 通道数量：原 STM32 flash_storage.h 为 2 路 */
#define AIRNODE_CHANNEL_COUNT         2

/* TCP 服务端口：原 STM32 w5500.h 使用 13550，Android App 依赖，保持不变 */
#define AIRNODE_TCP_PORT              13550

/* TCP 监听队列深度（lwIP accept backlog） */
#define AIRNODE_TCP_BACKLOG           4

/* 单条 JSON 消息最大长度（含 \r\n\r\n 分帧符），与 STM32 NetBuf(512) 一致 */
#define AIRNODE_MSG_MAX               512

/* 路由队列深度：NetTask/UART -> RouterTask */
#define AIRNODE_ROUTER_QUEUE_LEN      16
/* 应答队列深度：RouterTask -> TCP NetTask */
#define AIRNODE_SEND_QUEUE_LEN        8

/* ============================================================
 * NVS 存储
 * ============================================================ */
/* 通道 PWM 参数（config_store.c 使用） */
#define AIRNODE_NVS_NAMESPACE         "airnode_cfg"
#define AIRNODE_NVS_KEY               "channels"

/* 网络参数（net_config.c 使用）：
 * 上电从 NVS 加载，缺省则写 192.168.144.20/24（与 STM32 一致），
 * 之后改网段只需 net_set 指令，避免改一次烧一次 */
#define AIRNODE_NVS_NET_NAMESPACE     "airnode_net"
#define AIRNODE_NVS_NET_KEY           "cfg"

/* 默认舵机 PWM（us） */
#define AIRNODE_DEFAULT_CLOSED_PWM    1500
#define AIRNODE_DEFAULT_RELEASED_PWM  2000

/* 舵机安全 PWM 范围 */
#define AIRNODE_SERVO_PWM_MIN_US      500
#define AIRNODE_SERVO_PWM_MAX_US      2500
#define AIRNODE_SERVO_MIN_ANGLE       0
#define AIRNODE_SERVO_MAX_ANGLE       180

/* ============================================================
 * UART 配置口（PC 配置工具），按实际硬件接线修改
 * ============================================================ */
#define AIRNODE_UART_PORT             UART_NUM_1
#define AIRNODE_UART_TX_PIN           17
#define AIRNODE_UART_RX_PIN           18
#define AIRNODE_UART_BAUD             115200
#define AIRNODE_UART_BUF_SIZE         1024

/* ============================================================
 * 舵机 PWM 输出 GPIO，按实际硬件接线修改
 * ============================================================ */
#define AIRNODE_SERVO_GPIO_CH0        8
#define AIRNODE_SERVO_GPIO_CH1        9

/* LEDC 资源映射 */
#define AIRNODE_LEDC_TIMER            LEDC_TIMER_0
#define AIRNODE_LEDC_MODE             LEDC_LOW_SPEED_MODE
#define AIRNODE_LEDC_CH0              LEDC_CHANNEL_0
#define AIRNODE_LEDC_CH1              LEDC_CHANNEL_1
#define AIRNODE_LEDC_DUTY_RES         LEDC_TIMER_14_BIT
#define AIRNODE_LEDC_FREQ_HZ          50

#ifdef __cplusplus
}
#endif
