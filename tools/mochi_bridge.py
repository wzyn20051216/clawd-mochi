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
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


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
DAEMON_HOST = "127.0.0.1"
DAEMON_PORT = int(os.environ.get("MOCHI_DAEMON_PORT", "27665"))
DAEMON_DISABLED_VALUES = {"0", "false", "no", "off", "disabled"}


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
        deadline = time.monotonic() + hold_seconds
        reads = 0
        mode = "auto"
        while time.monotonic() < deadline:
            raw = await client.read_gatt_char(BLE_MODE_UUID)
            mode = normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore"))
            reads += 1
            await asyncio.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
    return {
        "ok": 1,
        "transport": "ble",
        "device": device.address,
        "bridge_mode": mode,
        "held_seconds": hold_seconds,
        "reads": reads,
    }


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


class DaemonRequest:
    """后台桥接器内部的一次桌宠事件请求。"""

    def __init__(self, host_arg: str | None, mood: str, text: str, timeout: float):
        self.host_arg = host_arg
        self.mood = normalize_mood(mood)
        self.text = text
        self.timeout = max(0.5, min(timeout, 10.0))
        self.created_at = time.monotonic()
        self.reply: queue.Queue[dict] = queue.Queue(maxsize=1)

    def respond(self, payload: dict) -> None:
        """把发送结果回传给 HTTP 请求线程。"""
        try:
            self.reply.put_nowait(payload)
        except queue.Full:
            pass


class MochiDaemonState:
    """后台桥接器共享状态。"""

    def __init__(self, host_arg: str | None):
        self.host_arg = host_arg
        self.requests: queue.Queue[DaemonRequest] = queue.Queue(maxsize=64)
        self.lock = threading.Lock()
        self.status: dict = {
            "ok": 1,
            "daemon": True,
            "connected": False,
            "transport": "idle",
            "bridge_mode": "auto",
            "phase": "starting",
        }

    def update(self, **values: object) -> None:
        """线程安全地更新状态快照。"""
        with self.lock:
            self.status.update(values)
            self.status["updated_at"] = time.time()

    def snapshot(self) -> dict:
        """返回当前后台状态。"""
        with self.lock:
            return dict(self.status)


def daemon_client_host() -> str:
    """返回 hook 访问的本机后台地址；默认是当前电脑自己的 loopback。"""
    return os.environ.get("MOCHI_DAEMON_HOST", DAEMON_HOST).strip() or DAEMON_HOST


def daemon_enabled() -> bool:
    """判断普通 CLI / hook 是否优先走本机常驻桥接器。"""
    if os.environ.get("MOCHI_IN_DAEMON") == "1":
        return False
    return os.environ.get("MOCHI_DAEMON", "1").strip().lower() not in DAEMON_DISABLED_VALUES


def daemon_autostart_enabled() -> bool:
    """判断 hook 找不到后台服务时是否自动拉起。"""
    return os.environ.get("MOCHI_DAEMON_AUTOSTART", "1").strip().lower() not in DAEMON_DISABLED_VALUES


def daemon_post(path: str, payload: dict | None, timeout: float) -> dict:
    """向本机常驻桥接器发送 JSON 请求。"""
    url = f"http://{daemon_client_host()}:{DAEMON_PORT}{path}"
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=max(0.3, min(timeout, 10.0))) as response:
        body = response.read().decode("utf-8", errors="replace")
    result = json.loads(body or "{}")
    return result if isinstance(result, dict) else {"raw": result}


def daemon_status(timeout: float = 0.5) -> dict:
    """读取本机后台桥接器状态。"""
    return daemon_post("/status", None, timeout)


def send_pet_daemon(host_arg: str | None,
                    mood: str,
                    text: str,
                    timeout: float,
                    drop_if_disconnected: bool = False) -> dict:
    """通过本机常驻桥接器发送桌宠状态。"""
    result = daemon_post(
        "/pet",
        {
            "host": host_arg,
            "mood": mood,
            "text": text,
            "timeout": timeout,
            "drop_if_disconnected": drop_if_disconnected,
        },
        timeout + 1.0,
    )
    if result.get("ok") != 1:
        raise RuntimeError(str(result.get("error") or result))
    return result


def start_daemon_background(host_arg: str | None = None) -> bool:
    """后台拉起常驻桥接器；默认只监听当前用户电脑的 loopback。"""
    try:
        if daemon_status(0.3).get("daemon"):
            return True
    except Exception:
        pass

    command = [sys.executable, str(Path(__file__).resolve()), "--daemon"]
    if host_arg:
        command.extend(["--host", host_arg])
    env = os.environ.copy()
    env["MOCHI_IN_DAEMON"] = "1"
    env.setdefault("MOCHI_DAEMON_AUTOSTART", "0")
    log_path = Path(__file__).with_name("mochi_daemon.log")
    creationflags = 0
    start_new_session = False
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
    else:
        start_new_session = True
    with log_path.open("ab") as output:
        subprocess.Popen(
            command,
            cwd=str(Path(__file__).resolve().parent),
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=env,
            creationflags=creationflags,
            start_new_session=start_new_session,
        )

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            if daemon_status(0.5).get("daemon"):
                return True
        except Exception:
            time.sleep(0.2)
    return False


