#!/usr/bin/env python3
"""生成 Clawd Mochi 桌宠桥接器的跨平台用户安装包。"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime
from pathlib import Path


PACKAGE_NAME = "clawd-mochi-bridge"
PACKAGE_FILES = (
    "mochi_bridge.py",
    "mochi_event.py",
    "agent_mochi.py",
    "install_mochi_bridge.py",
    "install_mochi_bridge.ps1",
    "install_mochi_bridge.sh",
)


def configure_stdio() -> None:
    """尽量让 Windows 终端也能正常打印中文提示。"""
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def project_root() -> Path:
    """返回项目根目录。"""
    return Path(__file__).resolve().parents[1]


def default_version() -> str:
    """优先使用 git 短哈希生成安装包版本号。"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=project_root(),
            text=True,
            capture_output=True,
            check=True,
        )
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return datetime.now().strftime("%Y%m%d")


def write_text(path: Path, text: str, executable: bool = False) -> None:
    """写入文本文件，并按需标记可执行权限。"""
    path.write_text(text.replace("\n", "\r\n") if path.suffix == ".cmd" else text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


def windows_cmd(action: str, title: str) -> str:
    """生成 Windows 双击脚本内容。"""
    action_arg = "" if action == "install" else f" -Action {action}"
    host_line = (
        'set "MOCHI_HOST=%~1"\n'
        'if "%MOCHI_HOST%"=="" set "MOCHI_HOST=clawd-mochi.local"\n'
        'powershell -ExecutionPolicy Bypass -File "%~dp0install_mochi_bridge.ps1" -MochiHost "%MOCHI_HOST%"\n'
        if action == "install"
        else f'powershell -ExecutionPolicy Bypass -File "%~dp0install_mochi_bridge.ps1"{action_arg}\n'
    )
    return f"""@echo off
chcp 65001 >nul
title Clawd Mochi {title}
cd /d "%~dp0"
{host_line}echo.
echo 操作完成，可以关闭这个窗口。
pause >nul
"""


def unix_script(action: str, title: str, pause: bool) -> str:
    """生成 macOS/Linux shell 脚本内容。"""
    action_arg = "" if action == "install" else f" --action {action}"
    if action == "install":
        command = 'python3 install_mochi_bridge.py --host "$HOST"'
        host_block = 'HOST="${1:-clawd-mochi.local}"\n'
    else:
        command = f"python3 install_mochi_bridge.py{action_arg}"
        host_block = ""
    pause_block = '\nprintf "操作完成，按回车关闭窗口..."\nread -r _\n' if pause else ""
    return f"""#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
{host_block}echo "Clawd Mochi {title}"
{command}
{pause_block}"""


def installer_readme(version: str) -> str:
    """生成安装包内的用户说明。"""
    return f"""# Clawd Mochi 桥接器安装包

版本：{version}

这个安装包用于让电脑上的 Codex / Claude Code 与 Clawd Mochi 桌宠联动。

## 使用前确认

1. 桌宠已经开机。
2. 电脑蓝牙已打开。蓝牙可用时，电脑和桌宠不需要在同一个 WiFi。
3. 如果需要 WiFi 通道，先对桌宠说“使用 WiFi”，并确认网页能用 `http://clawd-mochi.local` 或屏幕显示的 IP 打开。
4. 电脑已安装 Python 3。Windows 用户通常也可直接使用系统里的 `py -3`。
5. 安装器会尝试安装 `bleak`，用于 BLE 蓝牙桥接；安装失败时可切到“使用 WiFi”走局域网。

## Windows

双击：

```text
install_windows.cmd
```

如果 `clawd-mochi.local` 不可用，可以在命令行里指定屏幕显示的 IP：

```powershell
install_windows.cmd 192.168.1.123
```

## macOS

双击：

```text
install_macos.command
```

如果系统阻止运行，可在终端里执行：

```bash
cd 安装包目录
python3 install_mochi_bridge.py --host clawd-mochi.local
```

## Linux

在终端里执行：

```bash
sh install_linux.sh
```

## 测试和卸载

Windows：

```text
test_windows.cmd
uninstall_windows.cmd
```

macOS：

```text
test_macos.command
uninstall_macos.command
```

Linux：

```bash
sh test_linux.sh
sh uninstall_linux.sh
```

## 安装后效果

安装成功后，正常打开 Codex 或 Claude Code 即可。AI 开始思考、调用工具、完成任务或报错时，桌宠会自动变化表情并触发语音模块播报。

桥接器会读取桌宠当前桥接模式：自动桥接为 BLE 蓝牙优先且不主动连 WiFi；“使用蓝牙”为只走 BLE；“使用 WiFi”为只走 WiFi。用户可直接对桌宠说“自动桥接”“使用蓝牙”“使用 WiFi”来切换。

本桥接器只在本机写入 Codex / Claude Code 的 hook 配置，并向 ESP32 发送状态事件；不会读取或保存 Claude、Codex、OpenAI、Anthropic 的账号密钥。
"""


def copy_package_files(stage_dir: Path) -> None:
    """复制桥接器核心文件到临时打包目录。"""
    tools_dir = project_root() / "tools"
    for name in PACKAGE_FILES:
        source = tools_dir / name
        if not source.exists():
            raise FileNotFoundError(f"缺少打包文件：{source}")
        shutil.copy2(source, stage_dir / name)


def create_wrappers(stage_dir: Path) -> None:
    """生成用户双击或终端运行的包装脚本。"""
    write_text(stage_dir / "install_windows.cmd", windows_cmd("install", "Install"))
    write_text(stage_dir / "test_windows.cmd", windows_cmd("test", "Test"))
    write_text(stage_dir / "status_windows.cmd", windows_cmd("status", "Status"))
    write_text(stage_dir / "uninstall_windows.cmd", windows_cmd("uninstall", "Uninstall"))

    write_text(stage_dir / "install_macos.command", unix_script("install", "Install", True), executable=True)
    write_text(stage_dir / "test_macos.command", unix_script("test", "Test", True), executable=True)
    write_text(stage_dir / "status_macos.command", unix_script("status", "Status", True), executable=True)
    write_text(stage_dir / "uninstall_macos.command", unix_script("uninstall", "Uninstall", True), executable=True)

    write_text(stage_dir / "install_linux.sh", unix_script("install", "Install", False), executable=True)
    write_text(stage_dir / "test_linux.sh", unix_script("test", "Test", False), executable=True)
    write_text(stage_dir / "status_linux.sh", unix_script("status", "Status", False), executable=True)
    write_text(stage_dir / "uninstall_linux.sh", unix_script("uninstall", "Uninstall", False), executable=True)


def zip_directory(source_dir: Path, zip_path: Path) -> None:
    """压缩目录并保留 shell 脚本的可执行权限。"""
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source_dir.rglob("*")):
            if path.is_dir():
                continue
            arcname = source_dir.name + "/" + path.relative_to(source_dir).as_posix()
            info = zipfile.ZipInfo.from_file(path, arcname)
            if path.suffix in {".sh", ".command"}:
                info.external_attr = 0o755 << 16
            with path.open("rb") as handle:
                archive.writestr(info, handle.read(), compress_type=zipfile.ZIP_DEFLATED)


