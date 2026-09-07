/*
 * stress_test.c - UDP 压力测试模块（第七阶段）
 *
 * 说明：
 *   - 发送方向：ESP32 按统一测试包格式（sequence + CRC）向 PC 发送 count 个包
 *   - 接收方向：PC 向 ESP32 发送 count 个包，ESP32 通过 udp_test 任务统计，
 *               本任务监控接收计数直到达到目标或超时
 *   - 双向：同时进行
 *   - 完成后打印第十二阶段要求的统计报告与吞吐率
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
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"

#include "test_config.h"
#include "ethernet_manager.h"
#include "network_monitor.h"
#include "udp_test.h"
#include "stress_test.h"

static const char *TAG = "STRESS";

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_stop_request = false;

/* 测试包大小表 */
static const uint16_t s_sizes[UDP_TEST_SIZE_COUNT] = UDP_TEST_SIZES;

/* ------------------------------------------------------------------ */
/* 进度打印                                                            */
/* ------------------------------------------------------------------ */

static void stress_progress(const char *what, uint64_t done, uint64_t total)
{
    if (done == 0 || done == total || (done % 10000) == 0) {
        ESP_LOGI(TAG, "[%s] %s / %s", what,
                 network_monitor_format_u64(done),
                 network_monitor_format_u64(total));
    }
}

/* ------------------------------------------------------------------ */
/* 发送循环                                                            */
/* ------------------------------------------------------------------ */

static void stress_tx_loop(int sock, const stress_test_params_t *p,
                           uint16_t payload_len, uint32_t start_seq)
{
    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(p->target_port),
    };
    inet_pton(AF_INET, p->target_ip, &target.sin_addr);

    udp_test_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = UDP_TEST_MAGIC;
    pkt.payload_len = payload_len;
    /* payload 填充固定模式，便于 CRC 校验与内容检查 */
    for (uint16_t i = 0; i < payload_len; i++) {
        pkt.payload[i] = (uint8_t)(i * 7 + 3);
    }

    uint32_t sent_ok = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        if (s_stop_request || !ethernet_manager_is_net_ready()) {
            ESP_LOGW(TAG, "TX aborted at %u (stop=%d net_ready=%d)",
                     i, (int)s_stop_request, ethernet_manager_is_net_ready());
            break;
        }
        pkt.sequence = start_seq + i;
        pkt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        /* CRC 覆盖 magic..payload */
        uint32_t crc_len = (uint32_t)(sizeof(uint32_t) * 3 + sizeof(uint16_t) + payload_len);
        pkt.crc = udp_test_crc16((const uint8_t *)&pkt, crc_len);

        ssize_t n = sendto(sock, &pkt,
                           (size_t)(sizeof(uint32_t) * 3 + sizeof(uint16_t) + payload_len + sizeof(uint16_t)),
                           0, (struct sockaddr *)&target, sizeof(target));
        if (n > 0) {
            network_monitor_stats_lock();
            network_monitor_get_stats()->tx_packets++;
            network_monitor_get_stats()->tx_bytes += (uint64_t)n;
            network_monitor_stats_unlock();
            sent_ok++;
        }
        stress_progress("TX", sent_ok, p->count);
        /* 高吞吐时让出 CPU */
        if ((i & 0x3FF) == 0) {
            taskYIELD();
        }
    }
    ESP_LOGI(TAG, "TX finished: %u packets sent", sent_ok);
}

/* ------------------------------------------------------------------ */
/* 压力测试任务                                                        */
/* ------------------------------------------------------------------ */

