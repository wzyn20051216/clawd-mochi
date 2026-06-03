#!/usr/bin/env python3
"""运行 Codex / Claude Code，并把真实 JSON 事件流转成 ESP32 桌宠状态。"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

from mochi_bridge import configure_stdio, send_pet_auto
from mochi_audio import speak_auto
from mochi_event import compact_text, map_event, post_tool_failed

TEXT_KEYS = {"text", "content", "delta", "output_text", "result", "message"}
SKIP_TEXT_KEYS = {
    "type",
    "subtype",
    "role",
    "id",
    "tool_name",
    "name",
    "command",
    "status",
    "event",
}


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


def speak_status(host_arg: str | None, text: str, timeout: float) -> None:
    """可选语音播报，失败时不影响 agent 主流程。"""
    if os.environ.get("MOCHI_SPEAK", "").lower() not in {"1", "true", "yes", "on"}:
        return
    try:
        speak_auto(host_arg, text, max(5.0, timeout), os.environ.get("MOCHI_VOICE", ""), 0, 90)
    except Exception:
        pass


def speech_enabled() -> bool:
    """是否启用 ESP32 语音播报。"""
    return os.environ.get("MOCHI_SPEAK", "").lower() in {"1", "true", "yes", "on"}


def speak_text(host_arg: str | None, text: str, timeout: float) -> None:
    """播放一段回复文本，失败时不影响 agent 主流程。"""
    if not speech_enabled():
        return
    cleaned = clean_spoken_reply(text)
    if not cleaned:
        return
    try:
        speak_auto(host_arg, cleaned, max(10.0, timeout), os.environ.get("MOCHI_VOICE", ""), 0, 90)
    except Exception:
        pass


def clean_spoken_reply(text: str) -> str:
    """把最终回复清理成适合 TTS 的短文本。"""
    limit = int(os.environ.get("MOCHI_SPEAK_MAX_CHARS", "220") or "220")
    cleaned = re.sub(r"```.*?```", " ", text, flags=re.S)
    cleaned = re.sub(r"`([^`]*)`", r"\1", cleaned)
    cleaned = re.sub(r"https?://\S+", " 链接 ", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    if not cleaned:
        return ""
    return cleaned[: max(40, limit)]


def extract_text_value(value: Any, depth: int = 0) -> list[str]:
    """从常见 agent JSON 结构里保守提取自然语言文本。"""
    if depth > 6:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        parts: list[str] = []
        for item in value:
            parts.extend(extract_text_value(item, depth + 1))
        return parts
    if isinstance(value, dict):
        parts = []
        for key, item in value.items():
            key_text = str(key)
            if key_text in SKIP_TEXT_KEYS:
                continue
            if key_text in TEXT_KEYS:
                parts.extend(extract_text_value(item, depth + 1))
        return parts
    return []


def codex_event_reply_text(event: dict[str, Any]) -> str:
    """从 Codex JSON 事件提取回复文本片段。"""
    event_type = str(event.get("type") or event.get("event") or "")
    if event_type in {"agent_message.delta", "response.output_text.delta"}:
        for key in ("delta", "text", "content"):
            value = event.get(key)
            if isinstance(value, str):
                return value
        return " ".join(extract_text_value(event.get("item")))

    item = event.get("item")
    item_type = str(item.get("type") or "") if isinstance(item, dict) else ""
    if event_type == "item.completed" and item_type == "agent_message" and isinstance(item, dict):
        return " ".join(extract_text_value(item))
    if event_type in {"turn.completed", "session.completed", "thread.completed"}:
        return " ".join(extract_text_value(event))
    return ""


def claude_event_reply_text(event: dict[str, Any]) -> str:
    """从 Claude stream-json 事件提取回复文本片段。"""
    event_type = str(event.get("type") or "")
    if event_type == "assistant":
        message = event.get("message")
        if isinstance(message, dict):
            return " ".join(extract_text_value(message.get("content")))
        return " ".join(extract_text_value(event.get("content")))
    if event_type == "result":
        result = event.get("result")
        if isinstance(result, str):
            return result
        return " ".join(extract_text_value(result))
    return ""


class ReplyCapture:
    """收集 agent 流式回复，结束时朗读最终回答。"""

    def __init__(self, mode: str) -> None:
        self.mode = mode
        self.parts: list[str] = []
        self.last_full = ""

    def feed(self, event: dict[str, Any]) -> None:
        text = codex_event_reply_text(event) if self.mode == "codex" else claude_event_reply_text(event)
        if not text:
            return
        event_type = str(event.get("type") or event.get("event") or "")
        if event_type in {"item.completed", "turn.completed", "session.completed", "thread.completed", "assistant", "result"}:
            if len(text) >= len(self.last_full):
                self.last_full = text
        else:
            self.parts.append(text)

    def final_text(self) -> str:
        delta_text = "".join(self.parts).strip()
        if self.last_full and (not delta_text or len(self.last_full) >= len(delta_text)):
            return self.last_full
        return delta_text


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
    speak_status(host_arg, f"{mode} started", timeout)
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
    replies = ReplyCapture(mode)
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="")
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        replies.feed(event)
        status = mapper(event)
        if status:
            send_status(host_arg, status[0], status[1], timeout)

    return_code = process.wait()
    if return_code == 0:
        send_status(host_arg, "happy", f"{mode.title()} OK", timeout)
        final_reply = replies.final_text()
        speak_text(host_arg, final_reply, timeout)
        if not final_reply:
            speak_status(host_arg, f"{mode} done", timeout)
    else:
        send_status(host_arg, "error", f"{mode.title()} FAIL", timeout)
        speak_status(host_arg, f"{mode} failed", timeout)
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
