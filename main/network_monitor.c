/*
 * network_monitor.c - 网络状态监控与测试统计
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "test_config.h"
#include "ethernet_manager.h"
#include "tcp_test.h"
#include "udp_test.h"
#include "stress_test.h"
#include "network_monitor.h"

static const char *TAG = "MONITOR";

static SemaphoreHandle_t       s_stats_mutex = NULL;
static network_test_stats_t    s_stats = { 0 };
static TaskHandle_t            s_monitor_task = NULL;

/* 长稳测试 */
static volatile bool     s_stability_active = false;
static volatile uint32_t s_stability_duration_s = 0;
static volatile uint64_t s_stability_start_us = 0;

/* ------------------------------------------------------------------ */
/* 统计访问                                                            */
/* ------------------------------------------------------------------ */

network_test_stats_t *network_monitor_get_stats(void)
{
    return &s_stats;
}

void network_monitor_stats_lock(void)
{
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    }
}

void network_monitor_stats_unlock(void)
{
    if (s_stats_mutex) {
        xSemaphoreGive(s_stats_mutex);
    }
}

void network_monitor_stats_reset(void)
{
    network_monitor_stats_lock();
    memset(&s_stats, 0, sizeof(s_stats));
    network_monitor_stats_unlock();
}

/* ------------------------------------------------------------------ */
/* 格式化                                                              */
/* ------------------------------------------------------------------ */

const char *network_monitor_format_u64(uint64_t value)
{
    /* 8 个轮转缓冲区，支持同一 printf 内最多 8 次调用 */
    static char bufs[8][32];
    static uint8_t idx = 0;
    char *buf = bufs[(idx++) & 7];
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value);
    int len = strlen(tmp);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[j++] = ',';
        }
        buf[j++] = tmp[i];
    }
    buf[j] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* 状态打印                                                            */
/* ------------------------------------------------------------------ */

static void monitor_print_status(void)
{
    bool link_up = ethernet_manager_is_link_up();
    bool ready   = ethernet_manager_is_net_ready();

    ESP_LOGI(TAG, "------------------------------------------------");
    ESP_LOGI(TAG, "Uptime      : %llu s",
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    ESP_LOGI(TAG, "Link        : %s", link_up ? "UP" : "DOWN");
    if (link_up) {
        ESP_LOGI(TAG, "Speed       : %s", ethernet_manager_get_speed_str());
        ESP_LOGI(TAG, "Duplex      : %s", ethernet_manager_get_duplex_str());
        esp_netif_ip_info_t ip = { 0 };
        if (ethernet_manager_get_ip_info(&ip)) {
            ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&ip.ip));
        }
    }
    ESP_LOGI(TAG, "Net Ready   : %s", ready ? "YES" : "NO");

    network_monitor_stats_lock();
    ESP_LOGI(TAG, "RX packets  : %s", network_monitor_format_u64(s_stats.rx_packets));
    ESP_LOGI(TAG, "TX packets  : %s", network_monitor_format_u64(s_stats.tx_packets));
    ESP_LOGI(TAG, "RX bytes    : %s", network_monitor_format_u64(s_stats.rx_bytes));
    ESP_LOGI(TAG, "TX bytes    : %s", network_monitor_format_u64(s_stats.tx_bytes));
    ESP_LOGI(TAG, "Lost        : %llu  Dup: %llu  OOO: %llu  DataErr: %llu",
             (unsigned long long)s_stats.lost_packets,
             (unsigned long long)s_stats.duplicate_packets,
             (unsigned long long)s_stats.out_of_order_packets,
             (unsigned long long)s_stats.data_error_packets);
    network_monitor_stats_unlock();

    ESP_LOGI(TAG, "Free heap   : %lu B (min %lu B)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    /* 任务状态 */
    ESP_LOGI(TAG, "Tasks       : TCP=%s UDP=%s Stress=%s",
             tcp_test_is_enabled() ? (tcp_test_is_client_connected() ? "connected" : "listening") : "off",
             udp_test_is_enabled() ? "running" : "off",
             stress_test_is_running() ? "running" : "idle");
    TaskHandle_t h = NULL;
    if ((h = tcp_test_get_task_handle()) != NULL) {
        ESP_LOGI(TAG, "  TCP task watermark: %lu",
                 (unsigned long)uxTaskGetStackHighWaterMark(h));
    }
    if ((h = udp_test_get_task_handle()) != NULL) {
        ESP_LOGI(TAG, "  UDP task watermark: %lu",
                 (unsigned long)uxTaskGetStackHighWaterMark(h));
    }
    if ((h = stress_test_get_task_handle()) != NULL) {
        ESP_LOGI(TAG, "  Stress task watermark: %lu",
                 (unsigned long)uxTaskGetStackHighWaterMark(h));
    }

    if (s_stability_active) {
        uint64_t elapsed = (esp_timer_get_time() - s_stability_start_us) / 1000000ULL;
        ESP_LOGI(TAG, "Stability   : %s / %s s",
                 network_monitor_format_u64(elapsed),
                 network_monitor_format_u64(s_stability_duration_s));
    }
    ESP_LOGI(TAG, "------------------------------------------------");
}

