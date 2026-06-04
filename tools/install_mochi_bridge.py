#!/usr/bin/env python3
"""跨平台安装 Clawd Mochi 的 Codex / Claude Code 全局桥接器。"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


EVENTS = ("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop")
SOURCE_FILES = ("mochi_bridge.py", "mochi_event.py", "agent_mochi.py")


def info(text: str) -> None:
    """输出统一前缀的安装信息。"""
    print(f"[Mochi] {text}")


def project_root() -> Path:
    """返回项目根目录。"""
    return Path(__file__).resolve().parents[1]


def user_home() -> Path:
    """返回用户主目录，测试时可用 MOCHI_INSTALL_HOME 覆盖。"""
    override = os.environ.get("MOCHI_INSTALL_HOME")
    return Path(override).expanduser() if override else Path.home()


def codex_home() -> Path:
    """返回 Codex 用户配置目录。"""
    return Path(os.environ.get("CODEX_HOME", user_home() / ".codex")).expanduser()


def claude_home() -> Path:
    """返回 Claude Code 用户配置目录。"""
    return Path(os.environ.get("CLAUDE_HOME", user_home() / ".claude")).expanduser()


def backup_file(path: Path) -> None:
    """给已有配置文件生成一次备份。"""
    if not path.exists():
        return
    backup = path.with_name(path.name + ".bak_mochi")
    index = 1
    while backup.exists():
        backup = path.with_name(f"{path.name}.bak_mochi_{index}")
        index += 1
    shutil.copy2(path, backup)


def read_json(path: Path) -> dict[str, Any]:
    """读取 JSON 配置文件，空文件或不存在时返回空对象。"""
    if not path.exists() or not path.read_text(encoding="utf-8-sig").strip():
        return {}
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    return data if isinstance(data, dict) else {}


def write_json(path: Path, data: dict[str, Any]) -> None:
    """用 UTF-8 写回 JSON 配置文件。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def quote_windows_bash_arg(value: str) -> str:
    """生成 Windows 下 Git Bash 可执行的参数。"""
    normalized = value.replace("\\", "/")
    return f'"{normalized.replace(chr(34), chr(92) + chr(34))}"'


def quote_powershell_arg(value: str) -> str:
    """生成 PowerShell 可执行的参数。"""
    normalized = value.replace("\\", "/")
    escaped = normalized.replace("`", "``").replace("$", "`$").replace('"', '`"')
    return f'"{escaped}"'


def claude_hook_command(python_bin: str, event_path: Path) -> str:
    """生成 Claude Code hook 命令；Windows 版 Claude 通常经 Git Bash 执行。"""
    return claude_event_hook_command(python_bin, event_path, None)


def codex_hook_command(python_bin: str, event_path: Path) -> str:
    """生成 Codex hook 命令；Windows 版 Codex 通常经 PowerShell 执行。"""
    return codex_event_hook_command(python_bin, event_path, None)


def claude_event_hook_command(python_bin: str, event_path: Path, event: str | None) -> str:
    """生成单个 Claude hook 命令。"""
    parts = [python_bin, str(event_path), "--timeout", "5"]
    if event:
        parts.extend(["--event", event])
    if os.name == "nt":
        return " ".join(quote_windows_bash_arg(part) for part in parts)
    return shlex.join(parts)


def codex_event_hook_command(python_bin: str, event_path: Path, event: str | None) -> str:
    """生成单个 Codex hook 命令。"""
    parts = [python_bin, str(event_path), "--timeout", "5"]
    if event:
        parts.extend(["--event", event])
    if os.name == "nt":
        return "& " + " ".join(quote_powershell_arg(part) for part in parts)
    return shlex.join(parts)


def is_mochi_hook(hook: Any) -> bool:
    """判断一个 hook 项是否属于 Mochi。"""
    if not isinstance(hook, dict):
        return False
    return "mochi_event.py" in str(hook.get("command") or "")


