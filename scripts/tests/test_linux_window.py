#!/usr/bin/env python3
"""Build/check the opt-in real SDL window host; dummy tests do not prove OS input.

The window host builds against either SDL release and the two are selected
explicitly:

    --sdl-major 2 --sdl2-dir <cmake package dir>     # default, unchanged
    --sdl-major 3 --sdl3-dir <cmake package dir>     # SDL3 prototype

Both releases configure the same target matrix -- the release selects which SDL
library those targets build and link against, not which tests exist -- so the
targets and their minimum check counts are written down once, below, instead of
being repeated per release where one list could drift from another. SDL2 keeps
the historical check set and count; the SDL3 configuration adds the
release-specific gates that only make sense there (configure selection, the
KISAK_LINUX_SDL_MAJOR define reaching every monitored translation unit, and the
linked library being SDL3). Nothing below is relaxed for SDL3 and no SDL2 check
is dropped for it.

SDL3 renamed the video and audio driver hints, so the driver this checker asks
for is exported under the names of the selected release.

    python3 scripts/tests/test_linux_window.py --build-dir build-linux/window \\
        --sdl2-dir /usr/lib/i386-linux-gnu/cmake/SDL2
    python3 scripts/tests/test_linux_window.py --build-dir /tmp/kisak-sdl3-run \\
        --sdl-major 3 --sdl3-dir /opt/sdl3/lib/cmake/SDL3

A driver that cannot be created is a failure, not a skip: the checker asks for
one explicit driver and expects the host to report it back.
"""
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
from typing import NamedTuple, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]

SDL2, SDL3 = 2, 3

# The driver hints each release reads. SDL3 renamed both; the legacy SDL2
# spellings are still honoured by SDL3, so they are set alongside.
VIDEO_AUDIO_HINTS = {
    SDL2: ("SDL_VIDEODRIVER", "SDL_AUDIODRIVER"),
    SDL3: ("SDL_VIDEO_DRIVER", "SDL_AUDIO_DRIVER"),
}
ALL_DRIVER_HINTS = ("SDL_VIDEODRIVER", "SDL_AUDIODRIVER", "SDL_VIDEO_DRIVER", "SDL_AUDIO_DRIVER")


class Suite(NamedTuple):
    """One test executable in the checked matrix.

    ``target`` is the CMake target and, once built, the binary name. A suite
    with a ``minimum`` is built and run here, and must print at least that many
    checks with none failed; a suite without one is built here and exercised by
    the explicit command checks in ``main`` instead.

    ``sdl3_minimum`` states the SDL3 floor where it differs from the SDL2 one,
    because a ported fixture can cover a different number of cases in each
    release. It is a floor, not an equality: a suite that gains checks still
    passes, so no release has to be pinned to a count someone else is editing.
    """

    target: str
    label: str
    minimum: Optional[int] = None
    options: Tuple[str, ...] = ()
    summary: str = "-- "
    sdl3_minimum: Optional[int] = None

    def floor(self, major):
        return self.sdl3_minimum if major == SDL3 and self.sdl3_minimum is not None else self.minimum


