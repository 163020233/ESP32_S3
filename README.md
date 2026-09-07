# ESP32-S3 + W5500 AirNode / 以太网验证程序

基于 **ESP32-S3 + W5500 + ESP-IDF v6.2**：

- **AirNode 业务应用**（自 STM32F407 AirNode 移植）：TCP JSON Server + UART 配置工具 + 舵机 PWM + NVS 参数存储，与 Android App / PC 配置工具（`thrower_config.py`）兼容。
- **以太网验证工具**：验证 W5500 稳定的 100M 链路、TCP/UDP 双向通信、数据完整性、压力测试、长稳与断网恢复（可通过 Kconfig 裁剪）。

> 验证工具详细需求见 方案需求/ESP32-S3 + W5500 以太网通信验证程序实现需求.md

## 硬件连接

| 信号 | ESP32-S3 GPIO | 用途 |
|---|---:|---|
| W55_RSTn  | GPIO4 | W5500 复位 |
| W55_INTN  | GPIO5 | W5500 中断（预留） |
| W55_MOSI  | GPIO6 | SPI MOSI |
| W55_MISO  | GPIO7 | SPI MISO |
| W55_SCLK  | GPIO15 | SPI SCLK |
| W55_SCSN  | GPIO16 | SPI CS |
| UART1_TX  | GPIO17 | PC 配置工具（USB-TTL），115200 |
| UART1_RX  | GPIO18 | PC 配置工具 |
| SERVO_CH0 | GPIO8 | 舵机 1（LEDC PWM 50Hz） |
| SERVO_CH1 | GPIO9 | 舵机 2 |

> 引脚为当前按 ESP32-S3 开发板预定的默认值（`main/airnode_config.h` 中集中定义），
> **后续以最终原理图为准**调整。

## 软件架构

```
components/                  # 手动放置的 W5500 驱动组件（注册表托管组件）
├── esp_eth_driver_w5500/   # W5500 芯片驱动 (v2.x)
└── wiznet_common/          # WIZnet 公共驱动基座

main/
├── CMakeLists.txt / Kconfig.projbuild   # 构建 + 功能裁剪开关
├── main.c                  # 启动编排（NVS -> W5500 -> Ethernet -> AirNode/测试）
├── test_config.h           # W5500 GPIO / 测试参数 / IP 默认值
├── w5500_port.c/h          # GPIO/复位/SPI/VERSIONR 验证/MAC-PHY 创建
├── ethernet_manager.c/h    # esp_netif/静态IP(网络参数)/事件/Link/重启
├── net_config.c/h          # 网络参数 NVS 存储（改网段免重烧）
│
│   ── AirNode 业务（生产） ──
├── airnode_config.h        # AirNode 参数集中配置
├── airnode_app.c/h         # TCP:13550 + UART 配置 + 路由任务
├── airnode_json.c/h        # 轻量 JSON key-value 解析
├── config_store.c/h        # 通道闭合/投掷 PWM 的 NVS 存储
├── servo_controller.c/h    # 双路舵机 LEDC PWM + 状态查询
│
│   ── 以太网验证工具（可裁剪） ──
├── console_menu.c/h        # 串口测试菜单（从原 main.c 抽取）
├── network_monitor.c/h     # 统计/监控/长稳
├── tcp_test.c/h            # TCP Server 回显 (5000)
├── udp_test.c/h            # UDP Server (5001) 完整性检测
└── stress_test.c/h         # UDP 压力测试
tools/
├── pc_udp_test.py          # PC 端联调工具（UDP/TCP 测试）
└── (AirNode 配置工具 thrower_config.py 位于 STM32 工程 tools/，协议兼容，可直接使用)
```

### 构建开关（menuconfig）

| 选项 | 默认 | 说明 |
|---|---|---|
| `CONFIG_AIRNODE_APP_ENABLE` | y | AirNode 业务（TCP:13550 / UART1 配置 / 舵机 / NVS） |
| `CONFIG_AIRNODE_TEST_TOOLS_ENABLE` | y | 以太网验证工具（菜单 / 5000 回显 / 5001 UDP / 压力 / 长稳） |

**生产版裁剪**：`idf.py menuconfig` → *AirNode 应用配置* → 关闭
`AIRNODE_TEST_TOOLS_ENABLE`。裁剪后验证工具源码整体不参与编译，
固件只保留以太网底座 + AirNode 业务（不暴露测试端口、无串口菜单）。

```bash
idf.py menuconfig   # 手动裁剪，或直接把默认写进 sdkconfig.defaults
idf.py build
idf.py -p COMx flash monitor
```

## 网络配置

默认静态 IP **192.168.144.20 / 255.255.255.0 / gw 192.168.144.1**
（与 STM32F407 AirNode 同网段，Android App 与 PC 工具设置无需改动）。

- 首次上电自动将默认值写入 **NVS**，之后实际生效值以 NVS 为准。
- 改网段/网关**无需重新烧录**：向 TCP:13550 或 UART1 发
  `{"command":"net_set","ip":"192.168.144.20","mask":"255.255.255.0","gw":"192.168.144.1","cseq":"1"}\r\n\r\n`
  保存后即时生效（已连接客户端会断开重连）。
