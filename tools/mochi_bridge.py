#!/usr/bin/env python3
"""Clawd Mochi 桌宠桥接器。

用法示例：
    python tools/mochi_bridge.py happy "build ok"
    python tools/mochi_bridge.py thinking "coding..."
    python tools/mochi_bridge.py error "build failed"
"""

from __future__ import annotations

import argparse
import asyncio
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
BLE_DEVICE_NAME = "Clawd Mochi"
BLE_SERVICE_UUID = "6d6f6368-692d-7065-742d-627269646765"
BLE_STATUS_UUID = "6d6f6368-692d-7065-742d-737461747573"
BLE_MODE_UUID = "6d6f6368-692d-7065-742d-62726964676d"
BLE_DISABLED_VALUES = {"0", "false", "no", "off", "disabled"}
BRIDGE_MODES = {"auto", "ble", "wifi"}


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


def ble_enabled() -> bool:
    """判断电脑端是否允许优先尝试 BLE。"""
    return os.environ.get("MOCHI_BLE", "1").strip().lower() not in BLE_DISABLED_VALUES


def normalize_bridge_mode(mode: str | None) -> str:
    """把设备返回的桥接模式规整为 auto/ble/wifi。"""
    value = (mode or "auto").strip().lower()
    return value if value in BRIDGE_MODES else "auto"


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


def http_get_json(host: str, path: str, timeout: float, params: dict[str, str] | None = None) -> dict:
    """通过 HTTP GET 读取 ESP32 返回的 JSON。"""
    query = urllib.parse.urlencode(params or {})
    url = f"http://{host}{path}"
    if query:
        url += f"?{query}"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        body = response.read().decode("utf-8", errors="replace")
    try:
        return json.loads(body or "{}")
    except json.JSONDecodeError:
        return {"raw": body}


def send_pet(host: str, mood: str, text: str, timeout: float) -> dict:
    """通过 HTTP 把桌宠事件发送到 ESP32-S3。"""
    return http_get_json(host, "/pet", timeout, {"mood": normalize_mood(mood), "text": text})


def read_state_http(host: str, timeout: float) -> dict:
    """通过 HTTP 读取设备状态。"""
    return http_get_json(host, "/state", timeout)


def compact_ble_text(text: str, limit: int = 17) -> str:
    """把 BLE 低延迟载荷压到默认 MTU 能稳定写入的长度。"""
    ascii_text = text.encode("ascii", errors="ignore").decode("ascii")
    cleaned = " ".join(ascii_text.split())
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[: limit - 3] + "..."


def ble_payload(mood: str, text: str) -> bytes:
    """生成固件支持的短 BLE 状态载荷。"""
    codes = {
        "normal": "N",
        "happy": "H",
        "thinking": "T",
        "error": "E",
        "surprise": "S",
        "sleepy": "P",
        "love": "L",
        "wink": "W",
        "look": "K",
    }
    normalized = normalize_mood(mood)
    code = codes.get(normalized, "N")
    return f"!{code}{compact_ble_text(text)}".encode("ascii", errors="ignore")


async def find_ble_device(timeout: float):
    """扫描 Clawd Mochi BLE 设备。"""
    try:
        from bleak import BleakScanner
    except ImportError as exc:
        raise RuntimeError("bleak not installed") from exc

    scan_timeout = max(0.6, min(timeout, 3.0))
    return await BleakScanner.find_device_by_filter(
        lambda dev, adv: (
            (dev.name or "") == BLE_DEVICE_NAME
            or BLE_SERVICE_UUID.lower() in {uuid.lower() for uuid in (adv.service_uuids or [])}
        ),
        timeout=scan_timeout,
    )


async def send_pet_ble_async(mood: str, text: str, timeout: float) -> dict:
    """通过 BLE GATT 写入桌宠状态。"""
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise RuntimeError("bleak not installed") from exc

    device = await find_ble_device(timeout)
    if device is None:
        raise TimeoutError("BLE device not found")

    async with BleakClient(device, timeout=timeout) as client:
        await client.write_gatt_char(BLE_STATUS_UUID, ble_payload(mood, text), response=False)
    return {"ok": 1, "transport": "ble", "device": device.address}


async def read_mode_ble_async(timeout: float) -> tuple[str, str]:
    """通过 BLE 读取设备当前桥接模式。"""
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise RuntimeError("bleak not installed") from exc

    device = await find_ble_device(timeout)
    if device is None:
        raise TimeoutError("BLE device not found")

    async with BleakClient(device, timeout=timeout) as client:
        raw = await client.read_gatt_char(BLE_MODE_UUID)
    return normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore")), device.address


async def send_pet_ble_respecting_mode_async(mood: str, text: str, timeout: float) -> dict:
    """同一次 BLE 连接中读取模式，并在允许时发送状态。"""
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise RuntimeError("bleak not installed") from exc

    device = await find_ble_device(timeout)
    if device is None:
        raise TimeoutError("BLE device not found")

    async with BleakClient(device, timeout=timeout) as client:
        raw = await client.read_gatt_char(BLE_MODE_UUID)
        mode = normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore"))
        if mode == "wifi":
            return {"ok": 0, "transport": "ble", "device": device.address, "bridge_mode": mode, "skipped": "wifi"}
        await client.write_gatt_char(BLE_STATUS_UUID, ble_payload(mood, text), response=False)
    return {"ok": 1, "transport": "ble", "device": device.address, "bridge_mode": mode}


