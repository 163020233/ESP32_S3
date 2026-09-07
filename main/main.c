/*
 * main.c - ESP32-S3 + W5500 AirNode / 以太网验证程序入口
 *
 * 启动编排：
 *   1. NVS 初始化（AirNode 通道/网络参数存储）
 *   2. SPI/W5500 硬件验证
 *   3. Ethernet 链路（静态 IP 192.168.144.20/24，NVS 可覆盖）
 *   4. 按 Kconfig 启动：
 *        - CONFIG_AIRNODE_APP_ENABLE=y        AirNode 业务（TCP:13550 + UART 配置 + 舵机）
 *        - CONFIG_AIRNODE_TEST_TOOLS_ENABLE=y 以太网验证工具（监控/测试/串口菜单）
 *
 * 生产版裁剪：menuconfig 中关闭 AIRNODE_TEST_TOOLS_ENABLE 即可，
 * 验证工具相关源码（console_menu/network_monitor/tcp_test/udp_test/stress_test）
 * 不参与编译，固件只保留以太网底座 + AirNode 业务。
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"

#include "test_config.h"
#include "w5500_port.h"
#include "ethernet_manager.h"
#include "airnode_app.h"

#if CONFIG_AIRNODE_TEST_TOOLS_ENABLE
#include "network_monitor.h"
#include "tcp_test.h"
#include "udp_test.h"
#include "console_menu.h"
#endif

static const char *TAG = "MAIN";

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" ESP32-S3 + W5500 AirNode / Eth Test\n");
    printf("========================================\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, %d core(s), rev v%u.%u", CONFIG_IDF_TARGET,
             chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "Free heap: %lu B", (unsigned long)esp_get_free_heap_size());

    /* ---------- 0. NVS（AirNode 通道参数 + 网络参数持久化） ---------- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s (net config will fall back to defaults)",
                 esp_err_to_name(nvs_err));
    }

    /* ---------- 阶段一：SPI / W5500 硬件验证 ---------- */
    ESP_LOGI(TAG, "[W5500] Starting SPI/W5500 hardware verification...");
    w5500_spi_test_result_t spi_result;
    esp_err_t err = w5500_port_init(&spi_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "W5500 SPI init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Check wiring (MOSI=%d MISO=%d SCLK=%d CS=%d RST=%d)",
                 W5500_MOSI_GPIO, W5500_MISO_GPIO, W5500_SCLK_GPIO,
                 W5500_CS_GPIO, W5500_RST_GPIO);
        return;   /* 硬件不通过则不再继续 */
    }
    ESP_LOGI(TAG, "W5500 SPI                  PASS");

    /* ---------- 阶段二：Ethernet 链路（NVS 优先 / 默认 192.168.144.20） ---------- */
    err = ethernet_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return;
    }

    /* ---------- AirNode 业务（TCP:13550 + UART1 配置 + 舵机） ---------- */
#if CONFIG_AIRNODE_APP_ENABLE
    err = airnode_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AirNode app start failed: %s", esp_err_to_name(err));
    }
#endif

    /* ---------- 以太网验证工具（生产版可裁剪） ---------- */
#if CONFIG_AIRNODE_TEST_TOOLS_ENABLE
    network_monitor_start();    /* 监控打印 + 长稳测试 */
    udp_test_start();           /* UDP Server :5001（默认启动） */
    tcp_test_start();           /* TCP Server :5000 回显（默认启动） */
    err = console_menu_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console menu start failed: %s", esp_err_to_name(err));
    }
#endif

    ESP_LOGI(TAG, "System ready.");
}
