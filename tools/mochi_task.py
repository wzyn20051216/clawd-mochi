#!/usr/bin/env python3
"""运行本地命令，并把开始/成功/失败状态同步到 Clawd Mochi。"""

from __future__ import annotations

import argparse
import os
import shutil
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


def command_to_shell_text(command: list[str]) -> str:
    """生成适合当前系统 shell 执行的命令文本。"""
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return " ".join(subprocess.list2cmdline([part]) for part in command)


def run_command(command: list[str]) -> subprocess.CompletedProcess:
    """通过系统 shell 运行命令，让 Windows 能解析 idf.py、bat、cmd 等入口。"""
    return subprocess.run(command_to_shell_text(command), shell=True)


def print_command_hint(command: list[str]) -> None:
    """对常见环境问题给出可操作提示。"""
    exe = Path(command[0]).name.lower()
    if exe == "idf.py" and shutil.which("idf.py") is None:
        print("提示：当前终端没有加载 ESP-IDF 环境，所以找不到 idf.py。", file=sys.stderr)
        print('可以先运行：cd "E:\\Espressif\\frameworks\\esp-idf-v5.5.2"; .\\export.ps1', file=sys.stderr)
        print('然后回到项目目录：cd "E:\\desktop\\clawd mochi"', file=sys.stderr)


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

    try:
        print_command_hint(command)
        result = run_command(command)
    except FileNotFoundError as exc:
        print(f"命令未找到：{command[0]} ({exc})", file=sys.stderr)
        result = subprocess.CompletedProcess(command, 127)

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
