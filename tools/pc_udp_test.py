#!/usr/bin/env python3
"""
PC 端测试工具 - ESP32-S3 + W5500 以太网验证程序配套工具

与固件内的 UDP 测试包格式完全一致（小端序，packed）：
    magic       : uint32 = 0x57355030 ('W5P0')
    sequence    : uint32
    timestamp_ms: uint32
    payload_len : uint16
    payload     : payload_len 字节
    crc         : uint16 (CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF,
                           MSB-first, 不反转, 无异或; 校验范围 magic..payload)

用法示例：
    # 1) UDP 发送压力测试：向 ESP32 发 100000 个 1024 字节包
    python pc_udp_test.py send --ip 192.168.1.20 --port 5001 --count 100000 --size 1024

    # 2) UDP 接收（接收 ESP32 回显/ESP32 主动发送的测试包并统计）
    python pc_udp_test.py recv --port 5001 --count 100000 --timeout 120

    # 3) 双向：本机发 count 个，同时统计 ESP32 回显回来的包
    python pc_udp_test.py bidir --ip 192.168.1.20 --port 5001 --count 100000 --size 1024

    # 4) TCP 回显测试
    python pc_udp_test.py tcp --ip 192.168.1.20 --port 5000 --rounds 1000

    # 5) 简单文本 UDP 消息
    python pc_udp_test.py text --ip 192.168.1.20 --port 5001 --msg "hello w5500"
"""
import argparse
import socket
import struct
import sys
import time

MAGIC = 0x57355030
MAX_PAYLOAD = 1400
HEADER = struct.Struct("<IIIH")          # magic, seq, ts, payload_len
CRC_FIELD = struct.Struct("<H")


