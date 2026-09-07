/*
 * airnode_app.c - AirNode 业务应用（自 STM32F407 AirNode 移植）
 *
 * 架构（与 STM32 FreeRTOS 任务模型一一对应）：
 *   airnode_tcp_task  (原 NetTask)    监听 TCP:13550，收帧入路由队列，
 *                                     消费应答队列发给客户端（无客户端则丢弃）
 *   airnode_uart_task (原 SerialTask) UART1(17/18) 收 \r\n\r\n 分帧 JSON 入路由队列
 *   airnode_router_task (原 RouterTask) 取帧 -> JSON 路由 -> 生成应答：
 *                                     入 TCP 应答队列 + 直接回写 UART1
 *
 * 协议修正（相对 STM32 版）：
 *   1. servo_get 返回真实角度（原版硬编码 90°）
 *   2. 新增 servo_query -> servo_status（PC 工具预留指令）
 *   3. config_write 应答回显 closed/released（原版缺失）
 *   4. 新增 net_get / net_set：NVS 网络参数读写，改网段免重烧
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "airnode_config.h"
#include "airnode_app.h"
#include "airnode_json.h"
#include "config_store.h"
#include "servo_controller.h"
#include "ethernet_manager.h"
#include "net_config.h"

static const char *TAG = "AIRNODE";

#define MSG_DELIMITER       "\r\n\r\n"
#define TCP_POLL_MS         20      /* select 轮询间隔，兼顾 Link 变化感知 */

/* 入站/应答帧（固定长度拷贝进 FreeRTOS 队列，避免指针生命周期问题） */
typedef struct {
    uint8_t data[AIRNODE_MSG_MAX];
    uint16_t len;
} airnode_frame_t;

static QueueHandle_t s_router_q = NULL;   /* 入站帧：tcp/uart -> router */
static QueueHandle_t s_send_q   = NULL;   /* 应答帧：router -> tcp       */
static volatile bool s_running  = false;

static TaskHandle_t s_router_task = NULL;
static TaskHandle_t s_tcp_task    = NULL;
static TaskHandle_t s_uart_task   = NULL;

/* ================================================================== */
/* 通用工具                                                             */
/* ================================================================== */

static bool frame_enqueue(QueueHandle_t q, const uint8_t *data, size_t len,
                          const char *who)
{
    if (len == 0) {
        return false;
    }
    if (len >= AIRNODE_MSG_MAX) {
        ESP_LOGW(TAG, "[%s] frame too long (%u), dropped", who, (unsigned)len);
        return false;
    }
    airnode_frame_t f;
    memcpy(f.data, data, len);
    f.len = (uint16_t)len;
    if (xQueueSend(q, &f, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "[%s] queue full, frame dropped", who);
        return false;
    }
    return true;
}

/* 在缓冲中查找 \r\n\r\n，返回分隔符前的长度（不含分隔符），未找到返回 -1 */
static int frame_find_delimiter(const uint8_t *buf, size_t len)
{
    if (len < 4) {
        return -1;
    }
    for (size_t i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)i;
        }
    }
    return -1;
}

/* 统一应答发送：TCP（入应答队列，由 tcp 任务在客户端存在时下发）+ UART（直接写） */
static void send_response(const char *resp)
{
    size_t len = strlen(resp);

    if (s_send_q != NULL) {
        airnode_frame_t f;
        if (len < sizeof(f.data)) {
            memcpy(f.data, resp, len);
            f.len = (uint16_t)len;
            /* 无客户端时队列由 tcp 任务清空丢弃（与 STM32 行为一致） */
            if (xQueueSend(s_send_q, &f, pdMS_TO_TICKS(50)) != pdTRUE) {
                ESP_LOGW(TAG, "send queue full, TCP response dropped");
            }
        }
    }

    /* PC 配置工具在 UART1（专用，干净隔离，不混控制台日志） */
    uart_write_bytes(AIRNODE_UART_PORT, resp, len);
}

