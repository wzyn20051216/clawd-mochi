#!/usr/bin/env python3
"""Clawd Mochi 音频下行测试工具。

当前协议固定为 16 kHz / 16-bit little-endian / mono PCM。
后续 TTS 只要转成这个格式，即可 POST 到 ESP32 的 /audio/pcm。
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import urllib.error
import urllib.request

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


def main() -> int:
    configure_stdio()

    parser = argparse.ArgumentParser(description="Send PCM audio to Clawd Mochi.")
    parser.add_argument("--host", help="ESP32 host, default: auto discover")
    parser.add_argument("--freq", type=float, default=880.0, help="test tone frequency")
    parser.add_argument("--seconds", type=float, default=0.6, help="test tone duration")
    parser.add_argument("--volume", type=float, default=0.25, help="0.0 to 1.0")
    parser.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout seconds")
    args = parser.parse_args()

    pcm = make_tone(args.freq, args.seconds, max(0.0, min(1.0, args.volume)))
    try:
        host, body = post_auto(args.host, pcm, args.timeout)
    except (TimeoutError, socket.timeout, urllib.error.URLError) as exc:
        print(f"发送音频失败：{exc}", file=sys.stderr)
        print(f"已尝试 host：{', '.join(candidate_hosts(args.host))}", file=sys.stderr)
        return 1

    print(f"已发送 {len(pcm)} bytes PCM 到 {host}: {body.decode('utf-8', errors='replace')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