# Every target this checker builds, and the minimum every summary line it parses
# must reach. Keep an executable's entry here rather than in the build command,
# so a target cannot be built in one place and silently stop being run here.
#
# ``summary`` is the literal text the executable prints immediately before its
# count, including the separator (``"-- "`` or ``"label: "``), so the parsed text
# is the one the fixture actually writes.
SUITES: Tuple[Suite, ...] = (
    # The real SDL event queue and engine interpretation. This one also reports
    # the driver it actually created, which is what proves the environment
    # below reached the window host.
    Suite("window-test", "real SDL event/engine queue suite passes", 54),
    Suite("client-input-test", "actual client input lifecycle suite passes", 54),
    Suite("client-command-test", "actual command lifecycle suite passes", 28),
    Suite("client-runtime-test", "actual runtime lifecycle suite passes", 42),
    Suite("client-input-startup-test", "actual input-startup lifecycle suite passes", 23),
    Suite("string-owner-test", "actual string-owner ownership suite passes", 9,
          summary="string owner: "),
    Suite("string-consumers-test", "actual string-consumers ownership suite passes", 51,
          summary="string consumers: "),
    Suite("dvar-string-failure-test",
          "actual dvar-string-failure --name ownership suite passes", 11,
          options=("--name",), summary="dvar string failure: "),
    Suite("dvar-string-failure-test",
          "actual dvar-string-failure --value ownership suite passes", 11,
          options=("--value",), summary="dvar string failure: "),
    # Software presentation, renderer and UI composition fixtures.
    Suite("presentation-test", "actual presentation suite passes", 27),
    Suite("frame-deadline-test", "actual frame-deadline suite passes", 14),
    Suite("pixel-resource-test", "actual pixel-resource suite passes", 20),
    Suite("cpu-renderer-test", "actual cpu-renderer suite passes", 29),
    Suite("cpu-renderer-fault-test", "actual cpu-renderer-fault suite passes", 33),
    Suite("cpu-raw-test", "actual cpu-raw suite passes", 16),
    Suite("material-clone-mp-test", "actual material-clone-mp suite passes", 10),
    Suite("material-clone-radiant-test", "actual material-clone-radiant suite passes", 10),
    Suite("cpu-ui-test", "actual cpu-ui suite passes", 40),
    Suite("cpu-viewport-test", "actual cpu-viewport suite passes", 26),
    Suite("ui-time-test", "actual ui-time suite passes", 20),
    Suite("ui-registration-test", "actual ui-registration suite passes", 27),
    Suite("cpu-composition-test", "actual cpu-composition suite passes", 20),
    Suite("cpu-traversal-test", "actual cpu-traversal suite passes", 20),
    Suite("cpu-menu-frame-test", "actual cpu-menu-frame suite passes", 23),
    Suite("cpu-frame-owner-test", "actual cpu-frame-owner suite passes", 22),
    Suite("material-layered-mp-test", "actual material-layered-mp suite passes", 19,
          summary="layered material: "),
    Suite("material-layered-sp-test", "actual material-layered-sp suite passes", 19,
          summary="layered material: "),
    Suite("material-layered-radiant-test", "actual material-layered-radiant suite passes", 20,
          summary="layered material: "),
    Suite("cpu-text-test", "actual cpu-text suite passes", 22),
    Suite("cpu-menu-test", "actual cpu-menu suite passes", 25),
    Suite("menu-assets-test", "actual menu-assets suite passes", 11),
    Suite("ui-material-assets-test", "actual ui-material-assets suite passes", 19),
    Suite("menu-composition-assets-test", "actual menu-composition-assets suite passes", 13),
    Suite("iwi-assets-test", "actual iwi-assets suite passes", 28, summary="IWI decoder: "),
    Suite("font-assets-test", "actual font-assets suite passes", 10),
    Suite("menu-control-assets-test", "actual menu-control-assets suite passes", 14),
    Suite("menu-script-test", "actual menu-script suite passes", 31),
    Suite("ui-audio-test", "actual ui-audio suite passes", 20, summary="UI audio: "),
    Suite("ui-audio-failure-test", "actual ui-audio-failure suite passes", 15,
          summary="UI audio failure: "),
    Suite("ui-audio-assets-test", "actual ui-audio-assets suite passes", 30,
          summary="UI audio assets: "),
    # Built here, driven by the explicit command lines further down instead of a
    # summary line: the two programs, the signal fixture, and the menu faults.
    Suite("client-window", "bounded shell fixture builds"),
    Suite("client-input", "input host fixture builds"),
    Suite("shutdown-signal-test", "signal lifecycle fixture builds"),
    # Genuine menu owner fixture: it needs private assets and is run separately
    # with an explicit bundle and image directory, so only its build is checked.
    Suite("menu-input-failure-test", "genuine menu input fault fixture builds"),
    Suite("menu-input-owner-test", "genuine menu owner fixture builds"),
)

# Production translation units the window targets must compile, and the target
# each one is checked in. The first entry of every pair is a path suffix of the
# translation unit; the second is the CMake target that compiles it.
COMPILED: Tuple[Tuple[str, str], ...] = (
    ("window/window_host.cpp", "KisakCOD-window-platform"),
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
    ("linux/client/discovery_session.cpp", "KisakCOD-client-input"),
)

# The material implementation compiled once per game variant, so the editor and
# both games are checked against the same source with their own defines.
MATERIAL_FAMILIES: Tuple[Tuple[str, str], ...] = (
    ("clone", "mp"),
    ("clone", "radiant"),
    ("layered", "mp"),
    ("layered", "sp"),
    ("layered", "radiant"),
)


