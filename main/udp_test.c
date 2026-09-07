/*
 * udp_test.c - UDP 测试模块（第五、六阶段）
 *
 * 实现：
 *   - UDP Server (0.0.0.0:5001)
 *   - 测试包解析：magic / sequence / timestamp / payload_len / payload / crc
 *   - 完整性检测：丢包(lost) / 重复(duplicate) / 乱序(out_of_order) / 数据错误(data_error)
 *   - 可选回显（默认开），验证双向通信
 *
 * 注意：UDP 本身不保证可靠传输。本模块的统计仅用于"检测"当前网络环境下
 * 是否发生丢包/乱序/重复/数据错误，不代表 W5500 承诺不丢包。
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "test_config.h"
#include "ethernet_manager.h"
#include "network_monitor.h"
#include "udp_test.h"

static const char *TAG = "UDP";

#define UDP_BUF_SIZE    2048
#define UDP_RECV_TIMEOUT_MS 200

static TaskHandle_t s_task = NULL;
static volatile bool s_enabled = false;
static volatile bool s_echo = true;

/* 序列状态（仅本任务访问，无需锁） */
static uint32_t s_last_seq = 0;
static bool     s_first_packet = true;

/* ------------------------------------------------------------------ */
/* CRC16-CCITT (FALSE): poly 0x1021, init 0xFFFF, MSB-first           */
/* ------------------------------------------------------------------ */
uint16_t udp_test_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* 测试包处理                                                          */
/* ------------------------------------------------------------------ */

static void udp_process_packet(const uint8_t *buf, uint32_t len,
                               struct sockaddr_in *from, int sock)
{
    /* 至少包含 magic+seq+ts+payload_len 头部 */
    if (len < (sizeof(uint32_t) * 3 + sizeof(uint16_t))) {
        return;
    }

    const udp_test_packet_t *pkt = (const udp_test_packet_t *)buf;
    if (pkt->magic != UDP_TEST_MAGIC) {
        /* 非测试包：按普通文本消息打印（限制长度避免越界读） */
        int show = (len > 80) ? 80 : (int)len;
        ESP_LOGI(TAG, "Text msg (%u B) from %s:%d: %.*s",
                 (unsigned)len, inet_ntoa(from->sin_addr), ntohs(from->sin_port),
                 show, (const char *)buf);
        return;
    }

    uint16_t payload_len = pkt->payload_len;
    if (payload_len > UDP_TEST_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Bad packet: payload_len=%u exceeds max", payload_len);
        network_monitor_stats_lock();
        network_monitor_get_stats()->data_error_packets++;
        network_monitor_stats_unlock();
        return;
    }
    if (len < (sizeof(uint32_t) * 3 + sizeof(uint16_t) + payload_len + sizeof(uint16_t))) {
        ESP_LOGW(TAG, "Bad packet: len=%u too short for payload_len=%u", (unsigned)len, payload_len);
        network_monitor_stats_lock();
        network_monitor_get_stats()->data_error_packets++;
        network_monitor_stats_unlock();
        return;
    }

    /* CRC 校验：覆盖 magic..payload */
    uint32_t crc_len = (uint32_t)(sizeof(uint32_t) * 3 + sizeof(uint16_t) + payload_len);
    uint16_t calc = udp_test_crc16(buf, crc_len);
    if (calc != pkt->crc) {
        network_monitor_stats_lock();
        network_monitor_get_stats()->data_error_packets++;
        network_monitor_stats_unlock();
        ESP_LOGW(TAG, "CRC ERROR seq=%u (calc=0x%04X pkt=0x%04X)",
                 (unsigned)pkt->sequence, calc, pkt->crc);
        return;
    }

