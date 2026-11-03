#!/usr/bin/env python3
"""Compile and run the asset-free world surface checks for one configuration.

This runner performs **no CMake configure and no engine build**. It compiles eleven
standalone test translation units directly with a 32-bit C++20 compiler and runs
them, then runs the bundle prepare tests. Everything here works without DXVK, SDL,
a GPU, a Steam client, credentials, game assets or a network:

  * ``tests/linux_asset_export/world_surface_inventory_test.cpp`` -- the inventory
    allowlist, declaration routing, slice bounds and bundle-helper arithmetic, built
    against ``src`` because the header it includes lives there;
  * ``tests/linux_renderer/world_surface_context_test.cpp`` -- the shader-constant
    context arithmetic for one unlit surface draw;
  * ``tests/linux_renderer/world_surface_bundle_test.cpp`` -- the bundle loader, the
    shader framing walk and the vertex accessors;
  * ``tests/linux_renderer/world_surface_oracle_test.cpp`` -- the reference oracle's
    hashing, weights, sampling, colour and pixel arithmetic and its sample selection;
  * ``tests/linux_renderer/world_lit_bundle_test.cpp`` -- the lit bundle loader's
    refusals: the program pair's hash and length, the widened geometry counts and
    index walk, the colour-map sampler and its dependency, the exact constant set, and
    the captured lightmap's layout. It includes ``world_lit_bundle.h``, which reuses
    the reader in ``world_surface_bundle.h`` and adds no graphics header of its own;
  * ``tests/linux_asset_export/world_frontend_invariants_test.cpp`` -- the rules the
    offline world front-end probe holds every frame to (visible cells and surfaces,
    batch keys and stage ranges, completeness, no duplicates) and the entity-string
    scan that picks its spawn-point cameras, built against ``src`` for the one
    header it includes (``src/linux/tools/world_frontend_invariants.h``);
  * ``tests/linux_asset_export/world_device_rules_test.cpp`` -- the rules the world
    device probe holds each texture, shader and vertex/index buffer to, as pure
    functions over plain numbers, built against ``src`` for
    ``src/linux/tools/world_device_rules.h``, which includes no D3D9 header;
  * ``tests/linux_asset_export/world_draw_rules_test.cpp`` -- what the world draw
    probe holds a drawn frame to (the back-buffer measurement, the colour, coverage,
    brightness, repeat and camera rules, and the renderer's own draw counts), on
    frames built in memory, built against ``src`` for
    ``src/linux/tools/world_draw_rules.h``, which includes no engine or D3D9 header;
  * ``tests/linux_asset_export/map_effects_test.cpp`` -- the reader the free camera
    plays a map's own effects from: the script tokenizer, the effect table, the
    createfx list and the plan, on script text it writes itself, built against
    ``src`` for ``src/linux/tools/map_effects.h``, which includes no engine header;
  * ``tests/linux_asset_export/world_image_capture_test.cpp`` -- the image capture's
    bounds, name filter, duplicate rule and disabled path. This one is the only case
    that is not a single translation unit: the capture state lives behind a mutex in
    ``src/linux/tools/world_image_capture.cpp``, so that unit is compiled beside the
    test and linked with ``-pthread``. It is still engine-free -- no engine header, no
    asset tree, no zone -- and no other source is added to the line;
  * ``tests/linux_compat/msvc_rand_test.cpp`` -- the Microsoft runtime's ``rand`` and
    ``srand`` that the world draw probe links in place of glibc's: the runtime's
    sequence, the 15-bit range, per-thread state, and that shared libraries still
    find glibc's. Like the capture case it links its unit,
    ``src/linux/client/msvc_rand.cpp``, beside the test with ``-pthread``, since replacing
    glibc's definitions is a property of the link; no engine header;
  * ``scripts/tests/test_world_surface_prepare.py`` -- the prepare step's trust
    boundary over archives it writes itself.

Why these are compiled directly rather than through
``tests/linux_renderer/CMakeLists.txt``: that project requires ``dxvk-d3d9`` and
``sdl3`` from pkg-config before it configures anything, so it cannot be used for a
check that must not need DXVK, SDL or a device. The four renderer tests
(``world_surface_context``, ``world_surface_bundle``, ``world_surface_oracle`` and
``world_lit_bundle``) are asset-free by construction -- they include one header each
and open no file except temporary bundles they write themselves -- and the image
capture and rand tests compile their own unit and nothing else from ``src``, so all eleven are
built here with an explicit compiler line instead.

Configurations are explicit rather than inherited from a build system: ``Debug``
compiles at ``-O0`` and ``RelWithDebInfo`` at ``-O2``, both with ``-g``. Nothing is
detected and nothing is inferred from a cache.

What this runner does not claim
-------------------------------
A passing run says the helpers behave as their tests state. It does not claim that
any surface was rendered, that a captured frame was reproduced, or that the hosted
workflow has been executed: the workflow invokes this script and is not itself
verified by it.

Each command runs in its own process group with its own timeout, so a stuck check
is killed instead of hanging the job. Every command prints the exact argv it ran,
its working directory, its exit status and its output, and the run ends with one
status line per check: nothing is reported as success unless the process actually
exited 0. Compiled binaries live in a per-run temporary directory that is removed
afterwards, so two concurrent runs cannot collide in ``/tmp``.

Usage:
    python3 scripts/tests/run_world_surface_checks.py --config Debug
    python3 scripts/tests/run_world_surface_checks.py \\
        --config RelWithDebInfo --log-dir artifacts/world-surface-RelWithDebInfo

    python3 scripts/tests/run_world_surface_checks.py --list    # print, run nothing

Requires the multilib toolchain: ``g++ -m32`` must link, which means ``g++`` and
``g++-multilib`` are installed.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]

# Explicit optimization per configuration. These are the only two supported
# configurations, and neither is derived from a CMake cache.
CONFIG_FLAGS = {
    "Debug": ("-O0",),
    "RelWithDebInfo": ("-O2",),
}

# The target is the 32-bit Linux engine, so every check is built as ELF32/i386.
TARGET_BITS = 32
TARGET_MACHINE = 3  # EM_386

COMPILE_FLAGS = (
    "-m32",
    "-std=c++20",
    "-g",
    "-Wall",
    "-Wextra",
    # A warning in a file this job compiles is a defect in the check, not noise.
    "-Werror",
    # world_surface_bundle_test.cpp documents its own compile line with a trailing
    # backslash, which -Wcomment reports inside the comment. The comment is the
    # source's, not this runner's, so the warning is disabled rather than the file
    # edited; every other warning still fails the compile.
    "-Wno-comment",
)

# Each check prints a line containing this word only when a check failed. A test
# that reports a failure and still exits 0 is a broken test, so the run is failed
# here rather than trusted.
FAILURE_MARKER = "FAIL"

DEFAULT_RUN_TIMEOUT = 300
DEFAULT_COMPILE_TIMEOUT = 600


class Case:
    """One standalone C++ test: where it is, and the include roots it needs."""

    def __init__(self, name, description, source, include_dirs, extra_sources=(), flags=()):
        self.name = name
        self.description = description
        self.source = source
        # Only the roots this translation unit actually needs: the inventory test
        # includes <linux/tools/...> from src, the two renderer tests include their
        # own header from the test directory.
        self.include_dirs = include_dirs
        # Translation units compiled and linked beside the test, relative to the
        # repository. Empty for every case that is a single translation unit. The
        # capture and rand tests need exactly one each: their unit, with no engine
        # source beside it.
        self.extra_sources = tuple(extra_sources)
        # Per-case compiler flags. Only the capture and rand cases need one (-pthread,
        # for the std::mutex the capture unit holds and the rand test's threads). A flag here applies to that case alone; nothing
        # is shared, so a flag added for one check cannot change another's line.
        self.flags = tuple(flags)

    @property
    def sources(self):
        """Every source this case compiles, the test itself first."""
        return (self.source, *self.extra_sources)


CASES = (
    Case(
        "world_surface_inventory",
        "inventory allowlist, declaration routing and bundle helper arithmetic",
        "tests/linux_asset_export/world_surface_inventory_test.cpp",
        ("src",),
    ),
    Case(
        "world_surface_context",
        "shader-constant context for one unlit surface draw",
        "tests/linux_renderer/world_surface_context_test.cpp",
        ("tests/linux_renderer",),
    ),
    Case(
        "world_surface_bundle",
        "bundle loader, shader framing walk and vertex accessors",
        "tests/linux_renderer/world_surface_bundle_test.cpp",
        ("tests/linux_renderer",),
    ),
    Case(
        "world_surface_oracle",
        "oracle hashing, weights, sampling and pixel arithmetic",
        "tests/linux_renderer/world_surface_oracle_test.cpp",
        ("tests/linux_renderer",),
    ),
    Case(
        "world_lit_bundle",
        "lit bundle loader refusals: program pair, widened geometry, colour map and lightmap",
        "tests/linux_renderer/world_lit_bundle_test.cpp",
        ("tests/linux_renderer",),
    ),
    # Header only, like the inventory case: the rules the offline world front-end
    # probe holds each frame to, and the scan that turns a map's entity string into
    # spawn-point cameras. No engine header, no asset tree, no zone.
    Case(
        "world_frontend_invariants",
        "world front-end frame rules and the spawn-point camera scan",
        "tests/linux_asset_export/world_frontend_invariants_test.cpp",
        ("src",),
    ),
    # Header only as well: the rules the world device probe holds each texture,
    # shader and vertex/index buffer to, over plain numbers. No D3D9 header, no
    # device, no zone.
    Case(
        "world_device_rules",
        "world device texture, shader and buffer rules",
        "tests/linux_asset_export/world_device_rules_test.cpp",
        ("src",),
    ),
    # Header only: what the world draw probe holds a drawn frame to, checked on
    # frames built in memory. No engine header, no D3D9 header, no stored image.
    Case(
        "world_draw_rules",
        "world draw frame measurement and rules",
        "tests/linux_asset_export/world_draw_rules_test.cpp",
        ("src",),
    ),
    # Header only: the reader of a map's effect table and createfx list, checked on
    # script text the test writes itself. No engine header, no zone, no script VM.
    Case(
        "map_effects",
        "map effect table, createfx reader and effect plan",
        "tests/linux_asset_export/map_effects_test.cpp",
        ("src",),
    ),
    # The one case with a second translation unit. The capture state, its arena and
    # its mutex live in the unit beside its header, so the test cannot see them
    # without linking that unit. It is a single file from src/linux/tools, not the
    # engine: no engine header, no asset tree, no zone, and -pthread for the mutex.
    Case(
        "world_image_capture",
        "image capture bounds, name filter, duplicate rule and disabled path",
        "tests/linux_asset_export/world_image_capture_test.cpp",
        ("src",),
        extra_sources=("src/linux/tools/world_image_capture.cpp",),
        flags=("-pthread",),
    ),
    # The Microsoft runtime's rand() and srand() that the draw probe links in place
    # of glibc's. The test needs the unit itself beside it, since replacing glibc's
    # definitions is a property of the link, and -pthread for its per-thread
    # checks. One file from src/linux, no engine header.
    Case(
        "msvc_rand",
        "the Microsoft runtime's rand and srand: sequence, 15-bit range, per-thread state, hidden from shared libraries",
        "tests/linux_compat/msvc_rand_test.cpp",
        (),
        extra_sources=("src/linux/client/msvc_rand.cpp",),
        flags=("-pthread",),
    ),
)

PREPARE_NAME = "world_surface_prepare"
PREPARE_SCRIPT = "scripts/tests/test_world_surface_prepare.py"
PREPARE_DESCRIPTION = "prepare trust boundary over archives it writes itself"

KNOWN_NAMES = tuple(case.name for case in CASES) + (PREPARE_NAME,)


class Command:
    """One subprocess: argv, working directory, timeout."""

    def __init__(self, argv, cwd, timeout):
        self.argv = [str(part) for part in argv]
        self.cwd = Path(cwd)
        self.timeout = timeout

    def describe(self):
        return " ".join(shlex.quote(part) for part in self.argv)


class Check:
    """One named check, the commands it runs in order, and its built binary."""

    def __init__(self, name, description, commands, binary=None):
        self.name = name
        self.description = description
        self.commands = commands
        # Set for the compiled checks: the binary must be a 32-bit i386 ELF and its
        # output must be free of FAIL lines.
        self.binary = binary


def compile_argv(case, repo, workdir, cxx, config, extra_flags):
    """The exact compiler line for one case in one configuration."""
    include_arguments = []
    for directory in case.include_dirs:
        include_arguments.extend(("-I", str(repo / directory)))
    return [
        cxx,
        *COMPILE_FLAGS,
        *CONFIG_FLAGS[config],
        # The case's own flags come from its own entry and nothing else, so a flag
        # one check needs (-pthread for the capture unit's mutex) cannot appear on a
        # line that does not need it.
        *case.flags,
        *extra_flags,
        *include_arguments,
        *[str(repo / source) for source in case.sources],
        "-o",
        str(workdir / case.name),
    ]


def plan_checks(options, workdir, repo, python, cxx, selected):
    """The documented command list, in execution order."""
    checks = []

    for case in CASES:
        if case.name not in selected:
            continue
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

    if PREPARE_NAME in selected:
        checks.append(
            Check(
                PREPARE_NAME,
                PREPARE_DESCRIPTION,
                [
                    Command(
                        [python, repo / PREPARE_SCRIPT],
                        cwd=repo,
                        timeout=options.timeout,
                    )
                ],
            )
        )

    return checks


def is_target_elf(path):
    """True when the file is an ELF32 little-endian i386 executable.

    Checked from the header bytes rather than by running readelf: ``-m32`` producing
    a 64-bit binary would make every check here meaningless, and that has to be
    visible even on a host without binutils.
    """
    try:
        header = Path(path).read_bytes()[:20]
    except OSError:
        return False
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return False
    elf_class = header[4]
    elf_data = header[5]
    (machine,) = struct.unpack_from("<H", header, 18)
    return elf_class == 1 and elf_data == 1 and machine == TARGET_MACHINE


def failure_lines(output):
    """Lines in a test's output that report a failed check."""
    return [line for line in output.splitlines() if FAILURE_MARKER in line]


