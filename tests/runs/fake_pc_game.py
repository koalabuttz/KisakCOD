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

A timed entry may hold several commands split by ';', as the console takes
them. ``<dvar> <value>`` sets a dvar: ``r_fakeSquare`` then shades the square
and ``r_fakeCost`` (ms) slows the frame log (fps = 1000 / (10 + cost)), so a
launch with variants gets different pictures and frame rates per variant.
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
             f"env KISAK_NO_DIALOGS={os.environ.get('KISAK_NO_DIALOGS', '-')}",
             f"run: run_atMap \"{setting(argv, 'run_atMap')}\"",
             f"run: run_quitAfterMap {setting(argv, 'run_quitAfterMap') or 0}",
             "vulkan d3d9: modern: 1 scenes with ambient occlusion, 0.50 ms GPU each",
             "run: the map is up"]
    shade = int(os.environ.get("FAKE_SHADE", "100"))
    square = int(os.environ.get("FAKE_SQUARE", str(shade)))
    dvars = {}
    quit_now = False
    for part in setting(argv, "run_atMap").split("|"):
        words = part.split(None, 1)
        if len(words) < 2:
            continue
        lines.append(f"run: sending \"{words[1].strip()}\"")
        for text in words[1].split(";"):
            command = text.split()
            if not command:
                continue
            if command[0] == "framelog":
                ms = 10.0 + float(dvars.get("r_fakeCost", 0))
                lines.append(f"frame log {command[2] if len(command) > 2 else '-'}: 1000 frames in {ms:.3f} s: "
                             f"{1000 / ms:.2f} fps; ms mean {ms:.3f}, median {ms:.3f}, p95 {ms + 1:.3f}, "
                             f"p99 {ms + 2:.3f}, max {ms + 3:.3f}; 0 over 50 ms")
            elif command[0] == "screenshot":
                here = int(dvars.get("r_fakeSquare", square))
                tga(home / "main" / "screenshots" / f"{command[1]}.tga", shade, here, os.environ.get("FAKE_RLE") == "1")
                lines.append(f"Wrote screenshots/{command[1]}.tga")
            elif command[0] == "quit":
                quit_now = True
                break
            elif len(command) == 2 and command[0].startswith("r_"):
                dvars[command[0]] = command[1]
        if quit_now:
            break
    lines.append("vulkan d3d9: device destroyed after 0 validation errors")
    log.write_text("\n".join(lines) + "\n")
    return int(os.environ.get("FAKE_EXIT", "0"))


if __name__ == "__main__":
    sys.exit(main())