/* 从应答队列取角度字段的辅助 */
static int angle_of_channel(uint8_t ch, int fallback)
{
    int angle = fallback;
    if (servo_controller_get_angle(ch, &angle) != ESP_OK) {
        angle = fallback;
    }
    return angle;
}

/* ================================================================== */
/* 指令路由（RouterTask 主体，逻辑与 STM32 freertos.c 一致）              */
/* ================================================================== */

static void route_json(const char *json)
{
    char command[32];
    char action[16] = "close";
    char cseq[16];
    char resp[AIRNODE_MSG_MAX];
    int channel = 0;
    int angle = 90;
    int closed = 0;
    int released = 0;
    const airnode_channel_cfg_t *cfg;
    esp_err_t err;

    cseq[0] = '\0';
    command[0] = '\0';
    airnode_json_get_string(json, "cseq", cseq, sizeof(cseq));

    if (airnode_json_get_string(json, "command", command, sizeof(command)) != 0) {
        ESP_LOGD(TAG, "no command field, skip");
        return;
    }
    ESP_LOGI(TAG, "CMD: %s (from %s)", command, json);

    /* ================= 配置读取 ================= */
    if (strcmp(command, "config_read") == 0) {
        airnode_json_get_int(json, "ch", &channel);
        cfg = config_store_get((uint8_t)channel);
        if (cfg != NULL) {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"config_read_response\",\"code\":\"200\",\"cseq\":\"%s\","
                     "\"ch\":%d,\"closed\":%u,\"released\":%u}" MSG_DELIMITER,
                     cseq, channel, cfg->closed_pwm, cfg->released_pwm);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"config_read_response\",\"code\":\"400\",\"cseq\":\"%s\","
                     "\"ch\":%d,\"msg\":\"bad ch\"}" MSG_DELIMITER, cseq, channel);
        }
        send_response(resp);
    }
    /* ================= 配置写入（应答回显 closed/released，协议修正） ================= */
    else if (strcmp(command, "config_write") == 0) {
        airnode_json_get_int(json, "ch", &channel);
        airnode_json_get_int(json, "closed", &closed);
        airnode_json_get_int(json, "released", &released);

        if (channel >= 0 && channel < AIRNODE_CHANNEL_COUNT) {
            err = config_store_set((uint8_t)channel, (uint16_t)closed, (uint16_t)released);
            if (err == ESP_OK) {
                snprintf(resp, sizeof(resp),
                         "{\"command\":\"config_write\",\"code\":\"200\",\"ch\":%d,"
                         "\"closed\":%d,\"released\":%d,\"cseq\":\"%s\",\"msg\":\"saved\"}"
                         MSG_DELIMITER, channel, closed, released, cseq);
            } else {
                snprintf(resp, sizeof(resp),
                         "{\"command\":\"config_write\",\"code\":\"500\",\"ch\":%d,"
                         "\"cseq\":\"%s\",\"msg\":\"nvs error\"}" MSG_DELIMITER,
                         channel, cseq);
            }
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"config_write\",\"code\":\"400\",\"ch\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"bad ch\"}" MSG_DELIMITER, channel, cseq);
        }
        send_response(resp);
    }
    /* ================= 抛投触发 ================= */
    else if (strcmp(command, "servo_trigger") == 0) {
        airnode_json_get_int(json, "ch", &channel);
        airnode_json_get_string(json, "action", action, sizeof(action));

        cfg = config_store_get((uint8_t)channel);
        if (cfg != NULL) {
            uint16_t target_pwm = cfg->closed_pwm;
            if (strcmp(action, "release") == 0) {
                target_pwm = cfg->released_pwm;
            }
            ESP_LOGI(TAG, "trigger: ch=%d action=%s pwm=%u", channel, action, target_pwm);

            err = servo_controller_set_pulse_us((uint8_t)channel, target_pwm);
            if (err == ESP_OK) {
                snprintf(resp, sizeof(resp),
                         "{\"command\":\"servo_trigger\",\"code\":\"200\",\"ch\":%d,"
                         "\"action\":\"%s\",\"cseq\":\"%s\",\"msg\":\"ok\"}" MSG_DELIMITER,
                         channel, action, cseq);
            } else {
                snprintf(resp, sizeof(resp),
                         "{\"command\":\"servo_trigger\",\"code\":\"500\",\"ch\":%d,"
                         "\"action\":\"%s\",\"cseq\":\"%s\",\"msg\":\"pwm error\"}"
                         MSG_DELIMITER, channel, action, cseq);
            }
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_trigger\",\"code\":\"400\",\"ch\":%d,"
                     "\"action\":\"%s\",\"cseq\":\"%s\",\"msg\":\"bad ch\"}" MSG_DELIMITER,
                     channel, action, cseq);
        }
        send_response(resp);
    }
    /* ================= 角度设置 ================= */
    else if (strcmp(command, "servo_set") == 0) {
        airnode_json_get_int(json, "ch", &channel);
        airnode_json_get_int(json, "angle", &angle);
        if (angle < AIRNODE_SERVO_MIN_ANGLE) {
            angle = AIRNODE_SERVO_MIN_ANGLE;
        }
        if (angle > AIRNODE_SERVO_MAX_ANGLE) {
            angle = AIRNODE_SERVO_MAX_ANGLE;
        }

        if (channel >= 0 && channel < AIRNODE_CHANNEL_COUNT) {
            err = servo_controller_set_angle((uint8_t)channel, angle);
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_set\",\"code\":\"%s\",\"ch\":%d,\"angle\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"%s\"}" MSG_DELIMITER,
                     (err == ESP_OK) ? "200" : "500", channel, angle, cseq,
                     (err == ESP_OK) ? "ok" : "pwm error");
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_set\",\"code\":\"400\",\"ch\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"bad ch\"}" MSG_DELIMITER, channel, cseq);
        }
        send_response(resp);
    }
    /* ================= 角度查询（修正：真实角度，非硬编码 90） ================= */
    else if (strcmp(command, "servo_get") == 0) {
        airnode_json_get_int(json, "ch", &channel);
        if (channel >= 0 && channel < AIRNODE_CHANNEL_COUNT) {
            angle = angle_of_channel((uint8_t)channel, 90);
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_get\",\"code\":\"200\",\"ch\":%d,\"angle\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"ok\"}" MSG_DELIMITER, channel, angle, cseq);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_get\",\"code\":\"400\",\"ch\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"bad ch\"}" MSG_DELIMITER, channel, cseq);
        }
        send_response(resp);
    }
    /* ================= 舵机状态查询（servo_query -> servo_status，PC 工具预留） ================= */
    else if (strcmp(command, "servo_query") == 0) {
        uint16_t pulse = 1500;
        airnode_json_get_int(json, "ch", &channel);
        if (channel >= 0 && channel < AIRNODE_CHANNEL_COUNT) {
            angle = angle_of_channel((uint8_t)channel, 90);
            if (servo_controller_get_pulse_us((uint8_t)channel, &pulse) != ESP_OK) {
                pulse = 1500;
            }
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_status\",\"code\":\"200\",\"ch\":%d,"
                     "\"angle\":%d,\"pulse\":%u,\"cseq\":\"%s\",\"msg\":\"ok\"}"
                     MSG_DELIMITER, channel, angle, pulse, cseq);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"servo_status\",\"code\":\"400\",\"ch\":%d,"
                     "\"cseq\":\"%s\",\"msg\":\"bad ch\"}" MSG_DELIMITER, channel, cseq);
        }
        send_response(resp);
    }
    /* ================= 网络参数读取（NVS 持久化） ================= */
    else if (strcmp(command, "net_get") == 0) {
        char ip_s[16], mask_s[16], gw_s[16];
        err = net_config_get(ip_s, mask_s, gw_s);
        if (err == ESP_OK) {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"net_get\",\"code\":\"200\",\"ip\":\"%s\","
                     "\"mask\":\"%s\",\"gw\":\"%s\",\"cseq\":\"%s\",\"msg\":\"ok\"}"
                     MSG_DELIMITER, ip_s, mask_s, gw_s, cseq);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"net_get\",\"code\":\"500\",\"cseq\":\"%s\","
                     "\"msg\":\"nvs error\"}" MSG_DELIMITER, cseq);
        }
        send_response(resp);
    }
    /* ================= 网络参数写入（改网段免重烧） ================= */
    else if (strcmp(command, "net_set") == 0) {
        char ip_s[16], mask_s[16], gw_s[16];
        ip_s[0] = mask_s[0] = gw_s[0] = '\0';
        airnode_json_get_string(json, "ip", ip_s, sizeof(ip_s));
        airnode_json_get_string(json, "mask", mask_s, sizeof(mask_s));
        airnode_json_get_string(json, "gw", gw_s, sizeof(gw_s));

        err = net_config_set(ip_s[0] ? ip_s : NULL,
                             mask_s[0] ? mask_s : NULL,
                             gw_s[0] ? gw_s : NULL);
        if (err == ESP_OK) {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"net_set\",\"code\":\"200\",\"ip\":\"%s\","
                     "\"mask\":\"%s\",\"gw\":\"%s\",\"cseq\":\"%s\",\"msg\":\"saved, applying\"}"
                     MSG_DELIMITER, ip_s, mask_s, gw_s, cseq);
            send_response(resp);
            /* 先让应答发出去（TCP 队列 + UART），再切换 IP，客户端随后重连 */
            vTaskDelay(pdMS_TO_TICKS(200));
            ethernet_manager_reapply_config();
            return;
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"command\":\"net_set\",\"code\":\"400\",\"cseq\":\"%s\","
                     "\"msg\":\"bad ip/mask/gw\"}" MSG_DELIMITER, cseq);
        }
        send_response(resp);
    }
    /* ================= 未知指令 ================= */
    else {
        ESP_LOGI(TAG, "unknown command: %s", command);
        snprintf(resp, sizeof(resp),
                 "{\"command\":\"%s\",\"code\":\"400\",\"cseq\":\"%s\",\"msg\":\"unknown\"}"
                 MSG_DELIMITER, command, cseq);
        send_response(resp);
    }
}

