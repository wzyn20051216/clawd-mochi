#!/usr/bin/env python3
"""Clawd Mochi 桌宠桥接器。

用法示例：
    python tools/mochi_bridge.py happy "build ok"
    python tools/mochi_bridge.py thinking "coding..."
    python tools/mochi_bridge.py error "build failed"
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


CONFIG_PATH = Path(__file__).with_name(".mochi_host")
DEFAULT_HOST = "192.168.4.1"
MDNS_HOST = "clawd-mochi.local"
MDNS_GROUP = ("224.0.0.251", 5353)


MOOD_ALIASES = {
    "ok": "happy",
    "done": "happy",
    "success": "happy",
    "fail": "error",
    "failed": "error",
    "angry": "error",
    "think": "thinking",
    "coding": "thinking",
    "warn": "surprise",
}


def configure_stdio() -> None:
    """尽量让 Windows 终端也能正常打印中文提示。"""
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def normalize_mood(mood: str) -> str:
    """把常见英文状态词归一化为固件支持的 mood。"""
    key = mood.strip().lower()
    return MOOD_ALIASES.get(key, key or "normal")


def load_saved_host() -> str | None:
    """读取上次保存的 ESP32 地址。"""
    if not CONFIG_PATH.exists():
        return None
    host = CONFIG_PATH.read_text(encoding="utf-8-sig").strip().lstrip("\ufeff")
    return host or None


def save_host(host: str) -> None:
    """保存 ESP32 地址，避免每次命令都输入 --host。"""
    CONFIG_PATH.write_text(host.strip() + "\n", encoding="utf-8")


def normalize_host(host: str) -> str:
    """把用户可能复制的 URL 规整成 urllib 可直接使用的 host。"""
    value = host.strip()
    if value.startswith("http://") or value.startswith("https://"):
        parsed = urllib.parse.urlparse(value)
        return parsed.netloc or parsed.path
    return value.rstrip("/")


def encode_dns_name(name: str) -> bytes:
    """把 clawd-mochi.local 这样的名字编码成 DNS 查询格式。"""
    parts = name.rstrip(".").split(".")
    return b"".join(bytes([len(part)]) + part.encode("ascii") for part in parts) + b"\x00"


def skip_dns_name(packet: bytes, offset: int) -> int:
    """跳过 DNS name，支持 mDNS 响应里常见的压缩指针。"""
    while offset < len(packet):
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            return offset + 2
        if length == 0:
            return offset + 1
        offset += 1 + length
    return offset


def discover_mdns_ipv4(timeout: float = 0.8) -> str | None:
    """主动查询 mDNS A 记录，避免完全依赖 Windows 的 .local 解析。"""
    transaction_id = 0
    header = struct.pack("!HHHHHH", transaction_id, 0, 1, 0, 0, 0)
    question = encode_dns_name(MDNS_HOST) + struct.pack("!HH", 1, 0x8001)
    query = header + question
    deadline = time.monotonic() + timeout

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.settimeout(timeout)
        sock.sendto(query, MDNS_GROUP)
        while time.monotonic() < deadline:
            try:
                packet, _ = sock.recvfrom(1500)
            except socket.timeout:
                break
            if len(packet) < 12:
                continue
            _, _, qd_count, an_count, ns_count, ar_count = struct.unpack("!HHHHHH", packet[:12])
            offset = 12
            for _ in range(qd_count):
                offset = skip_dns_name(packet, offset) + 4
            for _ in range(an_count + ns_count + ar_count):
                offset = skip_dns_name(packet, offset)
                if offset + 10 > len(packet):
                    break
                rtype, rclass, _, rdlen = struct.unpack("!HHIH", packet[offset:offset + 10])
                offset += 10
                rdata = packet[offset:offset + rdlen]
                offset += rdlen
                if rtype == 1 and (rclass & 0x7FFF) == 1 and rdlen == 4:
                    return socket.inet_ntoa(rdata)
    except OSError:
        return None
    finally:
        sock.close()
    return None


def candidate_hosts(host_arg: str | None) -> list[str]:
    """生成自动发现顺序：显式配置优先，主动 mDNS 发现，其次保存值和兜底地址。"""
    discovered = discover_mdns_ipv4()
    hosts = [
        host_arg,
        os.environ.get("MOCHI_HOST"),
        discovered,
        load_saved_host(),
        MDNS_HOST,
        DEFAULT_HOST,
    ]
    result: list[str] = []
    for host in hosts:
        if not host:
            continue
        normalized = normalize_host(host)
        if normalized and normalized not in result:
            result.append(normalized)
    return result


def resolve_host(host_arg: str | None) -> str:
    """返回当前最优 host；真正发送时还会继续尝试 mDNS 和热点兜底。"""
    return candidate_hosts(host_arg)[0]


def send_pet(host: str, mood: str, text: str, timeout: float) -> dict:
    """通过 HTTP 把桌宠事件发送到 ESP32-S3。"""
    query = urllib.parse.urlencode({"mood": normalize_mood(mood), "text": text})
    url = f"http://{host}/pet?{query}"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        body = response.read().decode("utf-8", errors="replace")
    try:
        return json.loads(body or "{}")
    except json.JSONDecodeError:
        return {"raw": body}


def send_pet_auto(host_arg: str | None, mood: str, text: str, timeout: float) -> tuple[str, dict]:
    """按显式 host、保存 host、mDNS、热点地址依次发送桌宠事件。"""
    last_error: BaseException | None = None
    for host in candidate_hosts(host_arg):
        try:
            return host, send_pet(host, mood, text, timeout)
        except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
            last_error = exc
    if last_error:
        raise last_error
    raise urllib.error.URLError("no host candidate")


def run_demo(host: str, timeout: float) -> None:
    """依次发送几种常用状态，用于快速确认桌宠桥接可用。"""
    steps = [
        ("happy", "LAN OK"),
        ("thinking", "Coding"),
        ("surprise", "Ping"),
        ("sleepy", "Idle"),
        ("love", "Done"),
        ("normal", ""),
    ]
    for mood, text in steps:
        result = send_pet(host, mood, text, timeout)
        print(json.dumps({"mood": mood, "result": result}, ensure_ascii=False))
        time.sleep(0.8)


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Send a pet event to Clawd Mochi.")
    parser.add_argument("mood", nargs="?", help="normal/happy/thinking/error/surprise/sleepy/love/wink/look")
    parser.add_argument("text", nargs="*", help="short ASCII text shown on the LCD")
    parser.add_argument("--host", help="ESP32 address, default: MOCHI_HOST, saved host, or 192.168.4.1")
    parser.add_argument("--set-host", help="save ESP32 LAN address for later commands")
    parser.add_argument("--demo", action="store_true", help="send a short status demo")
    parser.add_argument("--ping", action="store_true", help="send a small ping event")
    parser.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout seconds")
    args = parser.parse_args()

    if args.set_host:
        save_host(args.set_host)
        print(f"已保存 host：{args.set_host}")
        return 0

    text = " ".join(args.text).strip()
    mood = args.mood or "normal"
    if args.ping:
        mood = "happy"
        text = text or "PING"

    try:
        if args.demo:
            host = resolve_host(args.host)
            run_demo(host, args.timeout)
            return 0
        host, result = send_pet_auto(args.host, mood, text, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
        print(f"发送失败：{exc}", file=sys.stderr)
        print(f"已尝试 host：{', '.join(candidate_hosts(args.host))}", file=sys.stderr)
        print("请确认电脑和 ESP32 在同一局域网，或先连接热点 ClaWD-Mochi 后访问 http://192.168.4.1。", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
