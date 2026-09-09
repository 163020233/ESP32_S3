/*
 * ethernet_manager.c - 以太网链路管理（第二阶段：Ethernet 链路验证）
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "test_config.h"
#include "w5500_port.h"
#include "ethernet_manager.h"
#include "net_config.h"

static const char *TAG = "ETH";

static EventGroupHandle_t s_evt_group = NULL;
static esp_eth_handle_t   s_eth_handle = NULL;
static esp_netif_t       *s_eth_netif  = NULL;

static volatile bool s_link_up = false;
static volatile bool s_net_ready = false;
static eth_speed_t  s_speed  = ETH_SPEED_10M;
static eth_duplex_t s_duplex = ETH_DUPLEX_HALF;

/* ------------------------------------------------------------------ */
/* 事件处理                                                            */
/* ------------------------------------------------------------------ */

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet driver started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet driver stopped");
        break;
    case ETHERNET_EVENT_CONNECTED: {
        s_link_up = true;
        /* 读取速度与双工 */
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &s_speed) == ESP_OK &&
            esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &s_duplex) == ESP_OK) {
            ESP_LOGI(TAG, "Link UP");
            ESP_LOGI(TAG, "Speed: %s", ethernet_manager_get_speed_str());
            ESP_LOGI(TAG, "Duplex: %s", ethernet_manager_get_duplex_str());
        } else {
            ESP_LOGI(TAG, "Link UP (speed/duplex read failed)");
        }
        /* 静态 IP 已在启动时配置，链路建立即可使用 */
        if (s_evt_group) {
            xEventGroupSetBits(s_evt_group, ETH_EV_LINK_UP_BIT | ETH_EV_NET_READY_BIT);
        }
        s_net_ready = true;
        esp_netif_ip_info_t ip_info = { 0 };
        if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info.ip));
        }
        ESP_LOGI(TAG, "NETWORK READY");
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_net_ready = false;
        if (s_evt_group) {
            xEventGroupClearBits(s_evt_group, ETH_EV_LINK_UP_BIT | ETH_EV_NET_READY_BIT);
        }
        ESP_LOGW(TAG, "Link DOWN");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ip_info->ip));
    s_net_ready = true;
    if (s_evt_group) {
        xEventGroupSetBits(s_evt_group, ETH_EV_NET_READY_BIT);
    }
}

static void lost_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    s_net_ready = false;
    if (s_evt_group) {
        xEventGroupClearBits(s_evt_group, ETH_EV_NET_READY_BIT);
    }
    ESP_LOGW(TAG, "IP lost");
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t ethernet_manager_start(void)
{
    esp_err_t ret;

    if (s_eth_handle != NULL) {
        ESP_LOGW(TAG, "Ethernet already started");
        return ESP_OK;
    }

    /* 1. TCP/IP 网络接口 */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");

    s_evt_group = xEventGroupCreate();
    if (s_evt_group == NULL) {
        ESP_LOGE(TAG, "event group create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 2. 创建默认 ethernet netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "netif create failed");
        return ESP_FAIL;
    }

    /* 3. 静态 IP：
     *    NVS 网络参数优先（首次上电自动落盘默认 192.168.144.20/24，
     *    之后 net_set 修改即时生效且无需重新烧录），
     *    无 NVS 时退回 test_config.h 的 ETH_STATIC_* 默认值 */
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_netif_ip_info_t ip_info = { 0 };
    char ip_s[16], mask_s[16], gw_s[16];
    net_config_load();
    net_config_get(ip_s, mask_s, gw_s);
    if (esp_netif_str_to_ip4(ip_s, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(mask_s, &ip_info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(gw_s, &ip_info.gw) != ESP_OK) {
        ESP_LOGE(TAG, "invalid static IP config");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &ip_info),
                        TAG, "set static IP failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_eth_netif, "esp32s3-w5500"),
                        TAG, "set hostname failed");

    /* 4. 创建 W5500 MAC/PHY 并安装驱动 */
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    ESP_RETURN_ON_ERROR(w5500_port_create_mac_phy(&mac, &phy),
                        TAG, "create W5500 MAC/PHY failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "eth driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. netif 绑定 + 默认接口 */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    if (glue == NULL) {
        ESP_LOGE(TAG, "netif glue create failed");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "netif attach failed");
    esp_netif_set_default_netif(s_eth_netif);

    /* 6. 注册事件 */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   &eth_event_handler, NULL),
                        TAG, "register ETH event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   &got_ip_event_handler, NULL),
                        TAG, "register GOT_IP event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                   &lost_ip_event_handler, NULL),
                        TAG, "register LOST_IP event failed");

    /* 6.5 写入 W5500 MAC：出厂基 MAC + locally-administered 位。
     * 不设置的话运行期 SHAR 为全 0（或阶段一测试残留的相同假 MAC），
     * 同网段多台设备必然冲突，且 SOCK0 开了 MAC 过滤会收不到包。 */
    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG, "read base MAC failed");
    mac_addr[0] = (mac_addr[0] & 0xFC) | 0x02;    /* unicast + locally administered */
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr),
                        TAG, "set ETH MAC failed");
    ESP_LOGI(TAG, "ETH MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);

    /* 7. 启动以太网 */
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start failed");
    ESP_LOGI(TAG, "Ethernet started (static IP %s/%s gw=%s, waiting for link...)",
             ip_s, mask_s, gw_s);
    return ESP_OK;
}

