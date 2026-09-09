#!/usr/bin/env python3
"""
AirNode 网络参数读写工具 (net_get / net_set)

支持两种通道：
  1) TCP  : 直连 AirNode 的 TCP:13550（需与设备同网段）
  2) 串口 : USB-TTL 接设备 UART1 配置口（GPIO17/18），115200-8N1

用法示例：
  # 读取当前网络参数
  python airnode_net_set.py get --ip 192.168.144.20
  python airnode_net_set.py get --port COM5

  # 修改并保存（三字段必须齐全；应答后约 200ms 设备切换 IP）
  python airnode_net_set.py set --ip 192.168.144.20 ^
      --new-ip 192.168.144.30 --mask 255.255.255.0 --gw 192.168.144.1
  python airnode_net_set.py set --port COM5 ^
      --new-ip 192.168.144.40 --mask 255.255.255.0 --gw 192.168.144.1

提示：net_set 修改的是 NVS 中保存的网络参数（免重烧），
TCP 通道改 IP 后本机连接会断开，请用新 IP 重连；串口通道不受影响。
"""
import argparse
import json
import sys
import time

MSG_DELIMITER = b"\r\n\r\n"
TCP_PORT_DEFAULT = 13550
BAUD_DEFAULT = 115200


def recv_until_delimiter(read, timeout_s=5.0):
    """read: callable(n)->bytes；读到 \r\n\r\n 或超时后返回已收字节"""
    buf = b""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        chunk = read(256)
        if chunk:
            buf += chunk
            if MSG_DELIMITER in buf:
                break
        else:
            time.sleep(0.02)
    return buf


def cmd_via_tcp(host, tcp_port, payload):
    import socket
    with socket.create_connection((host, tcp_port), timeout=8) as s:
        s.sendall(json.dumps(payload).encode("utf-8") + MSG_DELIMITER)
        return recv_until_delimiter(lambda n: s.recv(n))


def cmd_via_serial(port, baud, payload):
    try:
        import serial
    except ImportError:
        sys.exit("缺少 pyserial，请先安装: pip install pyserial")
    with serial.Serial(port, baud, timeout=1.0) as ser:
        ser.write(json.dumps(payload).encode("utf-8") + MSG_DELIMITER)
        return recv_until_delimiter(lambda n: ser.read(n))


def main():
    ap = argparse.ArgumentParser(
        description="AirNode 网络参数读写 (net_get / net_set，TCP 或串口通道)")
    sub = ap.add_subparsers(dest="action", required=True)

    def add_common(p):
        g = p.add_mutually_exclusive_group(required=True)
        g.add_argument("--ip", metavar="A.B.C.D", help="TCP 通道：设备 IP")
        g.add_argument("--port", metavar="COMn", help="串口通道：如 COM5")
        p.add_argument("--tcp-port", type=int, default=TCP_PORT_DEFAULT,
                       help="TCP 端口（默认 %d）" % TCP_PORT_DEFAULT)
        p.add_argument("--baud", type=int, default=BAUD_DEFAULT,
                       help="串口波特率（默认 %d）" % BAUD_DEFAULT)

    p_get = sub.add_parser("get", help="读取网络参数 (net_get)")
    add_common(p_get)

    p_set = sub.add_parser("set", help="修改并保存网络参数，立即生效 (net_set)")
    add_common(p_set)
    p_set.add_argument("--new-ip", required=True, metavar="A.B.C.D",
                       help="新 IP，如 192.168.144.30")
    p_set.add_argument("--mask", required=True, metavar="A.B.C.D",
                       help="子网掩码，如 255.255.255.0")
    p_set.add_argument("--gw", required=True, metavar="A.B.C.D",
                       help="网关，如 192.168.144.1")

    a = ap.parse_args()

    if a.action == "get":
        payload = {"command": "net_get", "cseq": "1"}
    else:
        payload = {"command": "net_set", "cseq": "1",
                   "ip": a.new_ip, "mask": a.mask, "gw": a.gw}

    try:
        if a.ip:
            print("[TCP] %s:%d -> %s" % (a.ip, a.tcp_port, payload["command"]))
            resp = cmd_via_tcp(a.ip, a.tcp_port, payload)
        else:
            print("[UART] %s @%d -> %s" % (a.port, a.baud, payload["command"]))
            resp = cmd_via_serial(a.port, a.baud, payload)
    except OSError as e:
        sys.exit("通信失败: %s" % e)

    text = resp.decode("utf-8", errors="ignore").strip()
    if not text:
        sys.exit("无应答（超时）。检查连接/IP/端口；"
                 "串口需接设备 UART1 (GPIO17 TX / GPIO18 RX)")
    print("应答:", text)

    if a.action == "set" and '"code":"200"' in text:
        print("已保存并应用。设备约 200ms 后切换 IP：%s" % a.new_ip)
        if a.ip:
            print("（TCP 通道已断开，请用新 IP 重连；串口通道不受影响）")


if __name__ == "__main__":
    main()