async def hold_ble_async(seconds: float, timeout: float) -> dict:
    """保持 BLE 连接一段时间，用于区分短连接策略和真实掉线。"""
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise RuntimeError("bleak not installed") from exc

    device = await find_ble_device(timeout)
    if device is None:
        raise TimeoutError("BLE device not found")

    hold_seconds = max(1.0, min(seconds, 300.0))
    async with BleakClient(device, timeout=timeout) as client:
        raw = await client.read_gatt_char(BLE_MODE_UUID)
        mode = normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore"))
        await asyncio.sleep(hold_seconds)
    return {"ok": 1, "transport": "ble", "device": device.address, "bridge_mode": mode, "held_seconds": hold_seconds}


def send_pet_ble(mood: str, text: str, timeout: float) -> dict:
    """同步封装 BLE 发送，便于 hook 直接调用。"""
    return asyncio.run(send_pet_ble_async(mood, text, timeout))


def send_pet_ble_respecting_mode(mood: str, text: str, timeout: float) -> dict:
    """同步封装：读取设备模式后按 BLE 发送或跳过。"""
    return asyncio.run(send_pet_ble_respecting_mode_async(mood, text, timeout))


def read_mode_ble(timeout: float) -> tuple[str, str]:
    """同步读取 BLE 桥接模式。"""
    return asyncio.run(read_mode_ble_async(timeout))


def hold_ble(seconds: float, timeout: float) -> dict:
    """同步封装 BLE 保持连接诊断。"""
    return asyncio.run(hold_ble_async(seconds, timeout))


def send_pet_auto(host_arg: str | None, mood: str, text: str, timeout: float) -> tuple[str, dict]:
    """按设备桥接模式发送：auto=BLE优先，ble=只BLE，wifi=只WiFi。"""
    force = normalize_bridge_mode(os.environ.get("MOCHI_TRANSPORT"))
    if os.environ.get("MOCHI_TRANSPORT") is None:
        force = "auto"

    if force == "wifi":
        return send_pet_wifi_candidates(host_arg, mood, text, timeout, None)

    if force == "ble":
        return "BLE", send_pet_ble(mood, text, timeout)

    if ble_enabled():
        try:
            result = send_pet_ble_respecting_mode(mood, text, timeout)
            mode = normalize_bridge_mode(str(result.get("bridge_mode") or "auto"))
            if result.get("skipped") == "wifi":
                return send_pet_wifi_candidates(host_arg, mood, text, timeout, None)
            return "BLE", result
        except (RuntimeError, TimeoutError, OSError, asyncio.TimeoutError) as exc:
            last_ble_error: BaseException | None = exc
        except Exception as exc:
            last_ble_error = exc
    else:
        last_ble_error = None

    return send_pet_wifi_candidates(host_arg, mood, text, timeout, last_ble_error)


def send_pet_wifi_candidates(host_arg: str | None,
                             mood: str,
                             text: str,
                             timeout: float,
                             last_ble_error: BaseException | None) -> tuple[str, dict]:
    """按候选 host 尝试 WiFi HTTP 发送。"""
    last_error: BaseException | None = None
    for host in candidate_hosts(host_arg):
        try:
            state = read_state_http(host, min(timeout, 1.2))
            mode = normalize_bridge_mode(str(state.get("bridge_mode") or "auto"))
            if mode != "wifi":
                if last_ble_error:
                    raise last_ble_error
                raise TimeoutError(f"device bridge mode is {mode}")
            result = send_pet(host, mood, text, timeout)
            if isinstance(result, dict):
                result.setdefault("bridge_mode", mode)
            return host, result
        except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
            last_error = exc
    if last_error:
        raise last_error
    if last_ble_error:
        raise last_ble_error
    raise urllib.error.URLError("no host candidate")


def run_demo(host_arg: str | None, timeout: float) -> None:
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
        host, result = send_pet_auto(host_arg, mood, text, timeout)
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
    parser.add_argument("--hold", type=float, help="hold BLE connection for N seconds for diagnostics")
    parser.add_argument("--timeout", type=float, default=3.0, help="BLE/HTTP timeout seconds")
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
        if args.hold is not None:
            result = hold_ble(args.hold, args.timeout)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.demo:
            run_demo(args.host, args.timeout)
            return 0
        host, result = send_pet_auto(args.host, mood, text, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError, OSError, RuntimeError, asyncio.TimeoutError) as exc:
        print(f"发送失败：{exc}", file=sys.stderr)
        print(f"已尝试 host：{', '.join(candidate_hosts(args.host))}", file=sys.stderr)
        print("请确认 ESP32 已开机、电脑蓝牙已打开；若要走 WiFi 通道，请先对桌宠说“使用 WiFi”，并确认电脑和 ESP32 在同一局域网。配网可连接热点 ClaWD-Mochi 后访问 http://192.168.4.1。", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
