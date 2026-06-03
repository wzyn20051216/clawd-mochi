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
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


CONFIG_PATH = Path(__file__).with_name(".mochi_host")
DEFAULT_HOST = "192.168.4.1"


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
    host = CONFIG_PATH.read_text(encoding="utf-8").strip()
    return host or None


def save_host(host: str) -> None:
    """保存 ESP32 地址，避免每次命令都输入 --host。"""
    CONFIG_PATH.write_text(host.strip() + "\n", encoding="utf-8")


def resolve_host(host_arg: str | None) -> str:
    """按命令行、环境变量、保存文件、默认热点地址的优先级选择 host。"""
    return host_arg or os.environ.get("MOCHI_HOST") or load_saved_host() or DEFAULT_HOST


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

    host = resolve_host(args.host)
    text = " ".join(args.text).strip()
    mood = args.mood or "normal"
    if args.ping:
        mood = "happy"
        text = text or "PING"

    try:
        if args.demo:
            run_demo(host, args.timeout)
            return 0
        result = send_pet(host, mood, text, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
        print(f"发送失败：{exc}", file=sys.stderr)
        print(f"当前 host：{host}", file=sys.stderr)
        print("请确认电脑和 ESP32 在同一局域网，或者先连接热点 ClaWD-Mochi 后访问 http://192.168.4.1。", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
