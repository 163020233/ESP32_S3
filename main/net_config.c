/*
 * net_config.c - 网络参数 NVS 存储实现
 *
 * NVS blob 布局：
 *   uint32_t magic = 0x4E455443 ('NETC')
 *   ip[4] / netmask[4] / gw[4]
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "test_config.h"
#include "airnode_config.h"
#include "net_config.h"

static const char *TAG = "NETCFG";

#define NET_CFG_MAGIC  0x4E455443u   /* 'NETC' */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
} net_cfg_blob_t;

static SemaphoreHandle_t s_mutex = NULL;
static net_cfg_blob_t s_cfg;

/* 点分字符串 -> blob 字节；返回 false 表示格式非法 */
static bool parse_ipv4(const char *str, uint8_t out[4])
{
    esp_ip4_addr_t ip4;
    if (str == NULL || esp_netif_str_to_ip4(str, &ip4) != ESP_OK) {
        return false;
    }
    memcpy(out, &ip4, 4);   /* 小端主机序，与 NVS 保存/回读一致 */
    return true;
}

static void set_defaults(void)
{
    s_cfg.magic = NET_CFG_MAGIC;
    parse_ipv4(ETH_STATIC_IP_ADDR, s_cfg.ip);
    parse_ipv4(ETH_STATIC_NETMASK, s_cfg.mask);
    parse_ipv4(ETH_STATIC_GATEWAY, s_cfg.gw);
}

static void blob_to_strings(const net_cfg_blob_t *b, char *ip, char *mask, char *gw)
{
    snprintf(ip,   16, "%u.%u.%u.%u", b->ip[0],   b->ip[1],   b->ip[2],   b->ip[3]);
    snprintf(mask, 16, "%u.%u.%u.%u", b->mask[0], b->mask[1], b->mask[2], b->mask[3]);
    snprintf(gw,   16, "%u.%u.%u.%u", b->gw[0],   b->gw[1],   b->gw[2],   b->gw[3]);
}

static esp_err_t write_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRNODE_NVS_NET_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, AIRNODE_NVS_NET_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t net_config_load(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 准备默认值作为兜底（NVS 无记录时使用） */
    set_defaults();

    esp_err_t err = ESP_OK;
    nvs_handle_t handle;
    esp_err_t open_err = nvs_open(AIRNODE_NVS_NET_NAMESPACE, NVS_READONLY, &handle);
    if (open_err == ESP_OK) {
        size_t len = sizeof(s_cfg);
        err = nvs_get_blob(handle, AIRNODE_NVS_NET_KEY, &s_cfg, &len);
        nvs_close(handle);
        if (err != ESP_OK || len != sizeof(s_cfg) || s_cfg.magic != NET_CFG_MAGIC) {
            err = ESP_ERR_NOT_FOUND;
        }
    } else {
        err = open_err;
    }

    if (err != ESP_OK) {
        char ip_s[16], mask_s[16], gw_s[16];
        blob_to_strings(&s_cfg, ip_s, mask_s, gw_s);
        ESP_LOGW(TAG, "NVS net config not found (%s), use defaults %s/%s gw=%s",
                 esp_err_to_name(err), ip_s, mask_s, gw_s);
        err = write_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write default net config failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "default net config saved to NVS");
        }
    } else {
        ESP_LOGI(TAG, "net config loaded from NVS");
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t net_config_get(char *ip_out, char *mask_out, char *gw_out)
{
    if (ip_out == NULL || mask_out == NULL || gw_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        esp_err_t err = net_config_load();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    blob_to_strings(&s_cfg, ip_out, mask_out, gw_out);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t net_config_set(const char *ip, const char *mask, const char *gw)
{
    uint8_t ipb[4], maskb[4], gwb[4];
    if (!parse_ipv4(ip, ipb) || !parse_ipv4(mask, maskb) || !parse_ipv4(gw, gwb)) {
        ESP_LOGW(TAG, "invalid net config: ip=%s mask=%s gw=%s",
                 ip ? ip : "(null)", mask ? mask : "(null)", gw ? gw : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        esp_err_t err = net_config_load();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_cfg.ip, ipb, 4);
    memcpy(s_cfg.mask, maskb, 4);
    memcpy(s_cfg.gw, gwb, 4);
    s_cfg.magic = NET_CFG_MAGIC;
    esp_err_t err = write_nvs();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) {
        char ip_s[16], mask_s[16], gw_s[16];
        blob_to_strings(&s_cfg, ip_s, mask_s, gw_s);
        ESP_LOGI(TAG, "net config saved: %s/%s gw=%s", ip_s, mask_s, gw_s);
    }
    return err;
}