def remove_mochi_hooks(hooks: Any) -> dict[str, Any]:
    """从 hooks 配置中移除旧 Mochi hook，同时保留用户已有 hook。"""
    if not isinstance(hooks, dict):
        return {}
    result: dict[str, Any] = {}
    for event, matcher_blocks in hooks.items():
        kept_blocks: list[Any] = []
        for block in matcher_blocks if isinstance(matcher_blocks, list) else []:
            if not isinstance(block, dict):
                continue
            block_hooks = block.get("hooks")
            if not isinstance(block_hooks, list):
                kept_blocks.append(block)
                continue
            kept_hooks = [hook for hook in block_hooks if not is_mochi_hook(hook)]
            if kept_hooks:
                kept = dict(block)
                kept["hooks"] = kept_hooks
                kept_blocks.append(kept)
        if kept_blocks:
            result[str(event)] = kept_blocks
    return result


def mochi_matcher_block(command: str) -> dict[str, Any]:
    """生成一个通用 matcher block。"""
    return {
        "matcher": "",
        "hooks": [
            {
                "type": "command",
                "command": command,
            }
        ],
    }


def install_bridge_files(install_dir: Path, host: str | None) -> None:
    """复制桥接器文件，并保存 ESP32 地址。"""
    source_dir = project_root() / "tools"
    install_dir.mkdir(parents=True, exist_ok=True)
    for name in SOURCE_FILES:
        source = source_dir / name
        if not source.exists():
            raise FileNotFoundError(f"缺少源文件：{source}")
        shutil.copy2(source, install_dir / name)

    host_path = install_dir / ".mochi_host"
    source_host_path = source_dir / ".mochi_host"
    if host:
        host_path.write_text(host.strip() + "\n", encoding="utf-8")
    elif source_host_path.exists():
        shutil.copy2(source_host_path, host_path)


