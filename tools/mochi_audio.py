#!/usr/bin/env python3
"""Clawd Mochi 音频下行测试工具。

当前协议固定为 16 kHz / 16-bit little-endian / mono PCM。
后续 TTS 只要转成这个格式，即可 POST 到 ESP32 的 /audio/pcm。
"""

from __future__ import annotations

import argparse
import math
import os
import subprocess
import socket
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import wave
from pathlib import Path

from mochi_bridge import candidate_hosts, configure_stdio


SAMPLE_RATE = 16000


def make_tone(freq: float, seconds: float, volume: float) -> bytes:
    """生成一段短正弦测试音。"""
    frames = max(1, int(SAMPLE_RATE * seconds))
    pcm = bytearray()
    for i in range(frames):
        t = i / SAMPLE_RATE
        attack = min(1.0, t * 20.0)
        release = min(1.0, max(0.0, (seconds - t) * 20.0))
        env = min(attack, release)
        sample = int(math.sin(2.0 * math.pi * freq * t) * env * volume * 32767)
        pcm += struct.pack("<h", max(-32768, min(32767, sample)))
    return bytes(pcm)


def sample_to_i16(data: bytes, sample_width: int) -> int:
    """把 WAV 中的常见 PCM sample 转成 signed int16。"""
    if sample_width == 1:
        return (data[0] - 128) << 8
    if sample_width == 2:
        return struct.unpack("<h", data)[0]
    if sample_width == 3:
        value = int.from_bytes(data, "little", signed=False)
        if value & 0x800000:
            value -= 0x1000000
        return max(-32768, min(32767, value >> 8))
    if sample_width == 4:
        return max(-32768, min(32767, struct.unpack("<i", data)[0] >> 16))
    raise ValueError(f"unsupported sample width: {sample_width}")


def wav_to_pcm16_mono_16k(path: Path) -> bytes:
    """读取 WAV，并转换为 ESP32 固件期望的 16kHz/pcm16/mono。"""
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        source_rate = wav.getframerate()
        frame_count = wav.getnframes()
        raw = wav.readframes(frame_count)

    if channels <= 0 or sample_width <= 0 or source_rate <= 0:
        raise ValueError("invalid wav format")

    source_samples: list[int] = []
    frame_size = channels * sample_width
    for offset in range(0, len(raw), frame_size):
        total = 0
        for channel in range(channels):
            start = offset + channel * sample_width
            total += sample_to_i16(raw[start:start + sample_width], sample_width)
        source_samples.append(int(total / channels))

    if not source_samples:
        return b""

    if source_rate == SAMPLE_RATE:
        return b"".join(struct.pack("<h", sample) for sample in source_samples)

    target_count = max(1, int(len(source_samples) * SAMPLE_RATE / source_rate))
    ratio = source_rate / SAMPLE_RATE
    pcm = bytearray()
    last_index = len(source_samples) - 1
    for i in range(target_count):
        pos = i * ratio
        left = min(last_index, int(pos))
        right = min(last_index, left + 1)
        frac = pos - left
        sample = int(source_samples[left] * (1.0 - frac) + source_samples[right] * frac)
        pcm += struct.pack("<h", max(-32768, min(32767, sample)))
    return bytes(pcm)


def run_powershell(args: list[str], timeout: float = 20.0) -> subprocess.CompletedProcess[str]:
    """运行 Windows PowerShell，封装编码参数。"""
    return subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", *args],
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        timeout=timeout,
    )


def list_sapi_voices() -> str:
    """列出 Windows SAPI 可用语音。"""
    script = (
        "Add-Type -AssemblyName System.Speech; "
        "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$s.GetInstalledVoices() | ForEach-Object { "
        "$v=$_.VoiceInfo; ('{0} | {1} | {2} | {3}' -f $v.Name,$v.Culture,$v.Gender,$v.Age) }; "
        "$s.Dispose()"
    )
    result = run_powershell(["-Command", script])
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "list voices failed")
    return result.stdout.strip()


