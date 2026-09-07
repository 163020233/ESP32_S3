# AirNode（生产业务）源码：始终编译
set(AIRNODE_SRCS
    "main.c"
    "w5500_port.c"
    "ethernet_manager.c"
    "airnode_app.c"
    "airnode_json.c"
    "config_store.c"
    "servo_controller.c"
    "net_config.c"
)

set(AIRNODE_REQUIRES
    esp_eth
    esp_eth_driver_w5500
    esp_driver_spi
    esp_driver_gpio
    esp_driver_uart
    esp_driver_ledc
    esp_driver_usb_serial_jtag
    esp_netif
    esp_event
    esp_timer
    nvs_flash
    lwip
)

# 以太网验证工具：生产版通过 menuconfig 关闭 AIRNODE_TEST_TOOLS_ENABLE
# 裁剪（对应源码整体不编译，端口 5000/5001/菜单不再暴露）
if(CONFIG_AIRNODE_TEST_TOOLS_ENABLE)
    list(APPEND AIRNODE_SRCS
        "network_monitor.c"
        "tcp_test.c"
        "udp_test.c"
        "stress_test.c"
        "console_menu.c")
    list(APPEND AIRNODE_REQUIRES ping)
endif()

idf_component_register(
    SRCS ${AIRNODE_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${AIRNODE_REQUIRES}
)