/* ================================================================== */
/* RouterTask                                                          */
/* ================================================================== */

static void router_task(void *arg)
{
    (void)arg;
    airnode_frame_t f;

    ESP_LOGI(TAG, "RouterTask started");
    for (;;) {
        if (xQueueReceive(s_router_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        f.data[f.len] = '\0';
        route_json((const char *)f.data);
    }
}

/* ================================================================== */
/* TCP Server 任务（原 NetTask）：TCP:13550 单连接会话 + 应答下发        */
/* ================================================================== */

/* 把应答队列里的数据全部发给当前客户端（无客户端时丢弃，与 STM32 一致） */
static void tcp_flush_responses(int client_fd)
{
    airnode_frame_t f;
    while (xQueueReceive(s_send_q, &f, 0) == pdTRUE) {
        if (client_fd < 0) {
            continue;   /* 无客户端：丢弃 */
        }
        size_t off = 0;
        while (off < f.len) {
            ssize_t s = send(client_fd, f.data + off, f.len - off, 0);
            if (s > 0) {
                off += (size_t)s;
            } else {
                ESP_LOGW(TAG, "send() failed: %d", errno);
                break;
            }
        }
    }
}

static void tcp_close_socket(int fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

static void tcp_server_task(void *arg)
{
    (void)arg;
    EventGroupHandle_t evt = ethernet_manager_get_event_group();
    int listen_fd = -1;
    int client_fd = -1;
    /* 客户端会话分帧缓冲（单客户端，任务内持有） */
    uint8_t fbuf[AIRNODE_MSG_MAX];
    size_t  pending = 0;

    while (s_running) {
        /* 等待网络就绪；断开时清理残留 socket */
        if (!ethernet_manager_is_net_ready()) {
            if (client_fd >= 0) {
                ESP_LOGW(TAG, "link down, closing client");
                tcp_close_socket(client_fd);
                client_fd = -1;
                pending = 0;
            }
            if (listen_fd >= 0) {
                tcp_close_socket(listen_fd);
                listen_fd = -1;
            }
            if (evt != NULL) {
                xEventGroupWaitBits(evt, ETH_EV_NET_READY_BIT, pdFALSE, pdTRUE,
                                    pdMS_TO_TICKS(500));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        /* 建立监听 */
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
                .sin_port = htons(AIRNODE_TCP_PORT),
                .sin_addr = { .s_addr = htonl(INADDR_ANY) },
            };
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                ESP_LOGE(TAG, "bind :%d failed: %d", AIRNODE_TCP_PORT, errno);
                tcp_close_socket(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (listen(listen_fd, AIRNODE_TCP_BACKLOG) != 0) {
                ESP_LOGE(TAG, "listen() failed: %d", errno);
                tcp_close_socket(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "AirNode TCP Server listening on :%d", AIRNODE_TCP_PORT);
        }

        /* 接受新客户端（select 带超时，可感知 Link/关闭变化） */
        if (client_fd < 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
            int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
            if (sel <= 0) {
                continue;
            }
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd < 0) {
                continue;
            }
            ESP_LOGI(TAG, "client connected: %s:%d",
                     inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            pending = 0;
            /* 丢弃监听期间积压的应答（无人接收） */
            tcp_flush_responses(-1);
            continue;
        }

        /* ======== 客户端会话：跨包累积分帧 + 应答下发 ======== */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = TCP_POLL_MS * 1000 };
        int sel = select(client_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "client select() failed: %d", errno);
                tcp_close_socket(client_fd);
                client_fd = -1;
                pending = 0;
                continue;
            }
        } else if (sel > 0) {
            uint8_t chunk[256];
            ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
            if (n > 0) {
                if (pending + (size_t)n >= sizeof(fbuf)) {
                    ESP_LOGW(TAG, "TCP frame overflow, resetting session buffer");
                    pending = 0;
                }
                memcpy(fbuf + pending, chunk, (size_t)n);
                pending += (size_t)n;

                /* 循环提取所有完整 \r\n\r\n 分帧消息 */
                for (;;) {
                    int delim = frame_find_delimiter(fbuf, pending);
                    if (delim < 0) {
                        break;  /* 等更多数据 */
                    }
                    if (delim > 0) {
                        frame_enqueue(s_router_q, fbuf, (size_t)delim, "tcp");
                    }
                    /* 移除本帧（含分隔符） */
                    size_t consumed = (size_t)delim + 4;
                    pending -= consumed;
                    if (pending > 0) {
                        memmove(fbuf, fbuf + consumed, pending);
                    }
                }
            } else if (n == 0) {
                ESP_LOGI(TAG, "client disconnected");
                tcp_close_socket(client_fd);
                client_fd = -1;
                pending = 0;
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ESP_LOGW(TAG, "recv() error: %d", errno);
                    tcp_close_socket(client_fd);
                    client_fd = -1;
                    pending = 0;
                    continue;
                }
            }
        }
        tcp_flush_responses(client_fd);
    }

    if (client_fd >= 0) {
        tcp_close_socket(client_fd);
    }
    if (listen_fd >= 0) {
        tcp_close_socket(listen_fd);
    }
    s_tcp_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================== */
/* UART 配置口任务（原 SerialTask）：UART1 \r\n\r\n 分帧入路由             */
/* ================================================================== */

static void uart_task(void *arg)
{
    (void)arg;
    uint8_t rbuf[AIRNODE_UART_BUF_SIZE];
    uint8_t frame[AIRNODE_MSG_MAX];
    size_t have = 0;

    ESP_LOGI(TAG, "UART config task started (port=%d tx=%d rx=%d baud=%d)",
             (int)AIRNODE_UART_PORT, AIRNODE_UART_TX_PIN, AIRNODE_UART_RX_PIN,
             AIRNODE_UART_BAUD);

    for (;;) {
        int n = uart_read_bytes(AIRNODE_UART_PORT, rbuf, sizeof(rbuf),
                                pdMS_TO_TICKS(20));
        if (n <= 0) {
            if (n < 0) {
                vTaskDelay(pdMS_TO_TICKS(200));   /* 驱动异常时降速，避免忙转 */
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (have < sizeof(frame) - 1) {
                frame[have++] = rbuf[i];
            } else {
                ESP_LOGW(TAG, "UART frame overflow, resetting");
                have = 0;
            }

            /* 检测 \r\n\r\n 结束符 */
            if (have >= 4 &&
                frame[have - 4] == '\r' && frame[have - 3] == '\n' &&
                frame[have - 2] == '\r' && frame[have - 1] == '\n') {
                size_t msg_len = have - 4;
                frame[msg_len] = '\0';
                if (msg_len > 0) {
                    ESP_LOGI(TAG, "UART frame: %s", (const char *)frame);
                    airnode_frame_t f;
                    if (msg_len < sizeof(f.data)) {
                        memcpy(f.data, frame, msg_len);
                        f.len = (uint16_t)msg_len;
                        if (xQueueSend(s_router_q, &f, pdMS_TO_TICKS(50)) != pdTRUE) {
                            ESP_LOGW(TAG, "router queue full, UART frame dropped");
                        }
                    }
                }
                have = 0;
            }
        }
    }
}

/* ================================================================== */
/* 对外接口                                                            */
/* ================================================================== */

esp_err_t airnode_app_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    /* 1. 通道参数（NVS），失败不影响继续（内部会用默认值） */
    esp_err_t err = config_store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config_store_init: %s, using defaults", esp_err_to_name(err));
    }

    /* 2. 舵机初始化，并默认驱动到"闭合"位置（closed_pwm），避免上电悬空 */
    err = servo_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo_controller_init failed: %s", esp_err_to_name(err));
        return err;
    }
    for (int ch = 0; ch < AIRNODE_CHANNEL_COUNT; ch++) {
        const airnode_channel_cfg_t *cfg = config_store_get((uint8_t)ch);
        if (cfg != NULL) {
            servo_controller_set_pulse_us((uint8_t)ch, cfg->closed_pwm);
        }
    }
    ESP_LOGI(TAG, "servos driven to closed position (power-on default)");

    /* 3. UART1 配置口驱动（专用引脚，与控制台 USB-JTAG 隔离） */
    uart_config_t uart_cfg = {
        .baud_rate  = AIRNODE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(AIRNODE_UART_PORT,
                              AIRNODE_UART_BUF_SIZE, AIRNODE_UART_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(AIRNODE_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(AIRNODE_UART_PORT,
                                               AIRNODE_UART_TX_PIN,
                                               AIRNODE_UART_RX_PIN,
                                               UART_PIN_NO_CHANGE,
                                               UART_PIN_NO_CHANGE));

    /* 4. 队列 */
    s_router_q = xQueueCreate(AIRNODE_ROUTER_QUEUE_LEN, sizeof(airnode_frame_t));
    s_send_q   = xQueueCreate(AIRNODE_SEND_QUEUE_LEN, sizeof(airnode_frame_t));
    if (s_router_q == NULL || s_send_q == NULL) {
        ESP_LOGE(TAG, "queue create failed (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    s_running = true;

    /* 5. 任务 */
    if (xTaskCreate(router_task, "airnode_router", 4096, NULL, 7,
                    &s_router_task) != pdPASS ||
        xTaskCreate(tcp_server_task, "airnode_tcp", 8192, NULL, 6,
                    &s_tcp_task) != pdPASS ||
        xTaskCreate(uart_task, "airnode_uart", 4096, NULL, 5,
                    &s_uart_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed (out of memory)");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AirNode app started: TCP :%d, UART%d @%d, %d servo channels",
             AIRNODE_TCP_PORT, (int)AIRNODE_UART_PORT, AIRNODE_UART_BAUD,
             AIRNODE_CHANNEL_COUNT);
    return ESP_OK;
}

bool airnode_app_is_running(void)
{
    return s_running;
}
