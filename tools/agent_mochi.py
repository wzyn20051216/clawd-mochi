#!/usr/bin/env python3
"""运行 Codex / Claude Code，并把真实 JSON 事件流转成 ESP32 桌宠状态。"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

from mochi_bridge import configure_stdio, send_pet_auto
from mochi_event import compact_text, map_event, post_tool_failed


def command_to_text(command: list[str]) -> str:
    """生成适合当前系统 shell 执行的命令文本。"""
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return " ".join(subprocess.list2cmdline([part]) for part in command)


def send_status(host_arg: str | None, mood: str, text: str, timeout: float) -> None:
    """发送状态，失败时不影响 agent 继续运行。"""
    try:
        send_pet_auto(host_arg, mood, text, timeout)
    except Exception:
        pass


def codex_event_to_status(event: dict[str, Any]) -> tuple[str, str] | None:
    """把 Codex --json 事件映射到桌宠状态。"""
    event_type = str(event.get("type") or event.get("event") or "")
    item = event.get("item")
    item_type = str(item.get("type") or "") if isinstance(item, dict) else ""

    if event_type in {"session.started", "thread.started", "turn.started"}:
        return "thinking", "Codex"
    if event_type == "item.started" and item_type == "command_execution" and isinstance(item, dict):
        return "look", compact_text(str(item.get("command") or "cmd"), 18)
    if event_type == "item.completed" and item_type == "command_execution" and isinstance(item, dict):
        exit_code = item.get("exit_code")
        failed = isinstance(exit_code, int) and exit_code != 0
        return ("error" if failed else "happy"), compact_text(str(item.get("command") or "cmd"), 18)
    if event_type == "item.completed" and item_type == "agent_message":
        return "thinking", "Writing"
    if event_type in {"agent_message.delta", "response.output_text.delta"}:
        return "thinking", "Writing"
    if event_type in {"tool_call.started", "exec.started", "command.started"}:
        name = event.get("tool_name") or event.get("name") or event.get("command") or "tool"
        return "look", compact_text(str(name), 18)
    if event_type in {"tool_call.completed", "exec.completed", "command.completed"}:
        failed = False
        for key in ("exit_code", "exitCode", "returncode"):
            value = event.get(key)
            if isinstance(value, int) and value != 0:
                failed = True
        name = event.get("tool_name") or event.get("name") or event.get("command") or "tool"
        return ("error" if failed else "happy"), compact_text(str(name), 18)
    if event_type in {"error", "turn.failed"}:
        return "error", "Codex FAIL"
    if event_type in {"turn.completed", "session.completed", "thread.completed"}:
        return "happy", "Codex OK"

    text = json.dumps(event, ensure_ascii=True)
    lowered = text.lower()
    if "tool" in lowered and ("start" in lowered or "call" in lowered):
        return "look", "Tool"
    if "error" in lowered or "failed" in lowered:
        return "error", "Codex FAIL"
    if "complete" in lowered or "done" in lowered:
        return "happy", "Codex OK"
    return None


def claude_event_to_status(event: dict[str, Any]) -> tuple[str, str] | None:
    """把 Claude Code stream-json 事件映射到桌宠状态。"""
    hook_event = event.get("hook_event_name")
    if isinstance(hook_event, str):
        return map_event(event)

    event_type = str(event.get("type") or "")
    subtype = str(event.get("subtype") or "")

    if event_type == "system" and subtype == "init":
        return "happy", "Claude"
    if event_type == "assistant":
        return "thinking", "Writing"
    if event_type == "result":
        if event.get("is_error") or subtype in {"error", "failed"}:
            return "error", "Claude FAIL"
        return "happy", "Claude OK"
    if event_type == "tool_use":
        return "look", compact_text(str(event.get("name") or "tool"), 18)
    if event_type == "hook":
        return map_event(event)
    return None


def run_stream(command: list[str], mode: str, host_arg: str | None, timeout: float) -> int:
    """运行 agent 命令并消费 JSONL 事件流。"""
    send_status(host_arg, "thinking", mode.title(), timeout)
    process = subprocess.Popen(
        command_to_text(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        shell=True,
        bufsize=1,
    )

    mapper = codex_event_to_status if mode == "codex" else claude_event_to_status
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="")
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        status = mapper(event)
        if status:
            send_status(host_arg, status[0], status[1], timeout)

    return_code = process.wait()
    if return_code == 0:
        send_status(host_arg, "happy", f"{mode.title()} OK", timeout)
    else:
        send_status(host_arg, "error", f"{mode.title()} FAIL", timeout)
    return return_code


def build_command(mode: str, args: list[str]) -> list[str]:
    """补齐 agent 的 JSON 事件流参数。"""
    if mode == "codex":
        return ["codex", "exec", "--json", *args]
    return ["claude", "--print", "--verbose", "--output-format", "stream-json", "--include-hook-events", *args]


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Run Codex/Claude with Clawd Mochi status feedback.")
    parser.add_argument("mode", choices=["codex", "claude"], help="agent to run")
    parser.add_argument("--host", help="ESP32 address, default: saved host or 192.168.4.1")
    parser.add_argument("--timeout", type=float, default=1.5, help="HTTP timeout seconds")
    parser.add_argument("agent_args", nargs=argparse.REMAINDER, help="arguments passed to the agent")
    args = parser.parse_args()

    agent_args = args.agent_args
    if agent_args and agent_args[0] == "--":
        agent_args = agent_args[1:]
    command = build_command(args.mode, agent_args)
    return run_stream(command, args.mode, args.host, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
