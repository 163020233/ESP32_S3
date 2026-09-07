/*
 * w5500_port.c - W5500 硬件端口层
 *
 * 实现阶段一（SPI/W5500 硬件验证）与 MAC/PHY 实例创建。
 *
 * W5500 SPI 帧格式（见 W5500 Datasheet 4.1.2）：
 *   [CMD(1B)] [ADDR[7:0](1B)] [DATA...]
 *   CMD 位7 = RWB（1 写 0 读），位6:5 = VDM（00 通用寄存器），位4:0 = ADDR[12:8]
 *   读：CMD, ADDR, 1 个 dummy 字节, 数据字节
 *   写：CMD, ADDR, 数据字节
 *
 * 驱动：espressif/esp_eth_driver_w5500 v2.x（手动置于 components/）
 *   - 阶段一测试使用本文件自建的临时 SPI 设备
 *   - MAC 实例由组件通过 ETH_W5500_DEFAULT_CONFIG(host, &spi_devcfg) 创建，
 *     组件内部自行 spi_bus_add_device（因此阶段一测试后要移除临时设备）
 *   - 支持 INT 引脚（GPIO5）中断驱动，通过 W5500_INT_ENABLE 控制
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_check.h"

#include "test_config.h"
#include "w5500_port.h"

/* esp_eth_driver_w5500 组件提供的芯片驱动头文件 */
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

static const char *TAG = "W5500";

/* W5500 寄存器地址（与驱动 w5500.h 中的映射一致） */
#define W5500_REG_MR           0x0000u  /* 模式寄存器 */
#define W5500_REG_SHAR         0x0009u  /* 源 MAC 地址（6 字节，可读写） */
#define W5500_REG_VERSIONR     0x0039u  /* 版本寄存器（只读，应为 0x04） */

#define W5500_SHAR_LEN         6

static spi_device_handle_t s_test_spi = NULL;   /* 阶段一测试用临时设备 */

/* ------------------------------------------------------------------ */
/* 底层 SPI 读写（阶段一测试用，通用寄存器块 VDM=00）                    */
/* ------------------------------------------------------------------ */

/*
 * W5500 SPI 帧格式（24 位头，与官方驱动 wiznet_spi.c 一致）：
 *   [寄存器偏移(16bit, MSB 在前)] [控制字节(8bit)] [数据(8bit*n)]
 *   控制字节 = BSB(3bit)<<3 | RWB(1bit)<<2
 *   通用寄存器块 BSB=0：读=0x00，写=0x04
 * 读：偏移 + 控制 + 1 个空闲字节 + 数据（数据出现在 rx[3] 起）
 * 写：偏移 + 控制 + 数据
 */
static esp_err_t w5500_reg_read(uint16_t reg, uint8_t *buf, uint32_t len)
{
    if (len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t tx[3 + 32] = { 0 };
    uint8_t rx[3 + 32];
    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFF);
    tx[2] = 0x00;                           /* BSB=0, RWB=0 (read) */
    spi_transaction_t t = {
        .tx_buffer = tx,
        .rx_buffer = rx,
        .length    = (3 + len) * 8,
        .rxlength  = (3 + len) * 8,
    };
    esp_err_t err = spi_device_polling_transmit(s_test_spi, &t);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(buf, &rx[3], len);
    return ESP_OK;
}

static esp_err_t w5500_reg_write(uint16_t reg, const uint8_t *buf, uint32_t len)
{
    if (len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t tx[3 + 32];
    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFF);
    tx[2] = 0x04;                           /* BSB=0, RWB=1 (write) */
    memcpy(&tx[3], buf, len);
    spi_transaction_t t = {
        .tx_buffer = tx,
        .length    = (3 + len) * 8,
    };
    return spi_device_polling_transmit(s_test_spi, &t);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t w5500_gpio_init(void)
{
    /* RST: 输出 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << W5500_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "RST gpio config failed");
    gpio_set_level(W5500_RST_GPIO, 1);

    /* INT: 输入上拉（若使能中断模式，驱动会自行重新配置） */
    gpio_config_t int_io = {
        .pin_bit_mask = (1ULL << W5500_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_io), TAG, "INT gpio config failed");

#if W5500_INT_ENABLE
    /* 驱动使用 gpio_isr_handler_add()，需先安装 GPIO ISR 服务；
     * 若已由其它模块安装，忽略 ESP_ERR_INVALID_STATE。 */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
        return isr_err;
    }
#endif
    return ESP_OK;
}

static esp_err_t w5500_reset(void)
{
    ESP_LOGI(TAG, "Reset...");
    gpio_set_level(W5500_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(W5500_RST_ASSERT_MS));
    gpio_set_level(W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(W5500_RST_RELEASE_DELAY_MS));
    ESP_LOGI(TAG, "Reset done");
    return ESP_OK;
}