async def daemon_worker(state: MochiDaemonState) -> None:
    """常驻 BLE 工作循环：保持连接、消费本机 hook 事件、断线自动重连。"""
    try:
        from bleak import BleakClient
    except ImportError as exc:
        state.update(connected=False, phase="missing_bleak", error="bleak not installed")
        raise RuntimeError("bleak not installed") from exc

    client = None
    device_address = ""
    last_keepalive = 0.0
    while True:
        try:
            if client is None or not getattr(client, "is_connected", False):
                state.update(connected=False, transport="ble", phase="scan")
                device = await find_ble_device(5.0)
                if device is None:
                    raise TimeoutError("BLE device not found")
                client = BleakClient(device, timeout=8.0)
                await client.connect()
                device_address = str(device.address)
                state.update(connected=True, transport="ble", device=device_address, phase="connected")

            try:
                request = await asyncio.to_thread(state.requests.get, True, 1.0)
            except queue.Empty:
                now = time.monotonic()
                if now - last_keepalive >= 15.0:
                    raw = await client.read_gatt_char(BLE_MODE_UUID)
                    mode = normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore"))
                    state.update(connected=True, bridge_mode=mode, phase="idle")
                    last_keepalive = now
                continue

            try:
                if time.monotonic() - request.created_at > request.timeout + 0.5:
                    request.respond({"ok": 0, "error": "stale daemon request"})
                    continue
                raw = await client.read_gatt_char(BLE_MODE_UUID)
                mode = normalize_bridge_mode(bytes(raw).decode("ascii", errors="ignore"))
                if mode == "wifi":
                    host, result = await asyncio.to_thread(
                        send_pet_wifi_candidates,
                        request.host_arg or state.host_arg,
                        request.mood,
                        request.text,
                        request.timeout,
                        None,
                    )
                    payload = {"ok": 1, "transport": "wifi-daemon", "host": host, "bridge_mode": mode, "result": result}
                else:
                    await client.write_gatt_char(BLE_STATUS_UUID, ble_payload(request.mood, request.text), response=False)
                    payload = {"ok": 1, "transport": "ble-daemon", "device": device_address, "bridge_mode": mode}
                request.respond(payload)
                state.update(connected=True, bridge_mode=mode, phase="idle", last_mood=request.mood, last_text=request.text)
            except Exception as exc:  # noqa: BLE001 - 单次发送失败后重连。
                request.respond({"ok": 0, "error": str(exc)})
                state.update(phase="send_error", error=str(exc))
                try:
                    await client.disconnect()
                except Exception:
                    pass
                client = None
        except Exception as exc:  # noqa: BLE001 - 后台循环必须自恢复。
            state.update(connected=False, phase="reconnect", error=str(exc))
            if client is not None:
                try:
                    await client.disconnect()
                except Exception:
                    pass
                client = None
            await asyncio.sleep(2.0)


