/*
 * tcp_test.c - TCP 测试模块（第四阶段）
 *
 * 实现：
 *   socket -> bind -> listen -> accept -> recv -> 回显 -> send
 * 支持：
 *   - 长连接（持续收发）
 *   - 主动断开（对端 FIN）与被动断开（Link DOWN 或对端 RST）
 *   - 重连（Server 一直监听，对端随时可重连）
 *   - 断网恢复（Link DOWN 时关闭套接字，Link UP 后重新监听）
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "test_config.h"
#include "ethernet_manager.h"
#include "network_monitor.h"
#include "tcp_test.h"

static const char *TAG = "TCP";

#define TCP_BUF_SIZE    2048
#define TCP_POLL_MS     500

static TaskHandle_t s_task = NULL;
static volatile bool s_enabled = false;
static volatile bool s_client_connected = false;

/* ------------------------------------------------------------------ */
/* 辅助                                                                */
/* ------------------------------------------------------------------ */

/* 等待网络就绪，返回 true 表示就绪 */
static bool tcp_wait_net_ready(EventGroupHandle_t evt)
{
    if (evt == NULL) {
        return ethernet_manager_is_net_ready();
    }
    EventBits_t bits = xEventGroupWaitBits(evt, ETH_EV_NET_READY_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(500));
    return (bits & ETH_EV_NET_READY_BIT) != 0;
}

/* 关闭套接字（忽略错误） */
static void tcp_close_socket(int fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

/* ------------------------------------------------------------------ */
/* Server 任务                                                         */
/* ------------------------------------------------------------------ */

static void tcp_server_task(void *arg)
{
    (void)arg;
    EventGroupHandle_t evt = ethernet_manager_get_event_group();
    int listen_fd = -1;
    int client_fd = -1;

    while (s_enabled) {
        /* 等待网络就绪 */
        if (!tcp_wait_net_ready(evt)) {
            /* 链路断开时关闭残留套接字 */
            if (client_fd >= 0) {
                ESP_LOGW(TAG, "Link down, closing client socket");
                tcp_close_socket(client_fd);
                client_fd = -1;
                s_client_connected = false;
            }
            if (listen_fd >= 0) {
                tcp_close_socket(listen_fd);
                listen_fd = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 创建监听套接字 */
        if (listen_fd < 0) {
            listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listen_fd < 0) {
                ESP_LOGE(TAG, "socket() failed: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(TCP_TEST_PORT),
                .sin_addr = { .s_addr = htonl(INADDR_ANY) },
            };
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                ESP_LOGE(TAG, "bind() failed: %d", errno);
                tcp_close_socket(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (listen(listen_fd, 4) != 0) {
                ESP_LOGE(TAG, "listen() failed: %d", errno);
                tcp_close_socket(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "TCP Server listening on port %d", TCP_TEST_PORT);
        }

        /* 等待/接受客户端连接（select 带超时，可感知 Link 变化） */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = TCP_POLL_MS * 1000 };
        int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno != EINTR) {
                ESP_LOGE(TAG, "select() failed: %d", errno);
            }
            continue;
        }
        if (sel == 0) {
            continue;   /* 超时，回到循环检查链路状态 */
        }

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            continue;
        }
        s_client_connected = true;
        ESP_LOGI(TAG, "Client connected: %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* 客户端会话：recv -> 回显 -> send */
        uint8_t *buf = malloc(TCP_BUF_SIZE);
        if (buf == NULL) {
            ESP_LOGE(TAG, "malloc failed");
            tcp_close_socket(client_fd);
            client_fd = -1;
            s_client_connected = false;
            continue;
        }
        uint64_t session_rx = 0, session_tx = 0;
        bool session_done = false;
        while (s_enabled && !session_done && ethernet_manager_is_net_ready()) {
            FD_ZERO(&rfds);
            FD_SET(client_fd, &rfds);
            sel = select(client_fd + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0) {
                if (errno != EINTR) {
                    ESP_LOGE(TAG, "client select() failed: %d", errno);
                }
                session_done = true;
                break;
            }
            if (sel == 0) {
                continue;   /* 超时：回到循环检查链路状态 */
            }
            ssize_t n = recv(client_fd, buf, TCP_BUF_SIZE, 0);
            if (n > 0) {
                network_monitor_stats_lock();
                network_monitor_get_stats()->rx_packets++;
                network_monitor_get_stats()->rx_bytes += (uint64_t)n;
                network_monitor_stats_unlock();
                session_rx += (uint64_t)n;

                /* 回显（验证 ESP32 -> PC 方向） */
                ssize_t sent = send(client_fd, buf, (size_t)n, 0);
                if (sent > 0) {
                    network_monitor_stats_lock();
                    network_monitor_get_stats()->tx_packets++;
                    network_monitor_get_stats()->tx_bytes += (uint64_t)sent;
                    network_monitor_stats_unlock();
                    session_tx += (uint64_t)sent;
                } else {
                    ESP_LOGW(TAG, "send() failed: %d", errno);
                    session_done = true;
                }
            } else if (n == 0) {
                /* 对端主动断开（FIN） */
                ESP_LOGI(TAG, "Client disconnected (session RX=%llu TX=%llu)",
                         (unsigned long long)session_rx, (unsigned long long)session_tx);
                session_done = true;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ESP_LOGW(TAG, "recv() error: %d", errno);
                    session_done = true;
                }
            }
        }
        free(buf);
        tcp_close_socket(client_fd);
        client_fd = -1;
        s_client_connected = false;
        ESP_LOGI(TAG, "Back to listening...");
    }

    /* 清理 */
    if (client_fd >= 0) {
        tcp_close_socket(client_fd);
        s_client_connected = false;
    }
    if (listen_fd >= 0) {
        tcp_close_socket(listen_fd);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t tcp_test_start(void)
{
    if (s_enabled) {
        return ESP_OK;
    }
    s_enabled = true;
    BaseType_t ret = xTaskCreate(tcp_server_task, "tcp_test", 8192, NULL, 6, &s_task);
    if (ret != pdPASS) {
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TCP test started (port %d)", TCP_TEST_PORT);
    return ESP_OK;
}

esp_err_t tcp_test_stop(void)
{
    if (!s_enabled) {
        return ESP_OK;
    }
    s_enabled = false;
    s_client_connected = false;
    ESP_LOGI(TAG, "TCP test stopping...");
    return ESP_OK;
}

bool tcp_test_is_enabled(void)
{
    return s_enabled;
}

bool tcp_test_is_client_connected(void)
{
    return s_client_connected;
}

void *tcp_test_get_task_handle(void)
{
    return s_task;
}