def file_sha256(path: Path) -> str:
    """计算文件 SHA-256，便于发布校验。"""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    """生成 dist 下的桥接器安装包。"""
    configure_stdio()
    parser = argparse.ArgumentParser(description="Build Clawd Mochi bridge installer package.")
    parser.add_argument("--version", default=default_version(), help="package version, default: git short hash")
    parser.add_argument("--out", default=str(project_root() / "dist"), help="output directory")
    args = parser.parse_args()

    out_dir = Path(args.out).expanduser().resolve()
    package_dir = out_dir / f"{PACKAGE_NAME}-{args.version}"
    zip_path = out_dir / f"{package_dir.name}.zip"

    if package_dir.exists():
        shutil.rmtree(package_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    package_dir.mkdir(parents=True)

    copy_package_files(package_dir)
    create_wrappers(package_dir)
    write_text(package_dir / "README_安装说明.md", installer_readme(args.version))
    zip_directory(package_dir, zip_path)
    (zip_path.with_suffix(zip_path.suffix + ".sha256")).write_text(
        f"{file_sha256(zip_path)}  {zip_path.name}\n",
        encoding="utf-8",
    )

    print(f"安装包目录：{package_dir}")
    print(f"安装包文件：{zip_path}")
    print(f"SHA256：{zip_path}.sha256")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