esp_err_t ethernet_manager_reapply_config(void)
{
    if (s_eth_netif == NULL) {
        ESP_LOGW(TAG, "Ethernet not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    char ip_s[16], mask_s[16], gw_s[16];
    esp_err_t err = net_config_get(ip_s, mask_s, gw_s);
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_ip_info_t ip_info = { 0 };
    if (esp_netif_str_to_ip4(ip_s, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(mask_s, &ip_info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(gw_s, &ip_info.gw) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "IP updated: %s/%s gw=%s (existing TCP clients will reconnect)",
                 ip_s, mask_s, gw_s);
    } else {
        ESP_LOGE(TAG, "set IP failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ethernet_manager_restart(void)
{
    if (s_eth_handle == NULL) {
        ESP_LOGW(TAG, "Ethernet not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Ethernet restart...");
    s_link_up = false;
    s_net_ready = false;
    if (s_evt_group) {
        xEventGroupClearBits(s_evt_group, ETH_EV_LINK_UP_BIT | ETH_EV_NET_READY_BIT);
    }
    esp_err_t ret = esp_eth_stop(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "eth stop failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "eth restart failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet restarted, waiting for link...");
    return ESP_OK;
}

EventGroupHandle_t ethernet_manager_get_event_group(void)
{
    return s_evt_group;
}

bool ethernet_manager_is_link_up(void)
{
    return s_link_up;
}

bool ethernet_manager_is_net_ready(void)
{
    return s_net_ready;
}

const char *ethernet_manager_get_speed_str(void)
{
    switch (s_speed) {
    case ETH_SPEED_10M:
        return "10 Mbps";
    case ETH_SPEED_100M:
        return "100 Mbps";
    default:
        return "Unknown";
    }
}

const char *ethernet_manager_get_duplex_str(void)
{
    switch (s_duplex) {
    case ETH_DUPLEX_HALF:
        return "HALF";
    case ETH_DUPLEX_FULL:
        return "FULL";
    default:
        return "Unknown";
    }
}

bool ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (s_eth_netif == NULL || ip_info == NULL) {
        return false;
    }
    return esp_netif_get_ip_info(s_eth_netif, ip_info) == ESP_OK;
}

void *ethernet_manager_get_eth_handle(void)
{
    return s_eth_handle;
}

esp_netif_t *ethernet_manager_get_netif(void)
{
    return s_eth_netif;
}