def synthesize_sapi_to_pcm(text: str, voice: str, rate: int, volume: int) -> bytes:
    """用 Windows SAPI 离线合成语音并转换成固件 PCM 格式。"""
    if os.name != "nt":
        raise RuntimeError("Windows SAPI TTS is only available on Windows")

    with tempfile.TemporaryDirectory(prefix="mochi_tts_") as tmp_dir:
        script_path = Path(tmp_dir) / "speak.ps1"
        wav_path = Path(tmp_dir) / "tts.wav"
        script = (
            "param([string]$Text,[string]$Out,[string]$Voice,[int]$Rate,[int]$Volume)\n"
            "Add-Type -AssemblyName System.Speech; "
            "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer; "
            "if ($Voice) { $s.SelectVoice($Voice) }; "
            "$s.Rate=$Rate; $s.Volume=$Volume; "
            "$s.SetOutputToWaveFile($Out); $s.Speak($Text); $s.Dispose()"
        )
        script_path.write_text(script, encoding="utf-8")
        result = run_powershell(
            ["-File", str(script_path), text, str(wav_path), voice, str(rate), str(volume)],
            timeout=max(20.0, len(text) * 0.25),
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "tts failed")
        return wav_to_pcm16_mono_16k(wav_path)


def post_pcm(host: str, pcm: bytes, timeout: float) -> bytes:
    """把 PCM 数据发送到 ESP32。"""
    request = urllib.request.Request(
        f"http://{host}/audio/pcm",
        data=pcm,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def post_auto(host_arg: str | None, pcm: bytes, timeout: float) -> tuple[str, bytes]:
    """按桥接器的自动发现顺序发送音频。"""
    last_error: BaseException | None = None
    for host in candidate_hosts(host_arg):
        try:
            return host, post_pcm(host, pcm, timeout)
        except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
            last_error = exc
    if last_error:
        raise last_error
    raise urllib.error.URLError("no host candidate")


def speak_auto(host_arg: str | None, text: str, timeout: float, voice: str = "", rate: int = 0, volume: int = 90) -> tuple[str, bytes]:
    """合成文字并发送给 ESP32 播放。"""
    pcm = synthesize_sapi_to_pcm(text, voice, max(-10, min(10, rate)), max(0, min(100, volume)))
    return post_auto(host_arg, pcm, timeout)


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Send PCM audio to Clawd Mochi.")
    parser.add_argument("--host", help="ESP32 host, default: auto discover")
    parser.add_argument("--say", help="speak text with Windows SAPI TTS")
    parser.add_argument("--voice", default="", help="SAPI voice name, empty uses Windows default")
    parser.add_argument("--rate", type=int, default=0, help="SAPI speech rate, -10 to 10")
    parser.add_argument("--tts-volume", type=int, default=90, help="SAPI volume, 0 to 100")
    parser.add_argument("--list-voices", action="store_true", help="list installed Windows SAPI voices")
    parser.add_argument("--freq", type=float, default=880.0, help="test tone frequency")
    parser.add_argument("--seconds", type=float, default=0.6, help="test tone duration")
    parser.add_argument("--volume", type=float, default=0.25, help="0.0 to 1.0")
    parser.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout seconds")
    args = parser.parse_args()

    if args.list_voices:
        print(list_sapi_voices())
        return 0

    try:
        if args.say:
            host, body = speak_auto(args.host, args.say, args.timeout, args.voice, args.rate, args.tts_volume)
        else:
            pcm = make_tone(args.freq, args.seconds, max(0.0, min(1.0, args.volume)))
            host, body = post_auto(args.host, pcm, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
        print(f"发送音频失败：{exc}", file=sys.stderr)
        print(f"已尝试 host：{', '.join(candidate_hosts(args.host))}", file=sys.stderr)
        return 1
    except (RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"生成语音失败：{exc}", file=sys.stderr)
        return 1

    print(f"已发送音频到 {host}: {body.decode('utf-8', errors='replace')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
