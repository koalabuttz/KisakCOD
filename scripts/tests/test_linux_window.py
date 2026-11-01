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
import struct
import tempfile

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
                               ("client/cl_ui_draw.cpp", "KisakCOD-cpu-viewport-test"),
                               ("gfx_d3d/r_viewport.cpp", "KisakCOD-cpu-viewport-test"),
                               ("linux/sys_events.cpp", "KisakCOD-client-window"),
                               ("linux/sys_critical.cpp", "KisakCOD-client-window"),
                               ("linux/sys_posix.cpp", "KisakCOD-client-window"),
                               ("tests/linux_window/window_test.cpp", "KisakCOD-window-test"),
                               ("client/cl_keys.cpp", "KisakCOD-client-input"),
                               ("client_mp/cl_input.cpp", "KisakCOD-client-input"),
                               ("qcommon/field.cpp", "KisakCOD-client-input"),
                               ("ui/ui_input_core.cpp", "KisakCOD-client-input"),
                               ("ui/ui_script_core.cpp", "KisakCOD-client-input"),
                               ("ui/ui_item_text.cpp", "KisakCOD-client-input"),
                               ("window/ui_audio.cpp", "KisakCOD-client-input"),
                               ("window/menu_input_owner.cpp", "KisakCOD-client-input"),
                               ("qcommon/cmd.cpp", "KisakCOD-client-input"),
                               ("universal/dvar.cpp", "KisakCOD-client-input"),
                               ("linux/net.cpp", "KisakCOD-client-input"),
                               ("linux/client/discovery_session.cpp", "KisakCOD-client-input")):
            commands = [entry["command"] for entry in entries if entry["file"].endswith(suffix)
                        and target + ".dir" in entry["command"]]
            check(len(commands) == 1, f"actual production compile entry: {target}/{suffix}")
            command = commands[0]
            for flag in ("-m32", "-DKISAK_MP", "-DRELEASE_ASSERTS"):
                check(bool(re.search(r"(?:^|\s)" + re.escape(flag) + r"(?:\s|$)", command)), f"{suffix} uses {flag}")
            check("-DKISAK_DEDICATED" not in command and "-DDEDICATED" not in command, f"{suffix} is a client compile")
        for family, variant in (("clone", "mp"), ("clone", "radiant"), ("layered", "mp"), ("layered", "sp"), ("layered", "radiant")):
            target = f"KisakCOD-material-{family}-{variant}-test"
            commands = [entry["command"] for entry in entries
                        if entry["file"].endswith(f"gfx_d3d/r_material_{family}.cpp") and target + ".dir" in entry["command"]]
            check(len(commands) == 1, f"actual production material {family} compiled for {variant}")
            command = commands[0]
            if variant == "radiant":
                check("-DKISAK_RADIANT" in command and "-DMAX_GENTITIES=2176" in command,
                      f"Radiant material {family} uses actual editor defines")
                check("-DKISAK_MP" not in command and "-DKISAK_SP" not in command,
                      f"Radiant material {family} excludes game defines")
            else:
                own, other = ("MP", "SP") if variant == "mp" else ("SP", "MP")
                check(f"-DKISAK_{own}" in command, f"{own} material {family} uses its game definition")
                check("-DKISAK_RADIANT" not in command and f"-DKISAK_{other}" not in command,
                      f"{own} material {family} excludes other variant defines")
        command = ["cmake", "--build", build, "--target", "KisakCOD-client-window", "KisakCOD-window-test", "KisakCOD-client-input", "KisakCOD-client-input-test", "KisakCOD-shutdown-signal-test", "KisakCOD-client-command-test", "KisakCOD-client-runtime-test", "KisakCOD-string-owner-test", "KisakCOD-string-consumers-test", "KisakCOD-dvar-string-failure-test", "KisakCOD-client-input-startup-test", "KisakCOD-presentation-test", "KisakCOD-frame-deadline-test", "KisakCOD-pixel-resource-test", "KisakCOD-cpu-renderer-test", "KisakCOD-cpu-renderer-fault-test", "KisakCOD-cpu-raw-test", "KisakCOD-material-clone-mp-test", "KisakCOD-material-clone-radiant-test", "KisakCOD-cpu-ui-test", "KisakCOD-material-layered-mp-test", "KisakCOD-material-layered-sp-test", "KisakCOD-material-layered-radiant-test", "KisakCOD-cpu-text-test", "KisakCOD-cpu-menu-test", "KisakCOD-menu-assets-test", "KisakCOD-ui-material-assets-test", "KisakCOD-menu-composition-assets-test", "KisakCOD-iwi-assets-test", "KisakCOD-font-assets-test", "KisakCOD-menu-input-owner-test", "KisakCOD-menu-control-assets-test", "KisakCOD-menu-script-test", "KisakCOD-ui-audio-test", "KisakCOD-ui-audio-assets-test", "KisakCOD-ui-audio-failure-test", "KisakCOD-menu-input-failure-test", "KisakCOD-cpu-viewport-test", "KisakCOD-ui-time-test", "KisakCOD-ui-registration-test", "KisakCOD-cpu-composition-test", "KisakCOD-cpu-traversal-test", "KisakCOD-cpu-menu-frame-test", "KisakCOD-cpu-frame-owner-test", "-j", args.jobs]
        run(command)
        run(command)
        check(True, "window targets build and incrementally rebuild")
        env = dict(os.environ, SDL_VIDEODRIVER=args.video_driver, SDL_AUDIODRIVER="dummy")
        result = run([build / "bin/KisakCOD-window-test"], env=env, timeout=30)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        summary = re.search(r"(?:-- |layered material: |IWI decoder: |UI audio: |UI audio failure: |UI audio assets: )(\d+) checks, (\d+) failures", result.stdout)
        check(summary and int(summary[1]) >= 54 and int(summary[2]) == 0, "real SDL event/engine queue suite passes")
        check(f"SDL driver={args.video_driver}" in result.stdout, "test reports the explicitly selected SDL driver")
        result = run([build / "bin/KisakCOD-client-input-test"], env=env, timeout=30)
        print(result.stdout, end="")
        summary = re.search(r"(?:-- |layered material: |IWI decoder: |UI audio: |UI audio failure: |UI audio assets: )(\d+) checks, (\d+) failures", result.stdout)
        check(summary and int(summary[1]) >= 54 and int(summary[2]) == 0, "actual client input lifecycle suite passes")
        for name, minimum in (("command", 28), ("runtime", 42), ("input-startup", 23)):
            result = run([build / f"bin/KisakCOD-client-{name}-test"], env=env, timeout=30)
            print(result.stdout, end="")
            summary = re.search(r"(?:-- |layered material: |IWI decoder: |UI audio: |UI audio failure: |UI audio assets: )(\d+) checks, (\d+) failures", result.stdout)
            check(summary and int(summary[1]) >= minimum and int(summary[2]) == 0,
                  f"actual {name} lifecycle suite passes")
        for name, options, minimum in (("string-owner", [], 9), ("string-consumers", [], 51),
                                       ("dvar-string-failure", ["--name"], 11),
                                       ("dvar-string-failure", ["--value"], 11)):
            result = run([build / f"bin/KisakCOD-{name}-test", *options], env=env, timeout=30)
            print(result.stdout, end="")
            summary = re.search(r"(?:string owner|dvar string failure|string consumers): (\d+) checks, (\d+) failures", result.stdout)
            check(summary and int(summary[1]) >= minimum and int(summary[2]) == 0,
                  f"actual {name} {' '.join(options)} ownership suite passes")
        for name, minimum in (("presentation", 27), ("frame-deadline", 14), ("pixel-resource", 20), ("cpu-renderer", 29), ("cpu-renderer-fault", 33), ("cpu-raw", 16), ("material-clone-mp", 10), ("material-clone-radiant", 10), ("cpu-ui", 40), ("cpu-viewport", 26), ("ui-time", 20), ("ui-registration", 27), ("cpu-composition", 20), ("cpu-traversal", 20), ("cpu-menu-frame", 23), ("cpu-frame-owner", 22), ("material-layered-mp", 19), ("material-layered-sp", 19), ("material-layered-radiant", 20), ("cpu-text", 22), ("cpu-menu", 25), ("menu-assets", 11), ("ui-material-assets", 19), ("menu-composition-assets", 13), ("iwi-assets", 28), ("font-assets", 10), ("menu-control-assets", 14), ("menu-script", 31), ("ui-audio", 20), ("ui-audio-failure", 15), ("ui-audio-assets", 30)):
            result = run([build / f"bin/KisakCOD-{name}-test"], env=env, timeout=30)
            print(result.stdout, end="")
            summary = re.search(r"(?:-- |layered material: |IWI decoder: |UI audio: |UI audio failure: |UI audio assets: )(\d+) checks, (\d+) failures", result.stdout)
            check(summary and int(summary[1]) >= minimum and int(summary[2]) == 0,
                  f"actual {name} suite passes")
        result = run([build / "bin/KisakCOD-client-input", "--frames", "3", "--trace"], env=env, timeout=15)
        check("presentation=engine-ui-image" in result.stdout and "presented=4" in result.stdout
              and "frame=unavailable" in result.stdout, "existing input host presents bounded actual UI image frames")
        result = run([build / "bin/KisakCOD-ui-audio-failure-test", "--disk"], env=env, timeout=30)
        print(result.stdout, end="")
        check("4 checks, 0 failures" in result.stdout, "real SDL disk callback output matches exact scaled synthetic PCM (not audibility)")
        result = run([build / "bin/KisakCOD-ui-audio-test", "--unavailable"], env=env, timeout=30)
        check("1 checks, 0 failures" in result.stdout, "unavailable audio driver rejects explicitly")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_input_prepare.py"], timeout=30)
        check(result.returncode == 0, "fresh exporter-schema control preparation preserves strict script roles and bounds")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_composition_prepare.py"], timeout=30)
        check(result.returncode == 0, "composition preparer validates actual literal resolution and unsupported metadata")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_reference_prepare.py"], timeout=30)
        check(result.returncode == 0, "pixel reference selects unique genuine metadata without external profiles")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_ui_audio_prepare.py"], timeout=30)
        print(result.stderr, end="")
        check("Ran 5 tests" in result.stderr and "OK" in result.stderr, "audio metadata preparation rejects unsupported profiles and existing output")
        # Genuine menu owner fixture is built above but requires private assets;
        # run it separately with an explicit bundle/image directory.
        result = run([build / "bin/KisakCOD-shutdown-signal-test"], env=env, timeout=30)
        check("Shutdown signal lifecycle: 0 failures" in result.stdout, "signal lifecycle restores handlers and retains first concurrent request")
        result = subprocess.run([str(build / "bin/KisakCOD-client-input"), "--frame"], env=env,
                                capture_output=True, text=True, timeout=15)
        check(result.returncode == 5 and "frame=unavailable" in result.stderr and "window=created" not in result.stdout,
              "unavailable game frame explicitly rejected")
        with tempfile.TemporaryDirectory(prefix="kisak-ui-assets-") as temporary:
            directory = Path(temporary)
            missing = directory / "missing"
            bad_iwi = directory / "unsupported.iwi"
            bad_iwi.write_bytes(struct.pack("<3sBBBHHH4I", b"IWi", 6, 255, 0, 4, 4, 1, 28, 28, 28, 28))
            bad_font = directory / "invalid.font"
            bad_font.write_bytes(bytes(16))
            font_name, material_name = b"checker-font\0", b"checker-material\0"
            glyphs = b"".join(struct.pack("<HbbBBBx4f", letter, 0, 0, 1, 1, 1, 0, 0, 1, 1) for letter in range(32, 128))
            names_start = 16 + len(glyphs)
            valid_font = directory / "synthetic-parser-only.font"
            valid_font.write_bytes(struct.pack("<4I", names_start, 48, 96, names_start + len(font_name)) + glyphs + font_name + material_name)
            for arguments, label in (
                (("--ui-image", missing), "missing explicit UI image"),
                (("--ui-image", bad_iwi), "unsupported explicit UI image"),
                (("--font-file", missing, "--font-atlas", missing), "missing explicit font"),
                (("--font-file", bad_font, "--font-atlas", bad_iwi), "invalid explicit font"),
                (("--font-file", valid_font, "--font-atlas", bad_iwi), "unsupported explicit font atlas"),
                (("--font-file", valid_font, "--font-atlas", missing), "missing explicit font atlas"),
            ):
                result = subprocess.run([str(build / "bin/KisakCOD-client-input"), *map(str, arguments), "--frames", "1"],
                                        env=env, capture_output=True, text=True, timeout=15)
                check(result.returncode == 1 and "window=ready" not in result.stdout and "Window host failed" in result.stderr,
                      label + " fails without visible fallback")
            result = subprocess.run([str(build / "bin/KisakCOD-client-input"), "--font-file", str(missing)],
                                    env=env, capture_output=True, text=True, timeout=15)
            check(result.returncode == 2 and "window=ready" not in result.stdout, "font and atlas must be supplied together")
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