/* ------------------------------------------------------------------ */
/* 监控任务                                                            */
/* ------------------------------------------------------------------ */

static void monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        monitor_print_status();
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));

        if (s_stability_active) {
            uint64_t elapsed = (esp_timer_get_time() - s_stability_start_us) / 1000000ULL;
            if (elapsed >= s_stability_duration_s) {
                ESP_LOGI(TAG, "========== STABILITY TEST COMPLETE ==========");
                ESP_LOGI(TAG, "Duration     : %s s", network_monitor_format_u64(elapsed));
                network_monitor_print_stats();
                ESP_LOGI(TAG, "============================================");
                s_stability_active = false;
            }
        }
    }
}

esp_err_t network_monitor_start(void)
{
    if (s_monitor_task != NULL) {
        return ESP_OK;
    }
    if (s_stats_mutex == NULL) {
        s_stats_mutex = xSemaphoreCreateMutex();
        if (s_stats_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t ret = xTaskCreate(monitor_task, "eth_monitor", 4096, NULL, 5, &s_monitor_task);
    if (ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 统计打印                                                            */
/* ------------------------------------------------------------------ */

void network_monitor_print_stats(void)
{
    ESP_LOGI(TAG, "========== NETWORK TEST STATISTICS ==========");
    network_monitor_stats_lock();
    ESP_LOGI(TAG, "Packets TX       : %s", network_monitor_format_u64(s_stats.tx_packets));
    ESP_LOGI(TAG, "Packets RX       : %s", network_monitor_format_u64(s_stats.rx_packets));
    ESP_LOGI(TAG, "Bytes TX         : %s", network_monitor_format_u64(s_stats.tx_bytes));
    ESP_LOGI(TAG, "Bytes RX         : %s", network_monitor_format_u64(s_stats.rx_bytes));
    ESP_LOGI(TAG, "Lost             : %s", network_monitor_format_u64(s_stats.lost_packets));
    ESP_LOGI(TAG, "Duplicate        : %s", network_monitor_format_u64(s_stats.duplicate_packets));
    ESP_LOGI(TAG, "Out of Order     : %s", network_monitor_format_u64(s_stats.out_of_order_packets));
    ESP_LOGI(TAG, "Data Error       : %s", network_monitor_format_u64(s_stats.data_error_packets));
    uint64_t total = s_stats.rx_packets + s_stats.lost_packets;
    double loss = (total > 0) ? ((double)s_stats.lost_packets * 100.0 / (double)total) : 0.0;
    ESP_LOGI(TAG, "Loss Rate        : %f %%", loss);
    network_monitor_stats_unlock();
    ESP_LOGI(TAG, "=============================================");
}

/* ------------------------------------------------------------------ */
/* 长稳测试                                                            */
/* ------------------------------------------------------------------ */

void network_monitor_start_stability_test(uint32_t duration_s)
{
    s_stability_duration_s = duration_s;
    s_stability_start_us = esp_timer_get_time();
    s_stability_active = true;
    ESP_LOGI(TAG, "Stability test started: %s s", network_monitor_format_u64(duration_s));
}

void network_monitor_stop_stability_test(void)
{
    if (s_stability_active) {
        ESP_LOGI(TAG, "Stability test stopped by user");
    }
    s_stability_active = false;
}

bool network_monitor_is_stability_active(void)
{
    return s_stability_active;
}
