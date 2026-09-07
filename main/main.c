/*
 * main.c - ESP32-S3 + W5500 以太网通信验证程序入口
 *
 * 流程：
 *   系统启动 -> 初始化基础环境 -> 初始化 Ethernet -> 启动测试任务
 *
 * 只负责编排，不写 W5500/TCP/UDP 业务逻辑。
 * 串口菜单（第十七阶段）提供测试控制，避免每次修改代码切换测试。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"

#include "test_config.h"
#include "w5500_port.h"
#include "ethernet_manager.h"
#include "network_monitor.h"
#include "tcp_test.h"
#include "udp_test.h"
#include "stress_test.h"

static const char *TAG = "MAIN";

/* ================================================================== */
/* 串口菜单                                                            */
/* ================================================================== */

static void print_menu(void)
{
    printf("\n");
    printf("================================\n");
    printf(" ESP32-S3 W5500 Ethernet Test\n");
    printf("================================\n");
    printf("\n");
    printf("1. Network Status\n");
    printf("2. Ping/Network Test\n");
    printf("3. TCP Test\n");
    printf("4. UDP Test\n");
    printf("5. UDP Stress Test\n");
    printf("6. Show Statistics\n");
    printf("7. Start Long-Time Test\n");
    printf("8. Ethernet Restart\n");
    printf("\n");
    printf("Select: ");
    fflush(stdout);
}

/*
 * 控制台单字节读取。
 * 注意：不使用 fgets(stdin) —— USB-Serial/JTAG 主控制台下 newlib stdin
 * 在主机未就绪时会立即返回 EIO，导致菜单刷屏；这里直接读底层驱动。
 */
static esp_err_t console_read_byte(uint8_t *b, TickType_t timeout)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(b, 1, timeout);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, b, 1, timeout);
#else
    (void)b; (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/*
 * 读取一行控制台输入。
 * @param idle_timeout_ms  无输入时的超时（返回 0）
 * @return  >0 输入行长度；0 超时无输入
 */
static int console_read_line(char *buf, int size, uint32_t idle_timeout_ms)
{
    int idx = 0;
    uint32_t last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t start_ms = last_activity_ms;

    while (idx < size - 1) {
        uint8_t b = 0;
        esp_err_t err = console_read_byte(&b, pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            if (b == '\r' || b == '\n') {
                break;
            }
            if (b == '\b' || b == 0x7F) {      /* 退格 */
                if (idx > 0) {
                    idx--;
                }
                continue;
            }
            buf[idx++] = (char)b;
            last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (idx > 0) {
                /* 已有部分输入：回车或超时才提交 */
                if ((now_ms - last_activity_ms) >= idle_timeout_ms) {
                    break;
                }
            } else if ((now_ms - start_ms) >= idle_timeout_ms) {
                return 0;                       /* 完全无输入，超时 */
            }
        }
    }
    buf[idx] = '\0';
    return idx;
}

/* ---------------- 2. Ping ---------------- */

typedef struct {
    SemaphoreHandle_t done;
} ping_ctx_t;

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t seq = 0, gap = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
    printf("Ping reply: seq=%" PRIu32 " time=%" PRIu32 " ms\n", seq, gap);
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    printf("Ping timeout\n");
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->done);
}

