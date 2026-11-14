#!/usr/bin/env python3
"""A stand-in for the game in scripts/tests/test_pc_run.py.

It reads what scripts/runs/pc_run.py puts on the command line the way the
client does (``+set run_atMap "<seconds> <command> | ..."``, ``fs_homepath``)
and does what the commands ask, without waiting: ``framelog`` prints a frame
log line, ``screenshot <name>`` writes ``<home>/main/screenshots/<name>.tga``,
``quit`` ends. Everything goes to ``<home>/main/console_mp.log``.

The picture is grey (FAKE_SHADE, default 100) with a square of FAKE_SQUARE
(default the same grey) at 100,60 to 164,124, written bottom row first and
RLE-packed when FAKE_RLE=1. FAKE_EXIT sets the exit code, FAKE_HANG=1 hangs.
"""

import os
import struct
import sys
import time
from pathlib import Path

WIDTH, HEIGHT = 320, 200


def setting(argv, name):
    for i in range(len(argv) - 2):
        if argv[i] == "+set" and argv[i + 1] == name:
            return argv[i + 2]
    return ""


def tga(path, shade, square, rle):
    rows = []
    for y in range(HEIGHT - 1, -1, -1):  # bottom row first
        row = bytearray()
        for x in range(WIDTH):
            value = square if 100 <= x < 164 and 60 <= y < 124 else shade
            row += bytes((value, value, value))  # B, G, R
        rows.append(bytes(row))
    pixels = b"".join(rows)
    if rle:
        packed = bytearray()
        for i in range(0, len(pixels), 3 * 128):
            run = pixels[i:i + 3 * 128]
            count = len(run) // 3
            if run == run[:3] * count:
                packed += bytes((0x80 | (count - 1),)) + run[:3]
            else:
                packed += bytes((count - 1,)) + run
        body, kind = bytes(packed), 10
    else:
        body, kind = pixels, 2
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, kind, 0, 0, 0, 0, 0, WIDTH, HEIGHT, 24, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + body)


def main():
    argv = sys.argv[1:]
    if os.environ.get("FAKE_HANG") == "1":
        time.sleep(3600)
    home = Path(setting(argv, "fs_homepath") or ".")
    log = home / "main" / "console_mp.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"env KISAK_VK_MODERN_AO={os.environ.get('KISAK_VK_MODERN_AO', '-')}",
             f"run: run_atMap \"{setting(argv, 'run_atMap')}\"",
             f"run: run_quitAfterMap {setting(argv, 'run_quitAfterMap') or 0}",
             "vulkan d3d9: modern: 1 scenes with ambient occlusion, 0.50 ms GPU each",
             "run: the map is up"]
    shade = int(os.environ.get("FAKE_SHADE", "100"))
    square = int(os.environ.get("FAKE_SQUARE", str(shade)))
    for part in setting(argv, "run_atMap").split("|"):
        words = part.split()
        if len(words) < 2:
            continue
        command = words[1:]
        lines.append(f"run: sending \"{' '.join(command)}\"")
        if command[0] == "framelog":
            lines.append(f"frame log {command[2] if len(command) > 2 else '-'}: 1000 frames in 10.000 s: 100.00 fps; "
                         "ms mean 10.000, median 10.000, p95 11.000, p99 12.000, max 13.000; 0 over 50 ms")
        elif command[0] == "screenshot":
            tga(home / "main" / "screenshots" / f"{command[1]}.tga", shade, square, os.environ.get("FAKE_RLE") == "1")
            lines.append(f"Wrote screenshots/{command[1]}.tga")
        elif command[0] == "quit":
            break
    lines.append("vulkan d3d9: device destroyed after 0 validation errors")
    log.write_text("\n".join(lines) + "\n")
    return int(os.environ.get("FAKE_EXIT", "0"))


if __name__ == "__main__":
    sys.exit(main())
