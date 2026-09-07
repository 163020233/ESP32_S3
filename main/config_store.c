/*
 * config_store.c - AirNode 参数存储
 *
 * NVS key/value 设计：
 *   key   = "channels"
 *   value = airnode_channel_cfg_t[AIRNODE_CHANNEL_COUNT]
 */
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "airnode_config.h"
#include "config_store.h"

static const char *TAG = "CFG";

static airnode_channel_cfg_t s_configs[AIRNODE_CHANNEL_COUNT] = {
    { AIRNODE_DEFAULT_CLOSED_PWM, AIRNODE_DEFAULT_RELEASED_PWM },
    { AIRNODE_DEFAULT_CLOSED_PWM, AIRNODE_DEFAULT_RELEASED_PWM },
};

static SemaphoreHandle_t s_mutex = NULL;
static bool s_loaded = false;

static void load_defaults(void)
{
    for (int i = 0; i < AIRNODE_CHANNEL_COUNT; i++) {
        s_configs[i].closed_pwm   = AIRNODE_DEFAULT_CLOSED_PWM;
        s_configs[i].released_pwm = AIRNODE_DEFAULT_RELEASED_PWM;
    }
}

static esp_err_t write_all(void)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRNODE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, AIRNODE_NVS_KEY,
                       s_configs, sizeof(s_configs));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_init(void)
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

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIRNODE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_configs);
        err = nvs_get_blob(handle, AIRNODE_NVS_KEY, s_configs, &len);
        nvs_close(handle);

        if (err == ESP_OK && len == sizeof(s_configs)) {
            s_loaded = true;
        } else {
            err = ESP_FAIL;
        }
    }

    if (!s_loaded) {
        ESP_LOGW(TAG, "NVS config not found/valid, loading defaults");
        load_defaults();
        s_loaded = true;
        err = write_all();
    }

    xSemaphoreGive(s_mutex);
    return err;
}

const airnode_channel_cfg_t *config_store_get(uint8_t ch)
{
    if (ch >= AIRNODE_CHANNEL_COUNT) {
        return NULL;
    }
    if (!s_loaded) {
        config_store_init();
    }
    return &s_configs[ch];
}

esp_err_t config_store_set(uint8_t ch, uint16_t closed_pwm, uint16_t released_pwm)
{
    if (ch >= AIRNODE_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        esp_err_t err = config_store_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_configs[ch].closed_pwm   = closed_pwm;
    s_configs[ch].released_pwm = released_pwm;
    esp_err_t err = write_all();

    xSemaphoreGive(s_mutex);
    return err;
}