def crc16_ccitt_false(data: bytes) -> int:
    """CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB-first."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_packet(seq: int, payload_len: int) -> bytes:
    payload = bytes(((i * 7 + 3) & 0xFF) for i in range(payload_len))
    body = HEADER.pack(MAGIC, seq, int(time.time() * 1000) & 0xFFFFFFFF, payload_len) + payload
    crc = crc16_ccitt_false(body)
    return body + CRC_FIELD.pack(crc)


def parse_packet(data: bytes):
    """解析并校验测试包，返回 dict 或 None。"""
    if len(data) < HEADER.size + CRC_FIELD.size:
        return None
    magic, seq, ts, payload_len = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        return None
    need = HEADER.size + payload_len + CRC_FIELD.size
    if len(data) < need:
        return None
    body = data[:HEADER.size + payload_len]
    (crc,) = CRC_FIELD.unpack_from(data, HEADER.size + payload_len)
    if crc16_ccitt_false(body) != crc:
        return {"seq": seq, "crc_ok": False, "len": len(data)}
    return {"seq": seq, "crc_ok": True, "len": len(data)}


class Stats:
    def __init__(self):
        self.rx = 0
        self.tx = 0
        self.lost = 0
        self.dup = 0
        self.ooo = 0
        self.err = 0
        self.last_seq = None
        self.first_seq = None

    def feed(self, pkt):
        self.rx += 1
        if not pkt["crc_ok"]:
            self.err += 1
            return
        seq = pkt["seq"]
        if self.first_seq is None:
            self.first_seq = seq
            self.last_seq = seq
            return
        if seq == self.last_seq + 1:
            pass
        elif seq == self.last_seq:
            self.dup += 1
        elif seq < self.last_seq:
            self.ooo += 1
        else:
            self.lost += (seq - self.last_seq - 1)
        self.last_seq = seq

    def report(self, duration):
        total = self.rx + self.lost
        loss = (self.lost * 100.0 / total) if total else 0.0
        print("========== PC UDP TEST RESULT ==========")
        print(f"Packets RX       : {self.rx:,}")
        print(f"Lost             : {self.lost:,}")
        print(f"Duplicate        : {self.dup:,}")
        print(f"Out of Order     : {self.ooo:,}")
        print(f"Data Error       : {self.err:,}")
        print(f"Loss Rate        : {loss:.6f} %")
        print(f"Duration         : {duration:.3f} seconds")
        print("========================================")


def cmd_send(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    addr = (args.ip, args.port)
    t0 = time.time()
    for i in range(1, args.count + 1):
        sock.sendto(build_packet(i, args.size), addr)
        if i % 10000 == 0:
            print(f"[TX] {i:,} / {args.count:,}")
    dt = time.time() - t0
    total = args.count * (HEADER.size + args.size + CRC_FIELD.size)
    print(f"TX done: {args.count:,} packets, {total/1e6:.3f} MB in {dt:.3f}s, "
          f"{total*8/dt/1e6:.2f} Mbps")
    sock.close()


def cmd_recv(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)
    stats = Stats()
    t0 = time.time()
    deadline = t0 + args.timeout
    while stats.rx < args.count and time.time() < deadline:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            continue
        pkt = parse_packet(data)
        if pkt is None:
            print(f"[INFO] non-test packet ({len(data)} B): {data[:60]!r}")
            continue
        stats.feed(pkt)
        if stats.rx % 10000 == 0:
            print(f"[RX] {stats.rx:,} / {args.count:,}")
    stats.report(time.time() - t0)
    sock.close()


def cmd_bidir(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.bind(("0.0.0.0", args.port))
    addr = (args.ip, args.port)
    stats = Stats()
    t0 = time.time()
    deadline = t0 + args.timeout
    sent = 0
    recv_done = args.count <= 0
    while time.time() < deadline and (sent < args.count or not recv_done):
        # 发送
        if sent < args.count:
            sock.sendto(build_packet(sent + 1, args.size), addr)
            sent += 1
            if sent % 10000 == 0:
                print(f"[TX] {sent:,} / {args.count:,}")
        # 接收（回显）
        while True:
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                break
            pkt = parse_packet(data)
            if pkt is None:
                continue
            stats.feed(pkt)
            if stats.rx % 10000 == 0:
                print(f"[RX] {stats.rx:,}")
            if args.count > 0 and stats.rx >= args.count:
                break
        recv_done = args.count > 0 and stats.rx >= args.count
    stats.report(time.time() - t0)
    print(f"PC TX: {sent:,} packets")
    sock.close()


def cmd_tcp(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(args.timeout)
    sock.connect((args.ip, args.port))
    print(f"TCP connected to {args.ip}:{args.port}")
    ok = 0
    for i in range(1, args.rounds + 1):
        msg = f"tcp-test-{i}-" + ("x" * args.size)
        sock.sendall(msg.encode())
        data = sock.recv(len(msg) + 64)
        if data == msg.encode():
            ok += 1
        else:
            print(f"[WARN] round {i} mismatch: got {len(data)} B")
        if i % 100 == 0:
            print(f"[TCP] {i:,} / {args.rounds:,}")
    print(f"TCP echo test done: {ok}/{args.rounds} OK")
    sock.close()


def cmd_text(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(args.msg.encode(), (args.ip, args.port))
    print(f"Sent text to {args.ip}:{args.port}: {args.msg}")
    sock.close()


def main():
    ap = argparse.ArgumentParser(description="ESP32-S3 W5500 UDP/TCP test tool")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("send", help="UDP 压力发送")
    p.add_argument("--ip", default="192.168.1.20")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--count", type=int, default=100000)
    p.add_argument("--size", type=int, default=1024, choices=[64, 256, 512, 1024, 1400])
    p.set_defaults(func=cmd_send)

    p = sub.add_parser("recv", help="UDP 接收统计")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--count", type=int, default=100000)
    p.add_argument("--timeout", type=int, default=300)
    p.set_defaults(func=cmd_recv)

    p = sub.add_parser("bidir", help="双向（发 + 收回显）")
    p.add_argument("--ip", default="192.168.1.20")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--count", type=int, default=100000)
    p.add_argument("--size", type=int, default=1024, choices=[64, 256, 512, 1024, 1400])
    p.add_argument("--timeout", type=int, default=600)
    p.set_defaults(func=cmd_bidir)

    p = sub.add_parser("tcp", help="TCP 回显测试")
    p.add_argument("--ip", default="192.168.1.20")
    p.add_argument("--port", type=int, default=5000)
    p.add_argument("--rounds", type=int, default=1000)
    p.add_argument("--size", type=int, default=128)
    p.add_argument("--timeout", type=int, default=10)
    p.set_defaults(func=cmd_tcp)

    p = sub.add_parser("text", help="发送文本 UDP 消息")
    p.add_argument("--ip", default="192.168.1.20")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--msg", default="hello w5500")
    p.set_defaults(func=cmd_text)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
