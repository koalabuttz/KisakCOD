#!/usr/bin/env python3
"""Compile and run the asset-free free-camera checks for one configuration.

Like ``run_world_surface_checks.py`` this performs **no CMake configure and no
engine build**: it compiles two standalone tests with an explicit 32-bit compiler
line and runs them. Nothing needs DXVK, SDL, a GPU, a window, a Steam client,
credentials, game assets or a network.

  * ``tests/linux_free_camera/free_camera_test.cpp`` -- camera pose, mouse and key
    turning, movement, projection, refdef filling and key bindings;
  * ``tests/linux_free_camera/frame_owner_test.cpp`` -- frame ordering against a
    recording renderer: resize/minimize, device loss and reset, load/reload/unload,
    shutdown (also deferred and repeated), fail-closed paths, focus and pointer
    capture, two scripted reference traversals and a seeded randomized lifecycle.

Production source compiled unmodified
-------------------------------------
Both tests link ``src/universal/com_math_anglevectors.cpp`` so the camera axis is
the engine's own AnglesToAxis/AngleVectors, not a transcription. That unit is
compiled to an object first with the production Linux flags only (the same
``-fms-extensions -msse2 -mfpmath=sse`` set and ``KISAK_MP`` the client targets
use) and without this runner's warning set: its warnings belong to the original
source, which is compiled as it is rather than edited. The unit also defines
AnglesToQuat, which needs AxisToQuat from the much larger com_math.cpp; the link
uses ``-ffunction-sections`` and ``--gc-sections`` exactly as the production Linux
targets do, so the unreferenced function is discarded instead of pulling in more
engine code.

The new free-camera sources and the tests are compiled with ``-Wall -Wextra
-Werror`` on top of the same production flags.

The default compiler is ``clang++`` because the Linux engine build requires Clang
for its Microsoft extensions; ``--cxx`` overrides it.

What this runner does not claim
-------------------------------
A pass says the camera arithmetic and the frame owner's ordering behave as the
tests state against a fake renderer. It does not claim that any map was loaded or
drawn, that a real device was reset, or that the hosted workflow has run.

Output, timeouts, process groups, the ELF32 check and the FAIL-line rule are the
world-surface runner's, imported from it, so both runners fail the same way.

Usage:
    python3 scripts/tests/run_free_camera_checks.py --config Debug
    python3 scripts/tests/run_free_camera_checks.py --config RelWithDebInfo \\
        --log-dir artifacts/free-camera-RelWithDebInfo
    python3 scripts/tests/run_free_camera_checks.py --list
"""

import argparse
from importlib import util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def _load_shared():
    """The world-surface runner's process, ELF and summary helpers."""
    path = Path(__file__).resolve().with_name("run_world_surface_checks.py")
    spec = util.spec_from_file_location("kisakcod_run_world_surface_checks", path)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


shared = _load_shared()
Command = shared.Command
Check = shared.Check
CONFIG_FLAGS = shared.CONFIG_FLAGS
TARGET_BITS = shared.TARGET_BITS

# The production Linux client-target flags (scripts/platform/linux/CMakeLists.txt),
# without the include roots, which are added explicitly below.
PRODUCTION_FLAGS = (
    "-m32",
    "-std=c++20",
    "-g",
    "-fms-extensions",
    "-msse2",
    "-mfpmath=sse",
    "-fno-strict-aliasing",
    "-ffunction-sections",
    "-fdata-sections",
    "-DKISAK_MP",
    "-D_FILE_OFFSET_BITS=64",
    "-D_GNU_SOURCE",
    "-Wno-c++11-narrowing",
    "-Wno-invalid-token-paste",
    "-Wno-ignored-attributes",
    "-Wno-format-security",
)

# Only on the new sources and tests: a warning there is a defect in this change.
STRICT_FLAGS = ("-Wall", "-Wextra", "-Werror")

LINK_FLAGS = ("-Wl,--gc-sections", "-pthread")

