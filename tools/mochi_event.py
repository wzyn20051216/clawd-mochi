#!/usr/bin/env python3
"""把 Codex / Claude Code hook 事件映射到 ESP32 桌宠状态。"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from mochi_bridge import configure_stdio, send_pet_auto


ACTIVE_EVENTS = {
    "UserPromptSubmit",
    "PreToolUse",
    "PermissionRequest",
    "SubagentStart",
    "TaskCreated",
    "Setup",
}

DONE_EVENTS = {
    "SessionStart",
    "Stop",
    "SessionEnd",
    "SubagentStop",
    "TaskCompleted",
}

ERROR_EVENTS = {
    "PostToolUseFailure",
    "StopFailure",
    "PermissionDenied",
}

OUTPUT_JSON_EVENTS = {
    "Stop",
    "SubagentStop",
    "StopFailure",
}

def log_hook(message: str) -> None:
    """写入轻量 hook 日志，便于判断全局 hook 是否触发。"""
    try:
        path = Path(__file__).with_name("mochi_hook.log")
        with path.open("a", encoding="utf-8") as handle:
            handle.write(message + "\n")
    except OSError:
        pass


def read_event() -> dict[str, Any]:
    """读取 hook stdin；没有输入时返回空事件，便于手动测试。"""
    raw = sys.stdin.read().strip()
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return {"hook_event_name": "InvalidHookJson", "raw": raw}
    return data if isinstance(data, dict) else {"hook_event_name": "UnknownHookJson", "raw": data}


def compact_text(text: str, limit: int = 18) -> str:
    """把状态文字压到 LCD 适合显示的 ASCII 短文本。"""
    ascii_text = text.encode("ascii", errors="ignore").decode("ascii")
    cleaned = " ".join(ascii_text.split())
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[: limit - 3] + "..."


def tool_label(data: dict[str, Any]) -> str:
    """生成工具调用的短标签。"""
    tool = str(data.get("tool_name") or data.get("tool") or "tool")
    tool_input = data.get("tool_input")
    if isinstance(tool_input, dict):
        command = tool_input.get("command")
        if isinstance(command, str) and command.strip():
            return compact_text(command, 18)
        path = tool_input.get("file_path") or tool_input.get("path")
        if isinstance(path, str) and path.strip():
            return compact_text(f"{tool} {Path(path).name}", 18)
    return compact_text(tool, 18)


def post_tool_failed(data: dict[str, Any]) -> bool:
    """尽量兼容 Codex 与 Claude 的工具结果字段，判断工具是否失败。"""
    if data.get("hook_event_name") == "PostToolUseFailure":
        return True

    response = data.get("tool_response")
    if isinstance(response, dict):
        for key in ("exit_code", "status", "returncode"):
            value = response.get(key)
            if isinstance(value, int) and value != 0:
                return True
        if response.get("is_error") is True or response.get("error") is not None:
            return True
        status = str(response.get("status") or "").lower()
        if status in {"error", "failed", "failure"}:
            return True

    return False


def map_event(data: dict[str, Any]) -> tuple[str, str]:
    """把 hook 事件映射为固件支持的 mood 和短文本。"""
    event = str(data.get("hook_event_name") or data.get("event") or "manual")

    if event == "SessionStart":
        source = compact_text(str(data.get("source") or "start"), 10)
        return "happy", f"AI {source}"
    if event == "UserPromptSubmit":
        return "thinking", "Thinking"
    if event == "PreToolUse":
        return "look", tool_label(data)
    if event == "PermissionRequest":
        return "surprise", "Need OK"
    if event == "PostToolUse":
        if post_tool_failed(data):
            return "error", tool_label(data)
        return "happy", tool_label(data)
    if event in ERROR_EVENTS:
        return "error", compact_text(event.replace("Failure", "Fail"), 18)
    if event == "SubagentStart":
        return "thinking", compact_text(str(data.get("agent_type") or "Agent"), 18)
    if event == "SubagentStop":
        return "happy", compact_text(str(data.get("agent_type") or "Agent OK"), 18)
    if event == "Stop":
        return "normal", "Ready"
    if event == "SessionEnd":
        return "sleepy", "Bye"
    if event in ACTIVE_EVENTS:
        return "thinking", compact_text(event, 18)
    if event in DONE_EVENTS:
        return "happy", compact_text(event, 18)
    return "normal", compact_text(event, 18)


def hook_stdout(data: dict[str, Any]) -> None:
    """按 hook 事件要求输出最小 JSON，避免影响 agent 行为。"""
    event = str(data.get("hook_event_name") or data.get("event") or "")
    if event in OUTPUT_JSON_EVENTS:
        print(json.dumps({"continue": True}, separators=(",", ":")))


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Bridge Codex/Claude hook events to Clawd Mochi.")
    parser.add_argument("--host", help="ESP32 address, default: saved host or 192.168.4.1")
    parser.add_argument("--event", help="manual event name for testing")
    parser.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout seconds")
    args = parser.parse_args()

    data = {"hook_event_name": args.event} if args.event else read_event()

    mood, text = map_event(data)
    event = str(data.get("hook_event_name") or data.get("event") or "manual")
    try:
        host, _ = send_pet_auto(args.host, mood, text, args.timeout)
        log_hook(f"{event}: pet {mood} {text} -> {host}")
    except Exception as exc:  # noqa: BLE001 - hook 失败不能影响 Codex / Claude 主流程。
        log_hook(f"{event}: pet failed {mood} {text}: {exc}")
        pass
    hook_stdout(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
