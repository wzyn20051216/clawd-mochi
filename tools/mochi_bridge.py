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
import urllib.error
import urllib.parse
import urllib.request


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


def normalize_mood(mood: str) -> str:
    """把常见英文状态词归一化为固件支持的 mood。"""
    key = mood.strip().lower()
    return MOOD_ALIASES.get(key, key or "normal")


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a pet event to Clawd Mochi.")
    parser.add_argument("mood", help="normal/happy/thinking/error/surprise/sleepy/love/wink/look")
    parser.add_argument("text", nargs="*", help="short ASCII text shown on the LCD")
    parser.add_argument("--host", default=os.environ.get("MOCHI_HOST", "192.168.4.1"), help="ESP32 address, default: MOCHI_HOST or 192.168.4.1")
    parser.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout seconds")
    args = parser.parse_args()

    text = " ".join(args.text).strip()
    try:
        result = send_pet(args.host, args.mood, text, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
        print(f"发送失败：{exc}", file=sys.stderr)
        print("请确认电脑已连接热点 ClaWD-Mochi，并且小屏控制页 http://192.168.4.1 可以打开。", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