- 查询：`{"command":"net_get","cseq":"1"}\r\n\r\n`

## AirNode 业务协议（与 STM32 版兼容）

消息以 `\r\n\r\n` 分帧，UTF-8 JSON。应答同时回写 TCP 与 UART1。

| 指令 | 请求示例 | 应答 command |
|---|---|---|
| 读通道配置 | `{"command":"config_read","ch":0,"cseq":"1"}` | `config_read_response`（ch/closed/released） |
| 写通道配置 | `{"command":"config_write","ch":0,"closed":1500,"released":2000,"cseq":"1"}` | `config_write`（回显 closed/released） |
| 抛投触发 | `{"command":"servo_trigger","ch":0,"action":"release","cseq":"1"}` | `servo_trigger` |
| 角度设置 | `{"command":"servo_set","ch":0,"angle":90,"cseq":"1"}` | `servo_set` |
| 角度查询 | `{"command":"servo_get","ch":0,"cseq":"1"}` | `servo_get`（真实角度，修正原版硬编码 90°） |
| 状态查询 | `{"command":"servo_query","ch":0,"cseq":"1"}` | `servo_status`（angle/pulse） |
| 网络参数 | `net_get` / `net_set` | `net_get` / `net_set` |

端口：**TCP 13550**（Android App 依赖，保持不变）。
舵机 PWM：50Hz，500~2500us；上电默认驱动到每通道 closed_pwm（闭合位置）。

## 以太网验证工具（串口菜单）

```
1. Network Status        5. UDP Stress Test
2. Ping/Network Test     6. Show Statistics
3. TCP Test (5000)       7. Long-Time Test
4. UDP Test (5001)       8. Ethernet Restart
```

- **1 网络状态**：Link / Speed / Duplex / IP
- **2 Ping**：默认 ping PC (192.168.144.100)
- **3/4 TCP/UDP**：启停回显/完整性 Server
- **5 压力测试**：包大小/包数/方向/目标
- **7 长稳测试**：1h / 8h / 24h

## 启动流程（串口输出）

```
========================================
 ESP32-S3 + W5500 AirNode / Eth Test
========================================
[W5500] Reset...
[W5500] VERSIONR = 0x04
[W5500] SPI TEST PASS
[ETH] Link UP, Speed: 100 Mbps, Duplex: FULL
[ETH] IP: 192.168.144.20
[ETH] NETWORK READY
[AIRNODE] AirNode app started: TCP :13550, UART1 @115200, 2 servo channels
```

## 联调测试（PC 端）

PC 与 ESP32-S3 直连（或经交换机），PC IP 设为 192.168.144.100/24。

```bash
python tools/pc_udp_test.py send --ip 192.168.144.20 --count 100000 --size 1024
python tools/pc_udp_test.py bidir --ip 192.168.144.20 --count 100000 --size 1024
python tools/pc_udp_test.py tcp --ip 192.168.144.20 --port 5000 --rounds 1000
ping 192.168.144.20
```

AirNode 配置工具：将 `thrower_config.py`（STM32 工程 tools/）用 USB-TTL 接到
**UART1 (GPIO17/18)** 即可读写通道配置/触发抛投，协议不变。

## 已知限制

1. **SPI 是吞吐瓶颈**：W5500 的以太网口是 100M，但主机接口是 SPI。
   当前默认 SPI 40 MHz，实际吞吐约 30~40 Mbps。
2. **UDP 不保证可靠传输**：统计仅用于*检测*丢包/乱序/重复/错误；
   需要可靠交付时请使用 TCP 或 UDP+ACK。
3. **INT (GPIO5) 暂未接入驱动**：已上拉并保留接口，可扩展中断驱动帧接收。
4. AirNode TCP Server 为单客户端会话模式（与 STM32 版一致）：新客户端连接时
   旧客户端被替换；无客户端时网络应答丢弃、UART 应答始终回写。
5. 工程名沿用 hello_world（兼容既有 CLion 配置），可自行重命名。

## 验收对照

| 项目 | 状态 |
|---|---|
| W5500 SPI / VERSIONR=0x04 | 启动自动验证 |
| Ethernet Link / Speed / Duplex | 启动 + 事件 |
| Ping | 菜单项 2 |
| TCP (5000) 回显 | 默认开启 |
| UDP (5001) 完整性检测 | 默认开启 |
| UDP 压力 / 长稳 / 断网恢复 | 菜单项 5/7，事件自动恢复 |
| AirNode TCP:13550 JSON 路由 | AirNode 应用默认开启 |
| AirNode UART1 配置口 | 默认开启（GPIO17/18, 115200） |
| 通道 PWM NVS 保存 | config_write 自动落盘 |
| 网络参数 NVS / 免重烧改网段 | net_set 自动落盘并即时生效 |
| 生产版裁剪 | menuconfig 关闭 AIRNODE_TEST_TOOLS_ENABLE |
