#!/usr/bin/env python3
"""运行本地命令，并把开始/成功/失败状态同步到 Clawd Mochi。"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from mochi_bridge import configure_stdio, resolve_host, send_pet


def shorten_text(text: str, limit: int = 18) -> str:
    """限制 LCD 文本长度，避免占用太多画面。"""
    cleaned = " ".join(text.split())
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[: limit - 3] + "..."


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Run a command with Clawd Mochi status feedback.")
    parser.add_argument("--host", help="ESP32 address, default: saved host or 192.168.4.1")
    parser.add_argument("--name", help="short task name shown on LCD")
    parser.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout seconds")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command after --, for example: -- idf.py build")
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing command, example: py -3 tools\\mochi_task.py -- idf.py build")

    host = resolve_host(args.host)
    task_name = shorten_text(args.name or Path(command[0]).name)

    try:
        send_pet(host, "thinking", task_name, args.timeout)
    except Exception as exc:  # noqa: BLE001 - 桌宠提示失败不应阻止本地命令执行。
        print(f"桌宠开始提示失败：{exc}", file=sys.stderr)

    result = subprocess.run(command, shell=False)

    try:
        if result.returncode == 0:
            send_pet(host, "happy", f"{task_name} OK", args.timeout)
        else:
            send_pet(host, "error", f"{task_name} FAIL", args.timeout)
    except Exception as exc:  # noqa: BLE001
        print(f"桌宠结束提示失败：{exc}", file=sys.stderr)

    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