INCLUDE_DIRS = ("src", "tests/linux_free_camera")

PRODUCTION_UNIT = "src/universal/com_math_anglevectors.cpp"
PRODUCTION_OBJECT = "com_math_anglevectors.o"
OBJECT_CHECK = "production_math_object"

CAMERA_SOURCES = (
    "src/linux/client/view/free_camera.cpp",
    "src/linux/client/view/free_camera_controls.cpp",
)


class Case:
    def __init__(self, name, description, source, sources):
        self.name = name
        self.description = description
        self.source = source
        self.sources = (source, *sources)


CASES = (
    Case(
        "free_camera",
        "camera pose, turning, movement, projection, refdef fill and bindings",
        "tests/linux_free_camera/free_camera_test.cpp",
        CAMERA_SOURCES,
    ),
    Case(
        "frame_owner",
        "frame order: resize, reset, reload, shutdown, fail-closed, input, traversals",
        "tests/linux_free_camera/frame_owner_test.cpp",
        ("src/linux/client/view/frame_owner.cpp", *CAMERA_SOURCES),
    ),
)

KNOWN_NAMES = tuple(case.name for case in CASES)


def include_arguments(repo):
    arguments = []
    for directory in INCLUDE_DIRS:
        arguments.extend(("-I", str(repo / directory)))
    return arguments


def production_object_argv(repo, workdir, cxx, config, extra_flags):
    """The unmodified production unit, production flags only, compiled to an object."""
    return [
        cxx,
        *PRODUCTION_FLAGS,
        *CONFIG_FLAGS[config],
        *extra_flags,
        *include_arguments(repo),
        "-c",
        str(repo / PRODUCTION_UNIT),
        "-o",
        str(workdir / PRODUCTION_OBJECT),
    ]


def compile_argv(case, repo, workdir, cxx, config, extra_flags):
    """The exact compile-and-link line for one case."""
    return [
        cxx,
        *PRODUCTION_FLAGS,
        *STRICT_FLAGS,
        *CONFIG_FLAGS[config],
        *extra_flags,
        *include_arguments(repo),
        *[str(repo / source) for source in case.sources],
        str(workdir / PRODUCTION_OBJECT),
        *LINK_FLAGS,
        "-o",
        str(workdir / case.name),
    ]


def plan_checks(options, workdir, repo, cxx, selected):
    """The production object first (whenever any case runs), then compile+run per case.

    The object is its own check so its log is kept separately and a failure in the
    original source is reported as that, not as a test failure.
    """
    checks = []
    cases = [case for case in CASES if case.name in selected]
    if not cases:
        return checks
    checks.append(
        Check(
            OBJECT_CHECK,
            "production AnglesToAxis/AngleVectors unit, unmodified, production flags",
            [
                Command(
                    production_object_argv(repo, workdir, cxx, options.config, options.cxx_flag),
                    cwd=repo,
                    timeout=options.compile_timeout,
                )
            ],
        )
    )
    for case in cases:
        binary = workdir / case.name
        checks.append(
            Check(
                case.name,
                case.description,
                [
                    Command(
                        compile_argv(case, repo, workdir, cxx, options.config, options.cxx_flag),
                        cwd=repo,
                        timeout=options.compile_timeout,
                    ),
                    Command([binary], cwd=workdir, timeout=options.timeout),
                ],
                binary=binary,
            )
        )
    return checks