class MochiDaemonHandler(BaseHTTPRequestHandler):
    """只监听本机 loopback 的轻量 HTTP 服务，供 hook 投递状态事件。"""

    server_version = "ClawdMochiDaemon/1.0"

    def log_message(self, format: str, *args: object) -> None:
        """禁止每次 hook 访问都刷终端。"""
        return

    def send_json(self, payload: dict, status: int = 200) -> None:
        """返回 JSON 响应。"""
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @property
    def daemon_state(self) -> MochiDaemonState:
        """取出挂在 HTTP server 上的共享状态。"""
        return self.server.mochi_state  # type: ignore[attr-defined]

    def do_GET(self) -> None:
        """处理状态查询和简单 GET 发送。"""
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/status":
            self.send_json(self.daemon_state.snapshot())
            return
        if parsed.path == "/shutdown":
            self.send_json({"ok": 1, "daemon": True, "stopping": True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        if parsed.path == "/pet":
            params = urllib.parse.parse_qs(parsed.query)
            self.handle_pet({
                "mood": (params.get("mood") or ["normal"])[0],
                "text": (params.get("text") or [""])[0],
                "host": (params.get("host") or [None])[0],
                "timeout": (params.get("timeout") or ["3"])[0],
            })
            return
        self.send_json({"ok": 0, "error": "not found"}, 404)

    def do_POST(self) -> None:
        """处理 hook 投递的桌宠事件。"""
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/pet":
            self.send_json({"ok": 0, "error": "not found"}, 404)
            return
        length = min(int(self.headers.get("Content-Length") or "0"), 4096)
        raw = self.rfile.read(length).decode("utf-8", errors="replace")
        try:
            payload = json.loads(raw or "{}")
        except json.JSONDecodeError:
            payload = {}
        self.handle_pet(payload if isinstance(payload, dict) else {})

    def handle_pet(self, payload: dict) -> None:
        """把 HTTP 请求转成后台 BLE 队列事件。"""
        timeout = float(payload.get("timeout") or 3.0)
        if payload.get("drop_if_disconnected") is True:
            snapshot = self.daemon_state.snapshot()
            if not snapshot.get("connected"):
                self.send_json({
                    "ok": 0,
                    "error": "daemon not connected",
                    "connected": False,
                    "phase": snapshot.get("phase"),
                }, 503)
                return
        request = DaemonRequest(
            str(payload.get("host") or "") or None,
            str(payload.get("mood") or "normal"),
            str(payload.get("text") or ""),
            timeout,
        )
        try:
            self.daemon_state.requests.put_nowait(request)
        except queue.Full:
            self.send_json({"ok": 0, "error": "daemon queue full"}, 503)
            return
        try:
            result = request.reply.get(timeout=request.timeout + 0.5)
        except queue.Empty:
            result = {"ok": 0, "error": "daemon send timeout"}
        self.send_json(result, 200 if result.get("ok") == 1 else 503)


def run_daemon(host_arg: str | None) -> int:
    """启动本机常驻桥接器，长期保持 BLE 连接并等待 hook 事件。"""
    configure_stdio()
    state = MochiDaemonState(host_arg)
    try:
        server = ThreadingHTTPServer(("127.0.0.1", DAEMON_PORT), MochiDaemonHandler)
    except OSError as exc:
        print(f"后台桥接器可能已在运行：{exc}")
        return 0
    server.mochi_state = state  # type: ignore[attr-defined]
    worker = threading.Thread(target=lambda: asyncio.run(daemon_worker(state)), name="mochi_ble", daemon=True)
    worker.start()
    state.update(phase="listening", url=f"http://127.0.0.1:{DAEMON_PORT}")
    print(f"Clawd Mochi 后台桥接器已启动：http://127.0.0.1:{DAEMON_PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def stop_daemon(timeout: float = 1.0) -> dict:
    """请求本机后台桥接器退出。"""
    return daemon_post("/shutdown", None, timeout)


def send_pet_auto(host_arg: str | None,
                  mood: str,
                  text: str,
                  timeout: float,
                  use_daemon: bool = True,
                  allow_fallback: bool = True,
                  daemon_autostart: bool = True,
                  daemon_drop_if_disconnected: bool = False) -> tuple[str, dict]:
    """按设备桥接模式发送：auto=BLE优先，ble=只BLE，wifi=只WiFi。"""
    force = normalize_bridge_mode(os.environ.get("MOCHI_TRANSPORT"))
    if os.environ.get("MOCHI_TRANSPORT") is None:
        force = "auto"

    if use_daemon and force == "auto" and daemon_enabled():
        try:
            return "DAEMON", send_pet_daemon(host_arg, mood, text, timeout, daemon_drop_if_disconnected)
        except Exception:
            if daemon_autostart and daemon_autostart_enabled() and start_daemon_background(host_arg):
                try:
                    return "DAEMON", send_pet_daemon(host_arg, mood, text, timeout, daemon_drop_if_disconnected)
                except Exception:
                    pass
            if not allow_fallback:
                raise

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
    parser.add_argument("--daemon", action="store_true", help="run persistent local BLE bridge daemon")
    parser.add_argument("--daemon-status", action="store_true", help="show persistent bridge daemon status")
    parser.add_argument("--daemon-stop", action="store_true", help="stop persistent local BLE bridge daemon")
    parser.add_argument("--no-daemon", action="store_true", help="send directly without using the local daemon")
    parser.add_argument("--timeout", type=float, default=3.0, help="BLE/HTTP timeout seconds")
    args = parser.parse_args()

    if args.daemon:
        return run_daemon(args.host)

    if args.daemon_status:
        try:
            print(json.dumps(daemon_status(args.timeout), ensure_ascii=False))
            return 0
        except Exception as exc:
            print(f"后台桥接器未运行：{exc}", file=sys.stderr)
            return 1

    if args.daemon_stop:
        try:
            print(json.dumps(stop_daemon(args.timeout), ensure_ascii=False))
            return 0
        except Exception as exc:
            print(f"后台桥接器未运行：{exc}", file=sys.stderr)
            return 1

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
        host, result = send_pet_auto(args.host, mood, text, args.timeout, use_daemon=not args.no_daemon)
    except (TimeoutError, socket.timeout, urllib.error.URLError, OSError, RuntimeError, asyncio.TimeoutError) as exc:
        print(f"发送失败：{exc}", file=sys.stderr)
        print(f"已尝试 host：{', '.join(candidate_hosts(args.host))}", file=sys.stderr)
        print("请确认 ESP32 已开机、电脑蓝牙已打开；若要走 WiFi 通道，请先对桌宠说“使用 WiFi”，并确认电脑和 ESP32 在同一局域网。配网可连接热点 ClaWD-Mochi 后访问 http://192.168.4.1。", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