static void menu_ping(void)
{
    char line[64];
    char ip_str[16];
    uint32_t count = 4;

    if (!ethernet_manager_is_net_ready()) {
        printf("Network not ready!\n");
        return;
    }
    printf("Ping target IP [%s]: ", TEST_PC_IP_ADDR);
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        snprintf(ip_str, sizeof(ip_str), "%.15s", line);   /* IP 最长 15 字符 */
    } else {
        snprintf(ip_str, sizeof(ip_str), "%.15s", TEST_PC_IP_ADDR);
    }
    printf("Ping count [%lu]: ", (unsigned long)count);
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int v = atoi(line);
        if (v > 0) {
            count = (uint32_t)v;
        }
    }

    ip4_addr_t ip4;
    if (inet_pton(AF_INET, ip_str, &ip4) != 1) {
        printf("Invalid IP: %s\n", ip_str);
        return;
    }

    ping_ctx_t ctx = { .done = xSemaphoreCreateBinary() };
    if (ctx.done == NULL) {
        printf("No memory for ping\n");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = count;
    config.target_addr.type = IPADDR_TYPE_V4;
    config.target_addr.u_addr.ip4 = ip4;

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };

    esp_ping_handle_t ping = NULL;
    if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK) {
        printf("esp_ping_new_session failed\n");
        vSemaphoreDelete(ctx.done);
        return;
    }
    printf("Pinging %s (%lu times)...\n", ip_str, (unsigned long)count);
    esp_ping_start(ping);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(count * 2000 + 5000));

    uint32_t sent = 0, recv = 0, duration = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    esp_ping_get_profile(ping, ESP_PING_PROF_DURATION, &duration, sizeof(duration));
    printf("Ping result: sent=%" PRIu32 " received=%" PRIu32 " duration=%" PRIu32 " ms\n",
           sent, recv, duration);
    printf("%s\n", (recv > 0) ? "PING PASS" : "PING FAIL");

    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);
}

/* ---------------- 5. UDP Stress ---------------- */

static void menu_stress(void)
{
    char line[64];
    stress_test_params_t p;
    memset(&p, 0, sizeof(p));
    p.size_idx = 3;                              /* 默认 1024 */
    p.count = STRESS_DEFAULT_COUNT;
    p.direction = STRESS_DIR_RX;
    snprintf(p.target_ip, sizeof(p.target_ip), "%s", TEST_PC_IP_ADDR);
    p.target_port = UDP_TEST_PORT;

    const uint16_t sizes[UDP_TEST_SIZE_COUNT] = UDP_TEST_SIZES;

    printf("Payload size: 1=64 2=256 3=512 4=1024 5=1400 [4]: ");
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int v = atoi(line);
        if (v >= 1 && v <= UDP_TEST_SIZE_COUNT) {
            p.size_idx = (uint8_t)(v - 1);
        }
    }
    printf("Packet count [%s]: ", network_monitor_format_u64(p.count));
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int64_t v = atoll(line);
        if (v > 0 && v <= STRESS_MAX_COUNT) {
            p.count = (uint32_t)v;
        }
    }
    printf("Direction: 0=RX(PC->ESP32) 1=TX(ESP32->PC) 2=BIDIR [0]: ");
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int v = atoi(line);
        if (v >= STRESS_DIR_RX && v <= STRESS_DIR_BIDIR) {
            p.direction = (uint8_t)v;
        }
    }
    printf("Target IP [%s]: ", TEST_PC_IP_ADDR);
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        snprintf(p.target_ip, sizeof(p.target_ip), "%.15s", line);   /* IP 最长 15 字符 */
    }
    printf("Target port [%d]: ", UDP_TEST_PORT);
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int v = atoi(line);
        if (v > 0 && v < 65536) {
            p.target_port = (uint16_t)v;
        }
    }

    esp_err_t err = stress_test_run(&p);
    if (err == ESP_OK) {
        printf("Stress test started: size=%u count=%s dir=%d target=%s:%u\n",
               sizes[p.size_idx], network_monitor_format_u64(p.count),
               p.direction, p.target_ip, p.target_port);
    } else {
        printf("Stress test start failed: %s\n", esp_err_to_name(err));
    }
}

/* ---------------- 7. Long-Time Test ---------------- */

