/*
 * servo_controller.c - 舵机 LEDC PWM 控制器
 *
 * ESP32-S3 LEDC 的 PWM 分辨率：AIRNODE_LEDC_DUTY_RES
 * 50Hz 周期 20ms，利用 14bit duty 计算脉宽：
 *   duty = pulse_us * (2^14) / 20000
 *
 * 状态记录：每通道保存最后一次成功下发的脉宽/角度，
 * 供 servo_get / servo_query 查询真实输出状态。
 */
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "airnode_config.h"
#include "servo_controller.h"

static const char *TAG = "SERVO";

static const ledc_channel_t s_servo_ledc_channel[AIRNODE_CHANNEL_COUNT] = {
    AIRNODE_LEDC_CH0,
    AIRNODE_LEDC_CH1,
};

static const int s_servo_gpio[AIRNODE_CHANNEL_COUNT] = {
    AIRNODE_SERVO_GPIO_CH0,
    AIRNODE_SERVO_GPIO_CH1,
};

static bool s_initialized = false;

/* 每通道最后输出状态 */
static uint16_t s_last_pulse_us[AIRNODE_CHANNEL_COUNT];
static int      s_last_angle[AIRNODE_CHANNEL_COUNT];   /* 0~180，由角度指令直接记录 */

static uint32_t pulse_us_to_duty(uint16_t pulse_us)
{
    return ((uint32_t)pulse_us * (1U << AIRNODE_LEDC_DUTY_RES)) / 20000U;
}

/* 脉宽反算角度（500~2500us -> 0~180°），四舍五入 */
static int pulse_us_to_angle(uint16_t pulse_us)
{
    if (pulse_us <= AIRNODE_SERVO_PWM_MIN_US) {
        return AIRNODE_SERVO_MIN_ANGLE;
    }
    if (pulse_us >= AIRNODE_SERVO_PWM_MAX_US) {
        return AIRNODE_SERVO_MAX_ANGLE;
    }
    int a = ((int)pulse_us - AIRNODE_SERVO_PWM_MIN_US) * 180 +
            (AIRNODE_SERVO_PWM_MAX_US - AIRNODE_SERVO_PWM_MIN_US) / 2;
    a /= (AIRNODE_SERVO_PWM_MAX_US - AIRNODE_SERVO_PWM_MIN_US);
    return a;
}

static esp_err_t set_pulse(uint8_t ch, uint16_t pulse_us)
{
    if (ch >= AIRNODE_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pulse_us < AIRNODE_SERVO_PWM_MIN_US) {
        pulse_us = AIRNODE_SERVO_PWM_MIN_US;
    }
    if (pulse_us > AIRNODE_SERVO_PWM_MAX_US) {
        pulse_us = AIRNODE_SERVO_PWM_MAX_US;
    }

    uint32_t duty = pulse_us_to_duty(pulse_us);
    ledc_channel_t channel = s_servo_ledc_channel[ch];

    esp_err_t err = ledc_set_duty(AIRNODE_LEDC_MODE, channel, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(AIRNODE_LEDC_MODE, channel);
    }
    if (err == ESP_OK) {
        s_last_pulse_us[ch] = pulse_us;
        /* 脉宽路径改变了输出：清掉旧的"角度指令"记录，
         * 让 servo_get/servo_status 回真实（由脉宽反算）状态 */
        s_last_angle[ch] = -1;
    }

    ESP_LOGD(TAG, "CH%u PWM set to %u us (duty=%lu)", ch, pulse_us,
             (unsigned long)duty);
    return err;
}

esp_err_t servo_controller_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = AIRNODE_LEDC_MODE,
        .duty_resolution = AIRNODE_LEDC_DUTY_RES,
        .timer_num       = AIRNODE_LEDC_TIMER,
        .freq_hz         = AIRNODE_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < AIRNODE_CHANNEL_COUNT; i++) {
        ledc_channel_config_t ch_cfg = {
            .gpio_num   = s_servo_gpio[i],
            .speed_mode = AIRNODE_LEDC_MODE,
            .channel    = s_servo_ledc_channel[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = AIRNODE_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
            .flags.output_invert = 0,
        };
        err = ledc_channel_config(&ch_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ledc_channel_config CH%d failed: %s",
                     i, esp_err_to_name(err));
            return err;
        }
        s_last_pulse_us[i] = 0;
        s_last_angle[i] = -1;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Servo PWM initialized (%d channels, 50Hz)", AIRNODE_CHANNEL_COUNT);
    return ESP_OK;
}

esp_err_t servo_controller_set_angle(uint8_t ch, int angle)
{
    if (ch >= AIRNODE_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (angle < AIRNODE_SERVO_MIN_ANGLE) {
        angle = AIRNODE_SERVO_MIN_ANGLE;
    }
    if (angle > AIRNODE_SERVO_MAX_ANGLE) {
        angle = AIRNODE_SERVO_MAX_ANGLE;
    }

    uint16_t pulse_us = (uint16_t)(AIRNODE_SERVO_PWM_MIN_US +
        (uint32_t)(AIRNODE_SERVO_PWM_MAX_US - AIRNODE_SERVO_PWM_MIN_US) *
        (uint32_t)angle / (AIRNODE_SERVO_MAX_ANGLE - AIRNODE_SERVO_MIN_ANGLE));

    esp_err_t err = set_pulse(ch, pulse_us);
    if (err == ESP_OK) {
        /* 角度指令记录角度值（查询时优先返回该值） */
        s_last_angle[ch] = angle;
    }
    return err;
}

esp_err_t servo_controller_set_pulse_us(uint8_t ch, uint16_t pulse_us)
{
    if (ch >= AIRNODE_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_pulse(ch, pulse_us);
}

esp_err_t servo_controller_get_pulse_us(uint8_t ch, uint16_t *pulse_us)
{
    if (ch >= AIRNODE_CHANNEL_COUNT || pulse_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    *pulse_us = s_last_pulse_us[ch];
    return ESP_OK;
}

esp_err_t servo_controller_get_angle(uint8_t ch, int *angle)
{
    if (ch >= AIRNODE_CHANNEL_COUNT || angle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_last_angle[ch] >= 0) {
        *angle = s_last_angle[ch];
    } else {
        *angle = pulse_us_to_angle(s_last_pulse_us[ch]);
    }
    return ESP_OK;
}