def run_command(command):
    """Run one command in its own process group and return (status, output, seconds).

    The process group matters on timeout: a test that spawns compilers leaves them
    behind if only the direct child is killed, and those survivors keep the job's
    pipes open.
    """
    print(f"$ {command.describe()}", flush=True)

    started = time.monotonic()
    try:
        process = subprocess.Popen(
            command.argv,
            cwd=str(command.cwd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            start_new_session=True,
        )
    except OSError as error:
        elapsed = time.monotonic() - started
        print(f"  cannot start: {error}", flush=True)
        return "error", f"cannot start {command.argv[0]}: {error}", elapsed

    try:
        output, _ = process.communicate(timeout=command.timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except OSError:
            process.kill()
        output, _ = process.communicate()

    elapsed = time.monotonic() - started

    if output:
        print(output if output.endswith("\n") else output + "\n", flush=True)

    if timed_out:
        print(f"  TIMEOUT after {command.timeout}s", flush=True)
        return "timeout", output or "", elapsed

    if process.returncode != 0:
        print(f"  exit {process.returncode}", flush=True)
        return "failed", output or "", elapsed

    print(f"  ok ({elapsed:.1f}s)", flush=True)
    return "ok", output or "", elapsed


def write_log(log_dir, name, text):
    """Keep the text of a command for upload; text logs only, no binaries."""
    if not log_dir:
        return
    path = Path(log_dir) / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def print_plan(checks, repo, config, workdir, log_dir):
    """Print the whole command list before anything runs."""
    print("World surface checks: asset-free, no DXVK, no SDL, no device", flush=True)
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


def run_checks(checks, log_dir):
    """Run every planned check and return (results, failures)."""
    # Each check records its own outcome so a late failure cannot hide an early one.
    results = []

    for index, check in enumerate(checks, start=1):
        print(f"[{index}/{len(checks)}] {check.name}: {check.description}", flush=True)
        print("-" * 64, flush=True)

        status, detail, elapsed = "ok", "", 0.0
        last_output = ""
        for part, command in enumerate(check.commands, start=1):
            kind = "compile" if part == 1 and check.binary and len(check.commands) == 2 else "run"
            if not check.binary:
                kind = "script"
            command_status, output, command_elapsed = run_command(command)
            elapsed += command_elapsed
            last_output = output
            write_log(
                log_dir,
                f"{index:02d}-{check.name}-{kind}.log",
                f"$ {command.describe()}\n\n{output}\n",
            )

            if command_status != "ok":
                status = command_status
                detail = f"{kind} {command_status}"
                break

        # A check that produced nothing verifiable is not a pass: the binary has to
        # be the 32-bit object this runner asked for, and its own output has to be
        # free of failure lines.
        if status == "ok" and check.binary is not None:
            if not check.binary.is_file():
                status, detail = "failed", "no binary was produced"
            elif not is_target_elf(check.binary):
                status, detail = "failed", f"binary is not ELF{TARGET_BITS}/i386"
            else:
                reported = failure_lines(last_output)
                if reported:
                    status = "failed"
                    detail = f"{len(reported)} failure line(s) reported"

        results.append((check, status, detail, elapsed))
        print(flush=True)

    write_log(
        log_dir,
        "summary.txt",
        "\n".join(f"{status}\t{check.name}\t{detail}" for check, status, detail, _ in results) + "\n",
    )
    failures = sum(1 for _, status, _, _ in results if status != "ok")
    return results, failures


def print_summary(results, failures):
    print("Summary", flush=True)
    print("=======", flush=True)
    for check, status, detail, elapsed in results:
        label = {"ok": "PASS", "failed": "FAIL", "timeout": "TIMEOUT", "error": "ERROR"}.get(
            status, status.upper()
        )
        suffix = f" ({detail})" if detail else ""
        print(f"  {label:<8} {check.name:<24} {elapsed:5.1f}s  {check.description}{suffix}", flush=True)

    print(flush=True)
    total = len(results)
    if failures:
        print(f"{failures} of {total} check(s) failed", flush=True)
        return 1
    print(f"all {total} check(s) passed", flush=True)
    return 0


def parse_names(value):
    return {name.strip() for name in value.split(",") if name.strip()} if value else set()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--config",
        choices=sorted(CONFIG_FLAGS),
        default="Debug",
        help="optimization configuration: Debug is -O0, RelWithDebInfo is -O2 (default: Debug)",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "g++"),
        help="32-bit C++ compiler for the standalone checks (default: g++)",
    )
    parser.add_argument(
        "--cxx-flag",
        action="append",
        default=[],
        metavar="FLAG",
        help="extra compiler flag, repeatable (for example -fsanitize=address); never implied",
    )
    parser.add_argument(
        "--python",
        default=sys.executable or "python3",
        help="interpreter for the Python checks (default: this interpreter)",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=ROOT,
        help="repository root holding the sources to compile (default: this checkout)",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        help="directory for the compiled binaries (default: a per-run temporary directory)",
    )
    parser.add_argument("--timeout", type=int, default=DEFAULT_RUN_TIMEOUT, help="timeout of each run, in seconds")
    parser.add_argument(
        "--compile-timeout",
        type=int,
        default=DEFAULT_COMPILE_TIMEOUT,
        help="timeout of each compile, in seconds",
    )
    parser.add_argument("--log-dir", type=Path, help="write one text log per command, plus summary.txt")
    parser.add_argument("--only", help="comma-separated check names to run")
    parser.add_argument("--skip", help="comma-separated check names to leave out")
    parser.add_argument("--list", action="store_true", help="print the command list and exit without running")
    options = parser.parse_args()

    options.only = parse_names(options.only)
    options.skip = parse_names(options.skip)
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

    missing = [str(repo / source) for case in CASES if case.name in selected for source in case.sources]
    missing += [str(repo / PREPARE_SCRIPT)] if PREPARE_NAME in selected else []
    for path in missing:
        if not Path(path).is_file():
            print(f"FAIL: source is missing: {path}", flush=True)
            return 1

    if not shutil.which(options.cxx) and not Path(options.cxx).is_file():
        print(f"FAIL: compiler not found: {options.cxx}", flush=True)
        print("Install the multilib toolchain (g++, g++-multilib) or pass --cxx.", flush=True)
        return 1

    temporary = None
    if options.work_dir:
        workdir = options.work_dir.expanduser().resolve()
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="kisakcod-world-surface-")
        workdir = Path(temporary.name)

    try:
        checks = plan_checks(options, workdir, repo, options.python, options.cxx, selected)
        print_plan(checks, repo, options.config, workdir, options.log_dir)

        if options.list:
            print("--list: printed the command list, ran nothing.", flush=True)
            return 0

        started = time.monotonic()
        results, failures = run_checks(checks, options.log_dir)
        elapsed = time.monotonic() - started
        print(f"elapsed: {elapsed:.1f}s", flush=True)
        return print_summary(results, failures)
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