static void menu_stability(void)
{
    char line[64];
    uint32_t duration = STABILITY_TEST_1H_S;
    printf("Duration: 1=1h 2=8h 3=24h custom=seconds [1]: ");
    fflush(stdout);
    if (console_read_line(line, sizeof(line), 30000) > 0) {
        int v = atoi(line);
        if (v == 2) {
            duration = STABILITY_TEST_8H_S;
        } else if (v == 3) {
            duration = STABILITY_TEST_24H_S;
        } else if (v > 60) {
            duration = (uint32_t)v;
        }
    }
    network_monitor_start_stability_test(duration);
    printf("Long-time test running: %s s (press 7 again or use option to check)\n",
           network_monitor_format_u64(duration));
}

/* ---------------- 菜单任务 ---------------- */

static void console_menu_task(void *arg)
{
    (void)arg;
    char line[64];
    vTaskDelay(pdMS_TO_TICKS(3000));    /* 等系统初始化打印完成 */

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* 控制台 VFS 默认使用"无驱动"模式（直接 LL 读写，不安装环形缓冲驱动），
     * 直接调用 usb_serial_jtag_read_bytes() 会因驱动未安装而崩溃。
     * 这里显式安装驱动，使菜单输入可阻塞读取；已安装时忽略 INVALID_STATE。 */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usj_err = usb_serial_jtag_driver_install(&cfg);
    if (usj_err != ESP_OK && usj_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(usj_err));
    }
#endif

    while (1) {
        print_menu();
        if (console_read_line(line, sizeof(line), 30000) <= 0) {
            /* EOF / 空行（如副控制台只读、UART RX 悬空噪声）：
             * 延时再提示，避免菜单刷屏 */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        int sel = atoi(line);
        printf("\n>> Option %d\n", sel);
        switch (sel) {
        case 1: {   /* Network Status */
            printf("Link        : %s\n", ethernet_manager_is_link_up() ? "UP" : "DOWN");
            if (ethernet_manager_is_link_up()) {
                printf("Speed       : %s\n", ethernet_manager_get_speed_str());
                printf("Duplex      : %s\n", ethernet_manager_get_duplex_str());
                esp_netif_ip_info_t ip = { 0 };
                if (ethernet_manager_get_ip_info(&ip)) {
                    printf("IP          : " IPSTR "\n", IP2STR(&ip.ip));
                }
            }
            printf("Net Ready   : %s\n", ethernet_manager_is_net_ready() ? "YES" : "NO");
            break;
        }
        case 2:
            menu_ping();
            break;
        case 3:
            if (tcp_test_is_enabled()) {
                tcp_test_stop();
                printf("TCP test stopped\n");
            } else {
                tcp_test_start();
                printf("TCP test started (port %d)\n", TCP_TEST_PORT);
            }
            break;
        case 4:
            if (udp_test_is_enabled()) {
                udp_test_stop();
                printf("UDP test stopped\n");
            } else {
                udp_test_start();
                printf("UDP test started (port %d)\n", UDP_TEST_PORT);
            }
            break;
        case 5:
            menu_stress();
            break;
        case 6:
            network_monitor_print_stats();
            break;
        case 7:
            if (network_monitor_is_stability_active()) {
                network_monitor_stop_stability_test();
                printf("Long-time test stopped\n");
            } else {
                menu_stability();
            }
            break;
        case 8:
            ethernet_manager_restart();
            break;
        default:
            printf("Unknown option\n");
            break;
        }
    }
}

/* ================================================================== */
/* 应用入口                                                            */
/* ================================================================== */

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" ESP32-S3 + W5500 Ethernet Test v1.0\n");
    printf("========================================\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, %d core(s), rev v%u.%u", CONFIG_IDF_TARGET,
             chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "Free heap: %lu B", (unsigned long)esp_get_free_heap_size());

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

    /* ---------- 阶段二：Ethernet 链路 ---------- */
    err = ethernet_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return;
    }

    /* ---------- 监控与测试任务 ---------- */
    network_monitor_start();
    udp_test_start();       /* UDP 常开：压力测试接收/完整性检测依赖它 */
    tcp_test_start();       /* TCP Server 常开 */
    xTaskCreate(console_menu_task, "menu", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "System ready. Console menu available.");
}