def ensure_ble_dependency(python_bin: str) -> bool:
    """尽量安装 bleak，让桥接器可优先使用 BLE；失败时可切到 WiFi 模式。"""
    check = subprocess.run(
        [python_bin, "-c", "import bleak"],
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if check.returncode == 0:
        info("BLE dependency: bleak already available.")
        return True

    info("BLE dependency: installing bleak for Bluetooth bridge...")
    install = subprocess.run(
        [python_bin, "-m", "pip", "install", "--user", "bleak"],
        text=True,
    )
    if install.returncode == 0:
        info("BLE dependency: bleak installed.")
        return True
    info("BLE dependency: install failed; WiFi bridge still works after switching Mochi to WiFi mode.")
    return False


def write_hooks_config(config_path: Path, command_for_event) -> None:
    """向单个配置文件写入 Mochi hook。"""
    backup_file(config_path)
    data = read_json(config_path)
    hooks = remove_mochi_hooks(data.get("hooks"))
    for event in EVENTS:
        command = command_for_event(event)
        blocks = hooks.get(event, [])
        if not isinstance(blocks, list):
            blocks = []
        blocks.append(mochi_matcher_block(command))
        hooks[event] = blocks
    data["hooks"] = hooks
    write_json(config_path, data)


def install_hooks(install_dir: Path, python_bin: str) -> dict[str, str]:
    """写入 Codex 和 Claude Code 全局 hook。"""
    event_path = install_dir / "mochi_event.py"
    commands = {
        "Codex": codex_event_hook_command(python_bin, event_path, "SessionStart"),
        "Claude": claude_event_hook_command(python_bin, event_path, "SessionStart"),
    }
    write_hooks_config(
        codex_home() / "hooks.json",
        lambda event: codex_event_hook_command(python_bin, event_path, event),
    )
    write_hooks_config(
        claude_home() / "settings.json",
        lambda event: claude_event_hook_command(python_bin, event_path, event),
    )
    return commands


def uninstall_hooks() -> None:
    """移除 Codex 和 Claude Code 全局 Mochi hook。"""
    for config_path in (codex_home() / "hooks.json", claude_home() / "settings.json"):
        if not config_path.exists():
            continue
        backup_file(config_path)
        data = read_json(config_path)
        data["hooks"] = remove_mochi_hooks(data.get("hooks"))
        write_json(config_path, data)


def bridge_status(install_dir: Path) -> None:
    """显示当前安装状态。"""
    host_path = install_dir / ".mochi_host"
    host = host_path.read_text(encoding="utf-8-sig").strip() if host_path.exists() else "(not saved)"
    codex_config = codex_home() / "hooks.json"
    claude_config = claude_home() / "settings.json"
    codex_ok = codex_config.exists() and "mochi_event.py" in codex_config.read_text(encoding="utf-8-sig")
    claude_ok = claude_config.exists() and "mochi_event.py" in claude_config.read_text(encoding="utf-8-sig")
    info(f"Install dir: {install_dir}")
    info(f"Bridge files: {(install_dir / 'mochi_event.py').exists()}")
    info(f"ESP32 host: {host}")
    info(f"Codex global hook: {codex_ok}")
    info(f"Claude global hook: {claude_ok}")


def test_bridge(install_dir: Path, python_bin: str) -> None:
    """发送一次 SessionStart 测试事件。"""
    event_path = install_dir / "mochi_event.py"
    if not event_path.exists():
        raise FileNotFoundError(f"桥接器未安装：{event_path}")
    payload = '{"hook_event_name":"SessionStart","source":"install"}'
    subprocess.run([python_bin, str(event_path)], input=payload, text=True, check=True)


def start_daemon(install_dir: Path, python_bin: str, host: str | None) -> None:
    """启动本机常驻桥接器；hook 会打 127.0.0.1，不依赖用户电脑局域网 IP。"""
    bridge_path = install_dir / "mochi_bridge.py"
    if not bridge_path.exists():
        raise FileNotFoundError(f"桥接器未安装：{bridge_path}")
    command = [python_bin, str(bridge_path), "--daemon"]
    if host:
        command.extend(["--host", host])
    log_path = install_dir / "mochi_daemon.log"
    with log_path.open("ab") as output:
        kwargs: dict[str, Any] = {
            "stdin": subprocess.DEVNULL,
            "stdout": output,
            "stderr": subprocess.STDOUT,
            "cwd": str(install_dir),
            "env": {**os.environ, "MOCHI_IN_DAEMON": "1", "MOCHI_DAEMON_AUTOSTART": "0"},
        }
        if os.name == "nt":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
        else:
            kwargs["start_new_session"] = True
        subprocess.Popen(command, **kwargs)
    info("Persistent local bridge daemon requested.")


def stop_daemon(install_dir: Path, python_bin: str) -> None:
    """停止本机常驻桥接器。"""
    bridge_path = install_dir / "mochi_bridge.py"
    if not bridge_path.exists():
        return
    subprocess.run([python_bin, str(bridge_path), "--daemon-stop"], text=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main() -> int:
    """解析命令行并执行安装动作。"""
    parser = argparse.ArgumentParser(description="Install Clawd Mochi bridge hooks for Codex / Claude Code.")
    parser.add_argument("--action", choices=("install", "status", "test", "uninstall"), default="install")
    parser.add_argument("--host", "--mochi-host", dest="host", help="ESP32 address, for example clawd-mochi.local")
    parser.add_argument("--install-dir", default=str(codex_home() / "mochi-bridge"), help="bridge install directory")
    parser.add_argument("--python", default=sys.executable or "python3", help="python executable used by hooks")
    parser.add_argument("--no-test", action="store_true", help="skip test event after install")
    args = parser.parse_args()

    install_dir = Path(args.install_dir).expanduser().resolve()
    if args.action == "install":
        install_bridge_files(install_dir, args.host)
        ensure_ble_dependency(args.python)
        commands = install_hooks(install_dir, args.python)
        stop_daemon(install_dir, args.python)
        start_daemon(install_dir, args.python, args.host)
        info("Global bridge installed.")
        info(f"Codex hook command: {commands['Codex']}")
        info(f"Claude hook command: {commands['Claude']}")
        bridge_status(install_dir)
        if not args.no_test:
            test_bridge(install_dir, args.python)
            info("Test event sent.")
    elif args.action == "status":
        bridge_status(install_dir)
    elif args.action == "test":
        test_bridge(install_dir, args.python)
        info("Test event sent.")
    elif args.action == "uninstall":
        uninstall_hooks()
        stop_daemon(install_dir, args.python)
        info("Codex / Claude global Mochi hooks removed.")
        info(f"Bridge files kept at: {install_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