    /* 序列检测：丢包 / 重复 / 乱序 */
    uint32_t seq = pkt->sequence;
    network_monitor_stats_lock();
    network_test_stats_t *st = network_monitor_get_stats();
    if (s_first_packet) {
        s_first_packet = false;
        s_last_seq = seq;
    } else {
        if (seq == s_last_seq + 1) {
            /* 正常顺序 */
        } else if (seq == s_last_seq) {
            st->duplicate_packets++;
            ESP_LOGW(TAG, "Duplicate seq=%u", (unsigned)seq);
        } else if (seq < s_last_seq) {
            st->out_of_order_packets++;
            ESP_LOGW(TAG, "Out of order seq=%u (last=%u)", (unsigned)seq, (unsigned)s_last_seq);
        } else {
            /* seq > last_seq + 1：中间丢包 */
            uint64_t lost = (uint64_t)(seq - s_last_seq - 1);
            st->lost_packets += lost;
            ESP_LOGW(TAG, "Lost %llu packet(s) before seq=%u",
                     (unsigned long long)lost, (unsigned)seq);
        }
        s_last_seq = seq;
    }
    network_monitor_stats_unlock();

    /* 回显（验证 ESP32 -> PC 方向；PC 侧工具可据此测量往返） */
    if (s_echo) {
        ssize_t sent = sendto(sock, buf, len, 0, (struct sockaddr *)from, sizeof(*from));
        if (sent > 0) {
            network_monitor_stats_lock();
            network_monitor_get_stats()->tx_packets++;
            network_monitor_get_stats()->tx_bytes += (uint64_t)sent;
            network_monitor_stats_unlock();
        }
    }
}

/* ------------------------------------------------------------------ */
/* UDP 任务                                                            */
/* ------------------------------------------------------------------ */

static void udp_test_task(void *arg)
{
    (void)arg;
    EventGroupHandle_t evt = ethernet_manager_get_event_group();
    int sock = -1;
    uint8_t *buf = malloc(UDP_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (s_enabled) {
        /* 等待网络就绪 */
        if (!ethernet_manager_is_net_ready()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            if (evt) {
                xEventGroupWaitBits(evt, ETH_EV_NET_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        /* 创建 UDP 套接字 */
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "socket() failed: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval tv = { .tv_sec = 0, .tv_usec = UDP_RECV_TIMEOUT_MS * 1000 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(UDP_TEST_PORT),
                .sin_addr = { .s_addr = htonl(INADDR_ANY) },
            };
            if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                ESP_LOGE(TAG, "bind() failed: %d", errno);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "UDP Server listening on port %d (echo=%s)",
                     UDP_TEST_PORT, s_echo ? "ON" : "OFF");
            s_first_packet = true;
        }

        /* 接收 */
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, UDP_BUF_SIZE, 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            network_monitor_stats_lock();
            network_monitor_get_stats()->rx_packets++;
            network_monitor_get_stats()->rx_bytes += (uint64_t)n;
            network_monitor_stats_unlock();
            udp_process_packet(buf, (uint32_t)n, &from, sock);
        }
        /* 超时/错误：回到循环检查链路状态 */
    }

    if (sock >= 0) {
        close(sock);
    }
    free(buf);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t udp_test_start(void)
{
    if (s_enabled) {
        return ESP_OK;
    }
    s_enabled = true;
    BaseType_t ret = xTaskCreate(udp_test_task, "udp_test", 8192, NULL, 6, &s_task);
    if (ret != pdPASS) {
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UDP test started (port %d)", UDP_TEST_PORT);
    return ESP_OK;
}

esp_err_t udp_test_stop(void)
{
    if (!s_enabled) {
        return ESP_OK;
    }
    s_enabled = false;
    ESP_LOGI(TAG, "UDP test stopping...");
    return ESP_OK;
}

bool udp_test_is_enabled(void)
{
    return s_enabled;
}

void udp_test_set_echo(bool enable)
{
    s_echo = enable;
    ESP_LOGI(TAG, "UDP echo %s", enable ? "ON" : "OFF");
}

bool udp_test_get_echo(void)
{
    return s_echo;
}

void *udp_test_get_task_handle(void)
{
    return s_task;
}
