#!/usr/bin/env python3
"""Build/check the opt-in real SDL window host; dummy tests do not prove OS input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def run(command, *, env=None, timeout=1800):
    result = subprocess.run([str(part) for part in command], cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{result.stdout[-5000:]}\n{result.stderr[-5000:]}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--build-type", default="Debug")
    parser.add_argument("--sdl2-dir", type=Path)
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--video-driver", default="dummy", help="explicit test driver; X11 also needs DISPLAY")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    dedicated = ROOT / "bin/KisakCOD-dedi"
    before = fingerprint(dedicated)
    checks = 0

    def check(condition, label):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(label)
        print("PASS:", label, flush=True)

    try:
        text = (ROOT / "CMakeLists.txt").read_text()
        option = text[text.index("option(KISAK_BUILD_LINUX_WINDOW_HOST"):]
        check(bool(re.search(r"\bOFF\)", option[:250])), "window host defaults OFF")
        command = ["cmake", "-S", ROOT, "-B", build, "-DKISAK_PLATFORM=linux",
                   "-DKISAK_BUILD_LINUX_WINDOW_HOST=ON", f"-DCMAKE_BUILD_TYPE={args.build_type}",
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={build / 'bin'}"]
        for key, value in (("SDL2_DIR", args.sdl2_dir), ("CMAKE_C_COMPILER", args.cc), ("CMAKE_CXX_COMPILER", args.cxx)):
            if value:
                command.append(f"-D{key}={value}")
        run(command)
        entries = json.loads((build / "compile_commands.json").read_text())
        for suffix, target in (("window/window_host.cpp", "KisakCOD-window-platform"),
                               ("window/engine_input.cpp", "KisakCOD-window-platform"),
                               ("linux/sys_events.cpp", "KisakCOD-client-window"),
                               ("linux/sys_critical.cpp", "KisakCOD-client-window"),
                               ("linux/sys_posix.cpp", "KisakCOD-client-window"),
                               ("tests/linux_window/window_test.cpp", "KisakCOD-window-test")):
            commands = [entry["command"] for entry in entries if entry["file"].endswith(suffix)
                        and target + ".dir" in entry["command"]]
            check(len(commands) == 1, f"actual production compile entry: {target}/{suffix}")
            command = commands[0]
            for flag in ("-m32", "-DKISAK_MP", "-DRELEASE_ASSERTS"):
                check(bool(re.search(r"(?:^|\s)" + re.escape(flag) + r"(?:\s|$)", command)), f"{suffix} uses {flag}")
            check("-DKISAK_DEDICATED" not in command and "-DDEDICATED" not in command, f"{suffix} is a client compile")
        command = ["cmake", "--build", build, "--target", "KisakCOD-client-window", "KisakCOD-window-test", "-j", args.jobs]
        run(command)
        run(command)
        check(True, "window targets build and incrementally rebuild")
        env = dict(os.environ, SDL_VIDEODRIVER=args.video_driver)
        result = run([build / "bin/KisakCOD-window-test"], env=env, timeout=30)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        summary = re.search(r"-- (\d+) checks, (\d+) failures --", result.stdout)
        check(summary and int(summary[1]) >= 34 and int(summary[2]) == 0, "real SDL event/engine queue suite passes")
        check(f"SDL driver={args.video_driver}" in result.stdout, "test reports the explicitly selected SDL driver")
        shell = build / "bin/KisakCOD-client-window"
        result = run([shell, "--frames", "2", "--trace", "--title", "KisakCOD window smoke"], env=env, timeout=15)
        print(result.stdout, end="")
        check("window=ready" in result.stdout and "window=closed" in result.stdout and "held=0" in result.stdout
              and "game=false" in result.stdout, "bounded shell presents and shuts down without a game session")
        result = run([shell, "--run-ms", "30"], env=env, timeout=15)
        check("window=closed" in result.stdout, "monotonic bounded shell lifetime exits")
        for arguments in (("--frames", "0"), ("--run-ms", "0"), ("--unknown",)):
            result = subprocess.run([str(shell), *arguments], cwd=ROOT, env=env, capture_output=True, text=True, timeout=15)
            check(result.returncode == 2 and "window=created" not in result.stdout, f"invalid arguments rejected: {arguments}")
        invalid = dict(env, SDL_VIDEODRIVER="kisakcod-test-driver-unavailable")
        result = subprocess.run([str(shell), "--frames", "1"], cwd=ROOT, env=invalid, capture_output=True, text=True, timeout=15)
        check(result.returncode == 1 and "SDL video initialization" in result.stderr, "missing video driver reports real SDL failure")
        result = run([shell, "--help"], env=invalid, timeout=15)
        check("no game session" in result.stdout, "help does not initialize SDL video")
    finally:
        check(fingerprint(dedicated) == before, "dedicated binary path unchanged")
    print(f"-- {checks} checks, 0 failures --")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