static void stress_task(void *arg)
{
    stress_test_params_t p;
    memcpy(&p, arg, sizeof(p));
    free(arg);

    uint16_t payload_len = s_sizes[p.size_idx % UDP_TEST_SIZE_COUNT];
    const char *dir_str = (p.direction == STRESS_DIR_RX) ? "RX" :
                          (p.direction == STRESS_DIR_TX) ? "TX" : "BIDIR";

    ESP_LOGI(TAG, "========== UDP STRESS TEST START ==========");
    ESP_LOGI(TAG, "Direction   : %s (%s)", dir_str, p.direction == STRESS_DIR_RX ? "PC->ESP32" :
             p.direction == STRESS_DIR_TX ? "ESP32->PC" : "PC<->ESP32");
    ESP_LOGI(TAG, "Payload size: %u bytes", payload_len);
    ESP_LOGI(TAG, "Packet count: %s", network_monitor_format_u64(p.count));
    ESP_LOGI(TAG, "Target      : %s:%u", p.target_ip, p.target_port);

    /* 统计清零（本测试窗口内的绝对计数） */
    network_monitor_stats_reset();

    uint64_t t0 = esp_timer_get_time();

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 发送方向：立即开始发送（从 seq=1 开始） */
    if (p.direction == STRESS_DIR_TX || p.direction == STRESS_DIR_BIDIR) {
        stress_tx_loop(sock, &p, payload_len, 1);
    }

    /* 接收方向：等待 PC 发送 count 个包（由 udp_test 任务统计） */
    if (p.direction == STRESS_DIR_RX || p.direction == STRESS_DIR_BIDIR) {
        uint64_t timeout_us = (uint64_t)STRESS_RX_TIMEOUT_S * 1000000ULL;
        uint64_t rx_start = esp_timer_get_time();
        uint64_t last_print = 0;
        while (!s_stop_request && ethernet_manager_is_net_ready()) {
            network_monitor_stats_lock();
            uint64_t rx = network_monitor_get_stats()->rx_packets;
            network_monitor_stats_unlock();
            if (rx >= p.count) {
                break;
            }
            uint64_t now = esp_timer_get_time();
            if ((now - rx_start) > timeout_us) {
                ESP_LOGW(TAG, "RX timeout after %llu s (got %s packets)",
                         (unsigned long long)((now - rx_start) / 1000000ULL),
                         network_monitor_format_u64(rx));
                break;
            }
            if (now - last_print > 5000000ULL) {
                ESP_LOGI(TAG, "[RX] received %s / %s",
                         network_monitor_format_u64(rx),
                         network_monitor_format_u64(p.count));
                last_print = now;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    close(sock);

    uint64_t t1 = esp_timer_get_time();
    double duration_s = (double)(t1 - t0) / 1000000.0;

    /* ---------------- 统计报告 ---------------- */
    network_monitor_stats_lock();
    network_test_stats_t *st = network_monitor_get_stats();
    uint64_t tx_pkts = st->tx_packets;
    uint64_t rx_pkts = st->rx_packets;
    uint64_t tx_bytes = st->tx_bytes;
    uint64_t rx_bytes = st->rx_bytes;
    uint64_t lost = st->lost_packets;
    uint64_t dup = st->duplicate_packets;
    uint64_t ooo = st->out_of_order_packets;
    uint64_t derr = st->data_error_packets;
    network_monitor_stats_unlock();

    double loss_rate = 0.0;
    uint64_t total_expected = rx_pkts + lost;
    if (total_expected > 0) {
        loss_rate = (double)lost * 100.0 / (double)total_expected;
    }
    double total_bits = (double)(rx_bytes + tx_bytes) * 8.0;
    double mbps = (duration_s > 0) ? (total_bits / duration_s / 1000000.0) : 0.0;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== UDP STRESS TEST ==========");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Packets TX       : %s", network_monitor_format_u64(tx_pkts));
    ESP_LOGI(TAG, "Packets RX       : %s", network_monitor_format_u64(rx_pkts));
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Lost             : %s", network_monitor_format_u64(lost));
    ESP_LOGI(TAG, "Duplicate        : %s", network_monitor_format_u64(dup));
    ESP_LOGI(TAG, "Out of Order     : %s", network_monitor_format_u64(ooo));
    ESP_LOGI(TAG, "Data Error       : %s", network_monitor_format_u64(derr));
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Loss Rate        : %f %%", loss_rate);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "RX Bytes         : %s", network_monitor_format_u64(rx_bytes));
    ESP_LOGI(TAG, "TX Bytes         : %s", network_monitor_format_u64(tx_bytes));
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Duration         : %.3f seconds", duration_s);
    ESP_LOGI(TAG, "Throughput       : %.2f Mbps (RX+TX aggregate)", mbps);
    ESP_LOGI(TAG, "======================================");

    s_running = false;
    s_stop_request = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t stress_test_run(const stress_test_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "Stress test already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ethernet_manager_is_net_ready()) {
        ESP_LOGW(TAG, "Network not ready, cannot start stress test");
        return ESP_ERR_INVALID_STATE;
    }
    /* RX/BIDIR 依赖 udp_test 任务接收并统计，未启用时自动拉起 */
    if (params->direction != STRESS_DIR_TX && !udp_test_is_enabled()) {
        ESP_LOGW(TAG, "UDP test not running, starting it for RX statistics");
        udp_test_start();
    }
    stress_test_params_t *copy = malloc(sizeof(stress_test_params_t));
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, params, sizeof(*copy));

    s_stop_request = false;
    s_running = true;
    BaseType_t ret = xTaskCreate(stress_task, "stress_test", 8192, copy, 8, &s_task);
    if (ret != pdPASS) {
        free(copy);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t stress_test_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_stop_request = true;
    ESP_LOGI(TAG, "Stop request sent");
    return ESP_OK;
}

bool stress_test_is_running(void)
{
    return s_running;
}

void *stress_test_get_task_handle(void)
{
    return s_task;
}
