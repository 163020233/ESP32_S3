/*
 * test_config.h - 统一保存所有测试参数（GPIO / IP / 端口 / 测试参数）
 *
 * ESP32-S3 + W5500 Ethernet 通信验证程序
 * 所有可调参数集中在此处，业务代码不得散落硬编码。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 一、W5500 硬件连接 (GPIO)
 * 严格按照方案需求：
 *   W55_RSTn  -> GPIO4
 *   W55_INTN  -> GPIO5
 *   W55_MOSI  -> GPIO6
 *   W55_MISO  -> GPIO7
 *   W55_SCLK  -> GPIO15
 *   W55_SCSN  -> GPIO16
 * ============================================================ */
#define W5500_RST_GPIO          4
#define W5500_INT_GPIO          5
#define W5500_MOSI_GPIO         6
#define W5500_MISO_GPIO         7
#define W5500_SCLK_GPIO         15
#define W5500_CS_GPIO           16

/* SPI 主机（ESP32-S3 可用 SPI2_HOST / FSPI_HOST） */
#define W5500_SPI_HOST          SPI2_HOST

/* SPI 时钟频率：
 * 初始验证使用稳定配置（10 MHz），确认稳定后可逐步提高。
 * W5500 最高支持约 80 MHz；40 MHz 是吞吐与稳定性的折中。 */
#define W5500_SPI_CLK_HZ        (40 * 1000 * 1000)

/* SPI 模式 0（CPOL=0, CPHA=0） */
#define W5500_SPI_MODE          0

/* W5500 复位时序（毫秒） */
#define W5500_RST_ASSERT_MS     100
#define W5500_RST_RELEASE_DELAY_MS 100

/* 第一阶段不依赖 INT，但保留中断引脚配置（输入上拉，预留） */
#define W5500_INT_ENABLE        0   /* 0 = 暂不使用，1 = 使能 GPIO 中断监视 */

/* ============================================================
 * 二、网络配置（静态 IP，方便测试）
 *   ESP32-S3/W5500 : 192.168.1.20
 *   PC             : 192.168.1.100
 *   Netmask        : 255.255.255.0
 * ============================================================ */
#define ETH_STATIC_IP_ADDR      "192.168.1.20"
#define ETH_STATIC_NETMASK      "255.255.255.0"
#define ETH_STATIC_GATEWAY      "192.168.1.1"

/* 默认测试对端（PC） */
#define TEST_PC_IP_ADDR         "192.168.1.100"

/* ============================================================
 * 三、测试端口
 * ============================================================ */
#define TCP_TEST_PORT           5000
#define UDP_TEST_PORT           5001

/* ============================================================
 * 四、UDP 测试数据包格式
 *
 * PC <-> ESP32 使用统一的测试包：
 *   magic + sequence + timestamp + payload_len + payload + crc
 * 字节序：小端（ESP32 / x86 PC 均为小端，直接使用）
 * CRC16: CCITT-FALSE (poly 0x1021, init 0xFFFF, 不反转, 无异或)
 *        校验范围：magic 到 payload 的最后一个字节
 * ============================================================ */
#define UDP_TEST_MAGIC          0x57355030u      /* 'W5P0' */
#define UDP_TEST_MAX_PAYLOAD    1400

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* UDP_TEST_MAGIC */
    uint32_t sequence;           /* 发送序号，从 1 开始 */
    uint32_t timestamp_ms;       /* 发送端启动后毫秒时间戳 */
    uint16_t payload_len;        /* 实际 payload 字节数 */
    uint8_t  payload[UDP_TEST_MAX_PAYLOAD];
    uint16_t crc;                /* CRC16-CCITT over magic..payload */
} udp_test_packet_t;

/* 可测试的 payload 大小 */
#define UDP_TEST_SIZE_COUNT     5
#define UDP_TEST_SIZES          { 64, 256, 512, 1024, 1400 }

/* ============================================================
 * 五、压力测试参数
 * ============================================================ */
#define STRESS_DEFAULT_COUNT    100000      /* 默认每轮包数（正式验收 1,000,000） */
#define STRESS_MAX_COUNT        10000000    /* 上限保护 */
#define STRESS_RX_TIMEOUT_S     300         /* 收包模式等待超时（秒） */

/* ============================================================
 * 六、监控 / 长稳测试参数
 * ============================================================ */
#define MONITOR_PERIOD_MS       5000        /* 监控任务打印周期 */
#define STABILITY_TEST_1H_S     (3600)      /* 1 小时 */
#define STABILITY_TEST_8H_S     (8 * 3600)
#define STABILITY_TEST_24H_S    (24 * 3600)

#ifdef __cplusplus
}
#endif
