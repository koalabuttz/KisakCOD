#!/usr/bin/env python3
"""Self-test for scripts/tools/shellshock_screenshot_check.py (no game assets).

Builds synthetic screenshots for a working and a broken effect and checks the
verdicts, and checks the written config takes every screenshot the checker reads.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts/tools/shellshock_screenshot_check.py"
sys.path.insert(0, str(TOOL.parent))
import shellshock_screenshot_check as tool  # noqa: E402

W, H = 64, 48


def checker(shift: int = 0) -> list[int]:
    return [200 if ((x + shift) // 4 + y // 4) % 2 else 40 for y in range(H) for x in range(W)]


def blurred(shift: int = 0) -> list[int]:
    # A 5-wide box blur of the checkerboard.
    src = checker(shift)
    out = []
    for y in range(H):
        for x in range(W):
            vals = [src[y * W + min(max(x + d, 0), W - 1)] for d in range(-2, 3)]
            vals += [src[min(max(y + d, 0), H - 1) * W + x] for d in range(-2, 3)]
            out.append(sum(vals) // len(vals))
    return out


def write_set(directory: Path, shots: dict[str, list[int]]) -> None:
    directory.mkdir()
    for name, gray in shots.items():
        tool.write_tga(directory / f"{name}.tga", W, H, gray)


def run_check(directory: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), "check", str(directory)], capture_output=True, text=True)


def main() -> int:
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        good = {
            "shk_none_a": checker(), "shk_none_b": checker(), "shk_none_c": checker(),
            "shk_flash_on": [250] * (W * H), "shk_flash_off": checker(1),
            "shk_conc_on": blurred(1), "shk_conc_off": checker(2),
        }
        write_set(tmp / "good", good)
        result = run_check(tmp / "good")
        if result.returncode != 0 or "RESULT PASS" not in result.stdout:
            failures.append("working effects were not passed:\n" + result.stdout)

        broken = dict(good, shk_flash_on=checker(1), shk_conc_on=checker(1))
        write_set(tmp / "broken", broken)
        result = run_check(tmp / "broken")
        if result.returncode == 0 or "flashbang" not in result.stdout or result.stdout.count("FAIL") < 3:
            failures.append("missing effects were not failed:\n" + result.stdout)

        moving = dict(good, shk_none_b=blurred())
        write_set(tmp / "moving", moving)
        if run_check(tmp / "moving").returncode == 0:
            failures.append("a moving view was not failed")

        faded = dict(good, shk_none_c=[v // 2 for v in checker()])
        write_set(tmp / "faded", faded)
        if run_check(tmp / "faded").returncode == 0:
            failures.append("a view that faded by the end was not failed")

        (tmp / "partial").mkdir()
        if run_check(tmp / "partial").returncode == 0:
            failures.append("missing screenshots were not failed")

        # Bottom-first (default) TGA rows must be flipped: a top-white image
        # written bottom-first reads back with white on the first row.
        path = tmp / "bottom.tga"
        rows = [[255] * 4, [0] * 4]  # bottom row first in the file
        header = bytes([0, 0, 2]) + bytes(9) + bytes([4, 0, 2, 0, 24, 0])
        path.write_bytes(header + bytes(v for row in rows for g in row for v in (g, g, g)))
        img = tool.read_tga(path)
        if round(img.luma[0]) != 0 or round(img.luma[4]) != 255:
            failures.append("bottom-first TGA rows were not flipped")

        cfg = tmp / "shk.cfg"
        subprocess.run([sys.executable, str(TOOL), "write-cfg", str(cfg)], check=True, capture_output=True)
        text = cfg.read_text()
        taken = set(re.findall(r"screenshot (\w+)", text))
        if taken != set(tool.SHOTS):
            failures.append(f"config takes {sorted(taken)}, checker reads {sorted(tool.SHOTS)}")
        for line in text.splitlines():
            if line.startswith("set ") and (line.count('"') != 2 or len(line) > 1000):
                failures.append(f"config line is not one quoted value under 1000 chars: {line[:60]}")
        chain = re.findall(r"vstr (\w+)", text)
        defined = set(re.findall(r"^set (\w+)", text, re.MULTILINE))
        if not set(chain) <= defined or "shk_go" not in defined:
            failures.append("config vstr chain points at an undefined dvar")
        if "quit" not in text:
            failures.append("config does not quit at the end")

    for failure in failures:
        print("FAIL " + failure)
    if not failures:
        print("ok   shellshock screenshot check self-test")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