def build_targets() -> Tuple[str, ...]:
    """Every CMake target to build, in the order its suite is written above."""
    return tuple(dict.fromkeys(f"KisakCOD-{suite.target}" for suite in SUITES))


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def run(command, *, env=None, timeout=1800):
    result = subprocess.run([str(part) for part in command], cwd=ROOT, env=env,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{result.stdout[-5000:]}\n{result.stderr[-5000:]}")
    return result


def sdl_environment(major, video, audio):
    """Environment for one run, with the driver hints that release reads.

    A hint inherited for the other release is removed rather than left to compete
    with the explicit choice, so "which driver ran" has exactly one answer.
    """
    environment = dict(os.environ)
    for name in ALL_DRIVER_HINTS:
        environment.pop(name, None)
    video_hint, audio_hint = VIDEO_AUDIO_HINTS[major]
    if video:
        environment[video_hint] = video
    if audio:
        environment[audio_hint] = audio
    if major == SDL3:
        # SDL3 still honours the SDL2 spellings for programs that set them by
        # their old names; pin them to the same value instead of leaving them
        # to disagree.
        if video:
            environment["SDL_VIDEODRIVER"] = video
        if audio:
            environment["SDL_AUDIODRIVER"] = audio
    return environment


def linked_libraries(build, name):
    """Text describing what ``name`` links, from the binary and the link record.

    Both are kept: the binary's NEEDED entries show what will actually be loaded
    at run time, and the recorded link line shows the library even when it was
    linked statically.
    """
    evidence = ""
    binary = build / "bin" / name
    if binary.exists():
        result = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True,
                                timeout=120)
        evidence += result.stdout
    link_record = build / "CMakeFiles" / f"{name}.dir" / "link.txt"
    if link_record.exists():
        evidence += link_record.read_text()
    return evidence


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--build-type", default="Debug")
    parser.add_argument("--sdl-major", type=int, choices=(SDL2, SDL3), default=SDL2,
                        help="SDL release the window host builds against (default: 2)")
    parser.add_argument("--sdl2-dir", type=Path, help="SDL2 CMake package directory for --sdl-major 2")
    parser.add_argument("--sdl3-dir", type=Path, help="SDL3 CMake package directory for --sdl-major 3")
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--video-driver", default="dummy", help="explicit test driver; X11 also needs DISPLAY")
    args = parser.parse_args()

    # Each release is asked for explicitly: an accidental SDL2 build standing in
    # for a requested SDL3 one would test the wrong library while passing.
    if args.sdl_major == SDL3:
        if not args.sdl3_dir:
            parser.error("--sdl-major 3 needs --sdl3-dir: the prototype does not fall back to an installed SDL3")
        if args.sdl2_dir:
            parser.error("--sdl2-dir selects the SDL2 package; use --sdl3-dir with --sdl-major 3")
    elif args.sdl3_dir:
        parser.error("--sdl3-dir selects the SDL3 package; use --sdl-major 3 with --sdl3-dir")
    sdk_dir = args.sdl3_dir if args.sdl_major == SDL3 else args.sdl2_dir
    if sdk_dir and not sdk_dir.is_dir():
        parser.error(f"not a CMake package directory: {sdk_dir}")

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
        release = text[text.index("set(KISAK_LINUX_SDL_MAJOR"):]
        # Both defaults are one verdict because they are one opt-in: the host is
        # off, and the release it would build is the historical SDL2. SDL2 runs
        # keep the check count CI recorded for them, so a new assertion joins
        # this check rather than starting a second one.
        check(bool(re.search(r"\bOFF\)", option[:250]))
              and bool(re.search(r"set\(KISAK_LINUX_SDL_MAJOR 2 ", release[:80])),
              "window host defaults OFF and selects SDL2")
        command = ["cmake", "-S", ROOT, "-B", build, "-DKISAK_PLATFORM=linux",
                   "-DKISAK_BUILD_LINUX_WINDOW_HOST=ON",
                   f"-DKISAK_LINUX_SDL_MAJOR={args.sdl_major}",
                   f"-DCMAKE_BUILD_TYPE={args.build_type}",
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={build / 'bin'}"]
        if sdk_dir:
            command.append(f"-D{'SDL3_DIR' if args.sdl_major == SDL3 else 'SDL2_DIR'}={sdk_dir.resolve()}")
        for key, value in (("CMAKE_C_COMPILER", args.cc), ("CMAKE_CXX_COMPILER", args.cxx)):
            if value:
                command.append(f"-D{key}={value}")
        run(command)
        entries = json.loads((build / "compile_commands.json").read_text())
        compiled_commands = {}
        for suffix, target in COMPILED:
            commands = [entry["command"] for entry in entries if entry["file"].endswith(suffix)
                        and target + ".dir" in entry["command"]]
            check(len(commands) == 1, f"actual production compile entry: {target}/{suffix}")
            command = commands[0]
            compiled_commands[(suffix, target)] = command
            for flag in ("-m32", "-DKISAK_MP", "-DRELEASE_ASSERTS"):
                check(bool(re.search(r"(?:^|\s)" + re.escape(flag) + r"(?:\s|$)", command)), f"{suffix} uses {flag}")
            check("-DKISAK_DEDICATED" not in command and "-DDEDICATED" not in command, f"{suffix} is a client compile")
        for family, variant in MATERIAL_FAMILIES:
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
        command = ["cmake", "--build", build, "--target", *build_targets(), "-j", str(args.jobs)]
        run(command)
        run(command)
        check(True, "window targets build and incrementally rebuild")

        # SDL3-only gates. The SDL2 run keeps the historical check set, so a
        # release-specific assertion is added here rather than folded into the
        # shared checks above, where it would also change the SDL2 count.
        if args.sdl_major == SDL3:
            cache = (build / "CMakeCache.txt").read_text()
            check(re.search(r"^KISAK_LINUX_SDL_MAJOR:STRING=3$", cache, re.MULTILINE) is not None,
                  "configure selects the SDL3 prototype release")
            for (suffix, target), command in compiled_commands.items():
                check(bool(re.search(r"(?:^|\s)-DKISAK_LINUX_SDL_MAJOR=3(?:\s|$)", command)),
                      f"{suffix} is compiled for the selected SDL3 release")
            linkage = linked_libraries(build, "KisakCOD-window-test")
            check("libSDL3" in linkage and "libSDL2" not in linkage,
                  "window targets link the selected SDL3 release and no SDL2")

        env = sdl_environment(args.sdl_major, args.video_driver, "dummy")
        for suite in SUITES:
            if suite.minimum is None:
                continue
            result = run([build / "bin" / f"KisakCOD-{suite.target}", *suite.options], env=env, timeout=30)
            print(result.stdout, end="")
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            summary = re.search(re.escape(suite.summary) + r"(\d+) checks, (\d+) failures", result.stdout)
            check(summary and int(summary[1]) >= suite.floor(args.sdl_major) and int(summary[2]) == 0,
                  suite.label)
            if suite.target == "window-test":
                check(f"SDL driver={args.video_driver}" in result.stdout,
                      "test reports the explicitly selected SDL driver")
        result = run([build / "bin/KisakCOD-ui-audio-failure-test", "--disk"], env=env, timeout=30)
        print(result.stdout, end="")
        # The disk-output comparison reports its own checks; the floor keeps the
        # comparison in the run without pinning the total another worker edits.
        summary = re.search(r"(\d+) checks, (\d+) failures", result.stdout)
        check(summary and int(summary[1]) >= 4 and int(summary[2]) == 0,
              "real SDL disk callback output matches exact scaled synthetic PCM (not audibility)")
        result = run([build / "bin/KisakCOD-ui-audio-test", "--unavailable"], env=env, timeout=30)
        summary = re.search(r"(\d+) checks, (\d+) failures", result.stdout)
        check(summary and int(summary[1]) >= 1 and int(summary[2]) == 0,
              "unavailable audio driver rejects explicitly")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_input_prepare.py"], timeout=30)
        check(result.returncode == 0, "fresh exporter-schema control preparation preserves strict script roles and bounds")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_composition_prepare.py"], timeout=30)
        check(result.returncode == 0, "composition preparer validates actual literal resolution and unsupported metadata")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_menu_reference_prepare.py"], timeout=30)
        check(result.returncode == 0, "pixel reference selects unique genuine metadata without external profiles")
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_ui_audio_prepare.py"], timeout=30)
        print(result.stderr, end="")
        check("Ran 5 tests" in result.stderr and "OK" in result.stderr, "audio metadata preparation rejects unsupported profiles and existing output")
        result = run([build / "bin/KisakCOD-shutdown-signal-test"], env=env, timeout=30)
        check("Shutdown signal lifecycle: 0 failures" in result.stdout, "signal lifecycle restores handlers and retains first concurrent request")
        result = run([build / "bin/KisakCOD-client-input", "--frames", "3", "--trace"], env=env, timeout=15)
        check("presentation=engine-ui-image" in result.stdout and "presented=4" in result.stdout
              and "frame=unavailable" in result.stdout, "existing input host presents bounded actual UI image frames")
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
        # A driver that does not exist must be reported by SDL, not replaced by a
        # working default: the explicit choice above is what the tests rely on.
        invalid = sdl_environment(args.sdl_major, "kisakcod-test-driver-unavailable", "dummy")
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
