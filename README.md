# ESP32-S3 + W5500 以太网通信验证程序

基于 **ESP32-S3 + W5500 + ESP-IDF v6.2** 的以太网通信验证程序。
目的：验证 ESP32-S3 通过 W5500 建立稳定的 100M 以太网链路，并验证
TCP/UDP 双向通信、数据完整性、压力测试、长时间稳定性以及断网恢复能力。

> 详细需求见 方案需求/ESP32-S3 + W5500 以太网通信验证程序实现需求.md

## 硬件连接

| W5500 信号 | ESP32-S3 GPIO |
|---|---:|
| W55_RSTn  | GPIO4 |
| W55_INTN  | GPIO5 |
| W55_MOSI  | GPIO6 |
| W55_MISO  | GPIO7 |
| W55_SCLK  | GPIO15 |
| W55_SCSN  | GPIO16 |

## 软件架构

```
components/                  # 手动放置的 W5500 驱动组件（注册表托管组件）
├── esp_eth_driver_w5500/   # W5500 芯片驱动 (v2.x)
└── wiznet_common/          # WIZnet 公共驱动基座（esp_eth_driver_w5500 的依赖）
main/
├── CMakeLists.txt
├── idf_component.yml       # 无注册表依赖，构建不联网
├── main.c                  # 系统启动 + 串口菜单
├── test_config.h          # 全部参数集中配置
├── w5500_port.c/h         # GPIO/复位/SPI/VERSIONR 验证/MAC-PHY 创建
├── ethernet_manager.c/h   # esp_netif/静态IP/事件/Link 检测/重启
├── network_monitor.c/h    # 统计结构/监控任务/长稳测试
├── tcp_test.c/h           # TCP Server (5000)
├── udp_test.c/h           # UDP Server (5001) + 序列/CRC 完整性检测
└── stress_test.c/h        # UDP 压力测试 + 统计报告
tools/
└── pc_udp_test.py         # PC 端联调工具（UDP/TCP 测试）
```

## 构建与烧录

**W5500 驱动组件已手动安装到 components/ 目录**（ESP-IDF v6.x 已移除 W5500 芯片驱动，
需从 Component Registry 下载后解压放入）：

```
components/esp_eth_driver_w5500/   # 来自 espressif/esp_eth_driver_w5500
components/wiznet_common/          # 来自 espressif/wiznet_common（驱动依赖）
```

下载地址：
- https://components.espressif.com/components/espressif/esp_eth_driver_w5500
- https://components.espressif.com/components/espressif/wiznet_common

构建（无需联网）：

```bash
idf.py set-target esp32s3   # 首次（若 sdkconfig 未生成）
idf.py build
idf.py -p COMx flash monitor
```

> 注意：本项目启用 MINIMAL_BUILD（裁剪构建到依赖组件），因为本机 ESP-IDF
> 检出未拉取 unity 子模块；如需关闭，请先 `git submodule update --init`。
>
> CLion：删除旧的 build/ 与 sdkconfig 后重新加载工程（目标从 esp32 改为 esp32s3）。

## 启动流程（串口输出）

```
========================================
 ESP32-S3 + W5500 Ethernet Test v1.0
========================================
[W5500] Reset...
[W5500] SPI Init...
[W5500] VERSIONR = 0x04
[W5500] SHAR write/read: ... -> OK
[W5500] SPI TEST PASS
[ETH] Link UP
[ETH] Speed: 100 Mbps
[ETH] Duplex: FULL
[ETH] IP: 192.168.1.20
[ETH] NETWORK READY
```

## 串口菜单

```
================================
 ESP32-S3 W5500 Ethernet Test
================================

1. Network Status
2. Ping/Network Test
3. TCP Test
4. UDP Test
5. UDP Stress Test
6. Show Statistics
7. Start Long-Time Test
8. Ethernet Restart

Select:
```

- **1 网络状态**：Link / Speed / Duplex / IP
- **2 Ping**：默认 ping PC (192.168.1.100)
- **3 TCP**：开关 TCP Server（端口 5000，默认启动，回显模式）
- **4 UDP**：开关 UDP Server（端口 5001，默认启动，回显 + 完整性检测）
- **5 压力测试**：交互选择包大小(64/256/512/1024/1400)、包数、方向(RX/TX/BIDIR)、目标 IP/端口
- **6 统计**：打印完整统计
- **7 长稳测试**：1h / 8h / 24h 或自定义秒数
- **8 以太网重启**：esp_eth stop + start

## 联调测试（PC 端）

PC 与 ESP32-S3 直连（或经交换机），PC IP 设为 192.168.1.100/24。

```bash
# UDP 压力发送 1M 包（1024 字节），观察 ESP32 侧统计
python tools/pc_udp_test.py send --ip 192.168.1.20 --count 1000000 --size 1024

# 双向：PC 发送 + 接收 ESP32 回显统计
python tools/pc_udp_test.py bidir --ip 192.168.1.20 --count 100000 --size 1024

# TCP 回显
python tools/pc_udp_test.py tcp --ip 192.168.1.20 --port 5000 --rounds 1000

# 基础连通
ping 192.168.1.20
```

PC 工具与固件使用相同的测试包格式（magic + sequence + timestamp + payload_len +
payload + CRC16-CCITT），PC 侧同样统计丢包/重复/乱序/CRC 错误。

## 已知限制

1. **SPI 是吞吐瓶颈**：W5500 的以太网口是 100M，但主机接口是 SPI。
   当前默认 SPI 40 MHz，实际吞吐约 30~40 Mbps；若需更高吞吐可提高 SPI 频率
   （修改 test_config.h 中 W5500_SPI_CLK_HZ），若出现不稳定可降回 10 MHz。
2. **UDP 不保证可靠传输**：本程序的统计（Sequence + CRC）用于*检测*丢包/乱序/
   重复/数据错误，不代表 W5500 承诺不丢包。需要可靠交付时请使用 TCP 或
   UDP + ACK/重传机制。
3. **第一阶段不依赖 INT (GPIO5)**：GPIO5 已配置为上拉输入并保留接口，
   后续可扩展为中断驱动的帧接收通知。
4. 本项目工程名沿用 hello_world（为兼容既有 CLion 配置），可自行重命名。

## 验收对照

| 项目 | 状态 |
|---|---|
| W5500 SPI / VERSIONR=0x04 | 启动自动验证 |
| Ethernet Link / Speed / Duplex | 启动 + 事件 |
| Ping | 菜单项 2 |
| TCP (连接/回显/断开/重连) | 默认开启 |
| UDP (5001) 完整性检测 | 默认开启 |
| UDP 压力 (64/256/512/1024/1400B) | 菜单项 5 |
| 长稳 (1h/8h/24h) | 菜单项 7 |
| 断网恢复 (Link Down/Up) | 事件驱动，自动恢复 |
| RTL8305 环境 | 接入交换机后重跑以上测试 |