def print_plan(checks, repo, config, workdir, log_dir):
    print("Free camera checks: asset-free, no DXVK, no SDL, no device", flush=True)
    print("=" * 64, flush=True)
    print(f"repository:  {repo}", flush=True)
    print(f"config:      {config} ({' '.join(CONFIG_FLAGS[config])})", flush=True)
    print(f"target:      ELF{TARGET_BITS}/i386", flush=True)
    print(f"work dir:    {workdir}", flush=True)
    if log_dir:
        print(f"logs:        {log_dir}", flush=True)
    print(flush=True)
    print(f"{len(checks)} check(s) in this order:", flush=True)
    for index, check in enumerate(checks, start=1):
        print(f"  {index}. {check.name}: {check.description}", flush=True)
        for part, command in enumerate(check.commands, start=1):
            print(f"     {index}.{part} $ {command.describe()}", flush=True)
            if command.cwd != repo:
                print(f"           cwd: {command.cwd}", flush=True)
            print(f"           timeout: {command.timeout}s", flush=True)
    print(flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", choices=sorted(CONFIG_FLAGS), default="Debug",
                        help="Debug is -O0, RelWithDebInfo is -O2 (default: Debug)")
    parser.add_argument("--cxx", default=os.environ.get("KISAK_FREE_CAMERA_CXX", "clang++"),
                        help="32-bit C++ compiler (default: clang++, as the Linux engine build requires)")
    parser.add_argument("--cxx-flag", action="append", default=[], metavar="FLAG",
                        help="extra compiler flag, repeatable; never implied")
    parser.add_argument("--repo-root", type=Path, default=ROOT,
                        help="repository root holding the sources (default: this checkout)")
    parser.add_argument("--work-dir", type=Path,
                        help="directory for objects and binaries (default: a per-run temporary directory)")
    parser.add_argument("--timeout", type=int, default=shared.DEFAULT_RUN_TIMEOUT, help="timeout of each run, in seconds")
    parser.add_argument("--compile-timeout", type=int, default=shared.DEFAULT_COMPILE_TIMEOUT,
                        help="timeout of each compile, in seconds")
    parser.add_argument("--log-dir", type=Path, help="write one text log per command, plus summary.txt")
    parser.add_argument("--only", help="comma-separated check names to run")
    parser.add_argument("--skip", help="comma-separated check names to leave out")
    parser.add_argument("--list", action="store_true", help="print the command list and exit without running")
    options = parser.parse_args(argv)

    options.only = shared.parse_names(options.only)
    options.skip = shared.parse_names(options.skip)
    for name in options.only | options.skip:
        if name not in KNOWN_NAMES:
            parser.error(f"unknown check: {name} (known: {', '.join(KNOWN_NAMES)})")
    if options.timeout <= 0:
        parser.error("--timeout must be positive")
    if options.compile_timeout <= 0:
        parser.error("--compile-timeout must be positive")
    selected = set(options.only) if options.only else set(KNOWN_NAMES)
    selected -= options.skip
    if not selected:
        parser.error("no checks selected: --only/--skip removed every check")

    repo = options.repo_root.expanduser().resolve()
    if not repo.is_dir():
        print(f"FAIL: repository root is not a directory: {repo}", flush=True)
        return 1
    needed = {PRODUCTION_UNIT}
    for case in CASES:
        if case.name in selected:
            needed.update(case.sources)
    for source in sorted(needed):
        if not (repo / source).is_file():
            print(f"FAIL: source is missing: {repo / source}", flush=True)
            return 1
    if not shutil.which(options.cxx) and not Path(options.cxx).is_file():
        print(f"FAIL: compiler not found: {options.cxx}", flush=True)
        print("Install clang and g++-multilib (for the 32-bit runtime) or pass --cxx.", flush=True)
        return 1

    temporary = None
    if options.work_dir:
        workdir = options.work_dir.expanduser().resolve()
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="kisakcod-free-camera-")
        workdir = Path(temporary.name)
    try:
        checks = plan_checks(options, workdir, repo, options.cxx, selected)
        print_plan(checks, repo, options.config, workdir, options.log_dir)
        if options.list:
            print("--list: printed the command list, ran nothing.", flush=True)
            return 0
        started = time.monotonic()
        results, failures = shared.run_checks(checks, options.log_dir)
        print(f"elapsed: {time.monotonic() - started:.1f}s", flush=True)
        return shared.print_summary(results, failures)
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