static esp_err_t w5500_spi_bus_init(void)
{
    ESP_LOGI(TAG, "SPI Init...");
    spi_bus_config_t buscfg = {
        .miso_io_num = W5500_MISO_GPIO,
        .mosi_io_num = W5500_MOSI_GPIO,
        .sclk_io_num = W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* 阶段一测试用临时 SPI 设备（验证完成后移除，MAC 由组件自行创建设备） */
    spi_device_interface_config_t devcfg = {
        .mode = W5500_SPI_MODE,
        .clock_speed_hz = W5500_SPI_CLK_HZ,
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 4,
        .flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(W5500_SPI_HOST, &devcfg, &s_test_spi),
                        TAG, "SPI add device failed");
    ESP_LOGI(TAG, "SPI Init done (host=%d, clk=%d Hz)", W5500_SPI_HOST, W5500_SPI_CLK_HZ);
    return ESP_OK;
}

/* 阶段一：SPI / W5500 硬件验证 */
static esp_err_t w5500_spi_test(w5500_spi_test_result_t *result)
{
    esp_err_t err;

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    /* 1) VERSIONR 读取 */
    uint8_t ver = 0;
    err = w5500_reg_read(W5500_REG_VERSIONR, &ver, 1);
    ESP_RETURN_ON_ERROR(err, TAG, "read VERSIONR failed");
    ESP_LOGI(TAG, "VERSIONR = 0x%02X", ver);
    if (result) {
        result->versionr = ver;
        result->version_ok = (ver == 0x04);
    }
    if (ver != 0x04) {
        ESP_LOGE(TAG, "VERSIONR mismatch (expect 0x04), W5500 may be absent or SPI wiring wrong");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2) 模式寄存器读取（正常应为 0x00） */
    uint8_t mr = 0;
    err = w5500_reg_read(W5500_REG_MR, &mr, 1);
    ESP_RETURN_ON_ERROR(err, TAG, "read MR failed");
    ESP_LOGI(TAG, "MR = 0x%02X", mr);
    if (result) {
        result->mr = mr;
    }

    /* 3) SHAR 写回验证（双向读写） */
    const uint8_t test_mac[W5500_SHAR_LEN] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };
    uint8_t back[W5500_SHAR_LEN] = { 0 };
    err = w5500_reg_write(W5500_REG_SHAR, test_mac, W5500_SHAR_LEN);
    ESP_RETURN_ON_ERROR(err, TAG, "write SHAR failed");
    err = w5500_reg_read(W5500_REG_SHAR, back, W5500_SHAR_LEN);
    ESP_RETURN_ON_ERROR(err, TAG, "read back SHAR failed");
    bool ok = (memcmp(test_mac, back, W5500_SHAR_LEN) == 0);
    ESP_LOGI(TAG, "SHAR write/read: %02X:%02X:%02X:%02X:%02X:%02X -> %s",
             back[0], back[1], back[2], back[3], back[4], back[5],
             ok ? "OK" : "MISMATCH");
    if (result) {
        memcpy(result->shar, back, W5500_SHAR_LEN);
        result->rw_test_ok = ok;
    }
    if (!ok) {
        ESP_LOGE(TAG, "SPI bidirectional r/w test FAILED");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPI TEST PASS");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t w5500_port_init(w5500_spi_test_result_t *result)
{
    ESP_RETURN_ON_ERROR(w5500_gpio_init(), TAG, "GPIO init failed");
    ESP_RETURN_ON_ERROR(w5500_reset(), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(w5500_spi_bus_init(), TAG, "SPI init failed");
    ESP_RETURN_ON_ERROR(w5500_spi_test(result), TAG, "SPI test failed");

    /* 阶段一测试完成：移除临时 SPI 设备（MAC 由组件自行创建） */
    if (s_test_spi != NULL) {
        spi_bus_remove_device(s_test_spi);
        s_test_spi = NULL;
    }
    return ESP_OK;
}

esp_err_t w5500_port_create_mac_phy(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out)
{
    if (mac_out == NULL || phy_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* W5500 Ethernet MAC（组件 v2.x API）
     * 组件内部根据 spi_host_id + spi_devcfg 自行创建 SPI 设备。 */
    spi_device_interface_config_t spi_devcfg = {
        .mode = W5500_SPI_MODE,
        .clock_speed_hz = W5500_SPI_CLK_HZ,
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 8,
        .flags = 0,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &spi_devcfg);
#if W5500_INT_ENABLE
    w5500_config.base.int_gpio_num = W5500_INT_GPIO;   /* 使用 INT 引脚中断驱动 */
    w5500_config.base.poll_period_ms = 0;              /* 中断模式不使用轮询定时器 */
#else
    w5500_config.base.int_gpio_num = -1;               /* 轮询模式 */
    w5500_config.base.poll_period_ms = 10;             /* 轮询周期 10ms（0 会导致定时器异常） */
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "create W5500 MAC failed");
        return ESP_FAIL;
    }

    /* W5500 内部集成 PHY */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_config.reset_gpio_num = W5500_RST_GPIO;
    phy_config.hw_reset_assert_time_us = 1000;      /* W5500 要求 RSTn 低电平 >= 500us */
    phy_config.post_hw_reset_delay_ms = 10;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "create W5500 PHY failed");
        if (mac->del) {
            mac->del(mac);
        }
        return ESP_FAIL;
    }

    *mac_out = mac;
    *phy_out = phy;
    ESP_LOGI(TAG, "W5500 MAC/PHY created (INT=%s)",
             W5500_INT_ENABLE ? "enabled" : "polling");
    return ESP_OK;
}

int w5500_port_get_int_level(void)
{
    return gpio_get_level(W5500_INT_GPIO);
}
