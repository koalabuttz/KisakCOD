#!/usr/bin/env python3
"""Checks for the opt-in native Linux client network core milestone.

Verified here:

1. **Bounded static facts** about the wiring: which production TUs are compiled,
   that the option is opt-in and off by default, that the client TUs are built
   without ``KISAK_DEDICATED``/``DEDICATED``, and the two command-header offsets
   the transmit wrappers use. The offsets are read from the production sources;
   no fake call is made to reach them (see ``check_header_offsets``).

2. **The real compile commands**, from the generated ``compile_commands.json``:
   eight full client TUs, ``-m32``, ``-DKISAK_MP``, no dedicated defines, and
   ``-DKISAK_OPENAL`` present only as the header shim. Static CMake text is not
   enough on its own -- a comment can say anything.

3. **The asset-free codec, state and packet integration tests build and pass**, in its own build
   directory, built twice so an incremental rebuild is proven to work (the
   build directory is reused, never wiped: a full configure from scratch would
   re-download dependencies).

4. **``bin/KisakCOD-dedi`` is untouched.** Fingerprinted before the work and
   compared in a ``finally``, so the check still runs when the build fails. A
   running dedicated server must never be disturbed.

Not registered in ``scripts/tests/run_linux_checks.py``; the workflow is owned
elsewhere.

Usage:
    python3 scripts/tests/test_linux_client_core.py [--cxx clang++-19] [--cc clang-19]
    python3 scripts/tests/test_linux_client_core.py --self-test
    python3 scripts/tests/test_linux_client_core.py --static-only
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

LINUX_CMAKE = ROOT / "scripts" / "platform" / "linux" / "CMakeLists.txt"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
CLIENT_NETCHAN = ROOT / "src" / "client_mp" / "cl_net_chan_mp.cpp"
SERVER_NETCHAN = ROOT / "src" / "server_mp" / "sv_net_chan_mp.cpp"
DEDI_BINARY = ROOT / "bin" / "KisakCOD-dedi"

OPTION = "KISAK_BUILD_LINUX_CLIENT_CORE"

# The milestone compiles exactly these client translation units.
EXPECTED_TUS = (
    "src/client_mp/cl_auth.cpp",
    "src/client_mp/cl_connection_mp.cpp",
    "src/client_mp/cl_connection_receive_mp.cpp",
    "src/client_mp/cl_main_mp.cpp",
    "src/client_mp/cl_cgame_mp.cpp",
    "src/client/cl_ui_draw.cpp",
    "src/client_mp/cl_net_chan_mp.cpp",
    "src/client_mp/cl_parse_mp.cpp",
    "src/client_mp/cl_input.cpp",
)
STATE_RUNTIME_TUS = (
    "src/client_mp/cl_parse_mp.cpp",
    "src/client_mp/cl_input.cpp",
    "src/qcommon/msg_mp.cpp",
    "src/universal/q_shared.cpp",
)

# UI wrappers moved to a separate core TU; the unchanged packet fixture does
# not consume them. Keep its production source expectations explicit.
PACKET_RUNTIME_TUS = (
    "src/client_mp/cl_cgame_mp.cpp", "src/client_mp/cl_net_chan_mp.cpp",
    "src/client_mp/cl_parse_mp.cpp", "src/client_mp/cl_input.cpp",
) + (
    "src/server_mp/sv_net_chan_mp.cpp", "src/qcommon/msg_mp.cpp",
    "src/qcommon/huffman.cpp", "src/universal/q_shared.cpp",
    "src/universal/com_shared.cpp", "src/universal/com_math.cpp",
)

AUTH_RUNTIME_TUS = (
    "src/client_mp/cl_auth.cpp", "src/client_mp/cl_connection_mp.cpp",
    "src/universal/base64.cpp", "src/universal/q_shared.cpp",
    "src/qcommon/msg_mp.cpp", "src/qcommon/huffman.cpp", "src/universal/com_shared.cpp",
)

RECEIVE_RUNTIME_TUS = (
    "src/client_mp/cl_connection_mp.cpp", "src/client_mp/cl_connection_receive_mp.cpp",
    "src/qcommon/cmd.cpp", "src/qcommon/msg_mp.cpp", "src/qcommon/huffman.cpp",
    "src/universal/q_shared.cpp", "src/universal/com_shared.cpp",
)
PLATFORM_RUNTIME_TUS = ("src/client_mp/cl_main_mp.cpp", "src/universal/q_shared.cpp")

HEADLESS_RUNTIME_TUS = (
    "src/linux/client/headless_main.cpp", "src/linux/client/diagnostic_host.cpp",
    "src/linux/client/discovery_session.cpp", "src/linux/sys_events.cpp",
    "src/qcommon/cmd.cpp", "src/universal/dvar_cmds.cpp",
    "src/linux/net.cpp", "src/linux/sys_threads.cpp", "src/linux/sys_critical.cpp",
    "src/linux/sys_posix.cpp", "src/qcommon/net_chan_mp.cpp", "src/universal/dvar.cpp",
    "src/universal/q_shared.cpp", "src/universal/com_shared.cpp", "src/universal/com_math.cpp",
    "src/client_mp/cl_auth.cpp", "src/universal/base64.cpp",
)

failures = []
checks = 0


def ok(name):
    global checks
    checks += 1
    print(f"ok: {name}")


def fail(name, detail):
    global checks
    checks += 1
    failures.append(name)
    print(f"FAIL: {name}: {detail}")


def check(name, condition, detail="condition false"):
    if condition:
        ok(name)
    else:
        fail(name, detail)
    return bool(condition)


def read(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"read {path.name}", str(exc))
        return ""


def sha256(path):
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# Compiler mapping
# ---------------------------------------------------------------------------


def map_c_compiler_name(cxx):
    """Map a C++ compiler name to the matching C compiler name.

    The stem must only drop the C++ marker, keeping any version suffix:
    ``clang++-19`` -> ``clang-19`` (not ``clang++-19``), ``g++-13`` -> ``gcc-13``.
    A path prefix is preserved.
    """
    directory, base = os.path.split(cxx)

    match = re.fullmatch(r"clang\+\+(-\d+(?:\.\d+)*)?", base)
    if match:
        mapped = "clang" + (match.group(1) or "")
    else:
        match = re.fullmatch(r"g\+\+(-\d+(?:\.\d+)*)?", base)
        if match:
            mapped = "gcc" + (match.group(1) or "")
        elif base.endswith("++"):
            # Unknown C++ driver: drop the marker rather than keeping it, since
            # a ``++`` name is never a usable C compiler.
            mapped = base[:-2]
        else:
            mapped = base

    return os.path.join(directory, mapped) if directory else mapped


def c_compiler(cxx, cc=None):
    """Resolve the C compiler to use, honouring an explicit ``--cc``."""
    if cc:
        resolved = shutil.which(cc)
        return resolved or cc
    mapped = map_c_compiler_name(cxx)
    resolved = shutil.which(mapped)
    return resolved or mapped


# ---------------------------------------------------------------------------
# Static wiring facts
# ---------------------------------------------------------------------------


def check_option_is_opt_in():
    root = read(ROOT_CMAKE)
    # The option must exist and default to OFF: the native Linux build stays a
    # dedicated server build unless somebody asks for more. The description
    # contains parentheses, so look for the closing "OFF)" in a window after the
    # declaration rather than parsing the whole call.
    index = root.find(f"option({OPTION}")
    if index < 0:
        fail(f"{OPTION} is declared", "option() call not found")
        return
    window = root[index:index + 600]
    check(f"{OPTION} defaults to OFF", bool(re.search(r"\bOFF\s*\)", window)))
    check(
        f"{OPTION} documents that it builds no game client executable",
        "no game client executable" in window,
    )


def check_client_tus_present():
    linux_cmake = read(LINUX_CMAKE)
    for rel in EXPECTED_TUS:
        check(f"{rel} exists on disk", (ROOT / rel).exists())
        # The CMake list spells these as "${SRC_DIR}/dir/file.cpp", so match on
        # the directory/file suffix rather than the repo-relative path.
        suffix = f"{Path(rel).parent.name}/{Path(rel).name}"
        check(f"{rel} is in the client core source list", suffix in linux_cmake)

    # The dedicated target must keep owning its own sources; the client core
    # block must not have rewritten the dedicated source glob.
    check(
        "dedicated target still builds from the shared source lists",
        "add_executable(KisakCOD-dedi" in linux_cmake,
    )
    check(
        "client core is an OBJECT library (no client executable)",
        "add_library(KisakCOD-client-core OBJECT" in linux_cmake,
    )


def check_no_dedicated_defines():
    """The client TU must be built without the dedicated-only defines."""
    linux_cmake = read(LINUX_CMAKE)
    start = linux_cmake.find("KisakCOD-client-core OBJECT")
    if start < 0:
        fail("client core block located", "target not found")
        return
    end = linux_cmake.find("add_executable(KisakCOD-client-netchan-test", start)
    block = linux_cmake[start:end if end > 0 else len(linux_cmake)]

    check("client core defines KISAK_MP", "KISAK_MP" in block)
    for forbidden in ("KISAK_DEDICATED", "DEDICATED"):
        defined = re.search(rf"(?m)^\s*{forbidden}\s*$", block)
        check(f"client core does not define {forbidden}", not defined)

    check(
        "KISAK_OPENAL is documented as a header shim, not an audio backend",
        "KISAK_OPENAL" in block and "type-shim" in block,
    )


def check_header_offsets():
    """Pin the command-header sizes the transmit wrappers skip.

    ``CL_Netchan_Transmit`` encodes ``data + 9`` for ``length - 9`` and
    ``SV_Netchan_Transmit`` encodes ``data + 4`` for ``length - 4``. Both
    wrappers call into ``qcommon/net_chan_mp.cpp``. These static checks retain
    guards for both directions. The packet integration test additionally reaches
    the production client transmit wrapper with a transport capture double; it
    does not exercise sockets or the server transmit wrapper.
    """
    client = read(CLIENT_NETCHAN)
    server = read(SERVER_NETCHAN)

    check(
        "CL_Netchan_Transmit encodes after a 9-byte header",
        bool(re.search(r"CL_Netchan_Encode\s*\(\s*data\s*\+\s*9\s*,\s*length\s*-\s*9\s*\)", client)),
        "expected 'CL_Netchan_Encode(data + 9, length - 9)'",
    )
    check(
        "SV_Netchan_Transmit encodes after a 4-byte header",
        bool(re.search(r"SV_Netchan_Encode\s*\(\s*client\s*,\s*data\s*\+\s*4\s*,\s*length\s*-\s*4\s*\)", server)),
        "expected 'SV_Netchan_Encode(client, data + 4, length - 4)'",
    )


# ---------------------------------------------------------------------------
# Build and run
# ---------------------------------------------------------------------------


def run(cmd, timeout):
    printable = " ".join(str(part) for part in cmd)
    print(f"  $ {printable}")
    return subprocess.run(
        [str(part) for part in cmd],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def flag_present(command, flag):
    """Token-exact flag test that does not match a longer flag."""
    return re.search(rf"(?:^|\s){re.escape(flag)}(?:\s|$)", command) is not None


def check_compile_commands(build_dir):
    """Read the generated compile_commands.json and verify the real flags.

    This is the check that static CMake text cannot substitute for: it reads
    what the compiler was actually invoked with.
    """
    path = build_dir / "compile_commands.json"
    if not check("compile_commands.json was generated", path.exists(), str(path)):
        return

    try:
        entries = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, json.JSONDecodeError) as exc:
        fail("compile_commands.json parses", str(exc))
        return

    by_file = {}
    for entry in entries:
        command = entry.get("command")
        if not command and entry.get("arguments"):
            command = " ".join(entry["arguments"])
        file_name = entry.get("file", "")
        by_file.setdefault(os.path.realpath(file_name), []).append(command or "")

    for rel in EXPECTED_TUS:
        full = os.path.realpath(ROOT / rel)
        commands = by_file.get(full, [])
        if not check(f"{rel} has a compile command", bool(commands),
                     f"found {len(commands)} entries"):
            continue
        # cl_net_chan_mp.cpp is also compiled into the netchan test executable,
        # so a file can legitimately have more than one compile command. Select
        # the object-library one by its output directory and validate that.
        core_commands = [c for c in commands if "KisakCOD-client-core.dir" in c]
        if not check(f"{rel} has exactly one client-core compile command",
                     len(core_commands) == 1,
                     f"found {len(core_commands)} of {len(commands)} entries"):
            continue
        command = core_commands[0]
        check(f"{rel} compiles with -m32", flag_present(command, "-m32"))
        check(f"{rel} compiles with -DKISAK_MP", flag_present(command, "-DKISAK_MP"))
        check(f"{rel} has no -DKISAK_DEDICATED", not flag_present(command, "-DKISAK_DEDICATED"))
        check(f"{rel} has no -DDEDICATED", not flag_present(command, "-DDEDICATED"))
        check(
            f"{rel} uses -DKISAK_OPENAL as the header shim",
            flag_present(command, "-DKISAK_OPENAL"),
        )

    for target, sources in (("state", STATE_RUNTIME_TUS), ("packet", PACKET_RUNTIME_TUS), ("auth", AUTH_RUNTIME_TUS),
                            ("receive", RECEIVE_RUNTIME_TUS), ("platform", PLATFORM_RUNTIME_TUS),
                            ("headless", HEADLESS_RUNTIME_TUS)):
        for rel in sources:
            commands = by_file.get(os.path.realpath(ROOT / rel), [])
            target_directory = "KisakCOD-client-headless.dir" if target == "headless" else f"KisakCOD-client-{target}-test.dir"
            commands = [c for c in commands if target_directory in c]
            if not check(f"{rel} has one runtime-{target} test compile command", len(commands) == 1):
                continue
            command = commands[0]
            for flag in ("-m32", "-DKISAK_MP", "-DKISAK_OPENAL", "-DRELEASE_ASSERTS"):
                check(f"{target} runtime {rel} uses {flag}", flag_present(command, flag))
            for flag in ("-DKISAK_DEDICATED", "-DDEDICATED"):
                check(f"{target} runtime {rel} excludes {flag}", not flag_present(command, flag))


def check_runtime_output(label, result, sections=(), minimum_checks=15):
    print(result.stdout.rstrip())
    if result.stderr:
        print(result.stderr.rstrip())
    check(f"{label} exits 0", result.returncode == 0)
    summary = re.search(r"--\s*(\d+) checks,\s*(\d+) failures\s*--", result.stdout)
    if not check(f"{label} printed a summary", summary is not None):
        return
    ran, failed = map(int, summary.groups())
    check(f"{label} ran a nonempty case set", ran >= minimum_checks)
    check(f"{label} had no failing case", failed == 0)
    section_total = 0
    for section in sections:
        match = re.search(rf"(?m)^{section}: (\d+) checks, (\d+) failures$", result.stdout)
        if check(f"{label} reports {section} execution", match is not None):
            count, errors = map(int, match.groups())
            check(f"{label} {section} checks passed", count > 0 and errors == 0)
            section_total += count
    if sections:
        check(f"{label} sections account for every check", section_total == ran)


def check_build_and_run(cxx, cc, build_dir, build_type, jobs, timeout, build_timeout):
    build_dir.mkdir(parents=True, exist_ok=True)
    before = sha256(DEDI_BINARY)
    try:
        configure = [
            "cmake", "-S", ROOT, "-B", build_dir,
            "-DKISAK_PLATFORM=linux",
            f"-D{OPTION}=ON",
            f"-DCMAKE_BUILD_TYPE={build_type}",
            f"-DCMAKE_C_COMPILER={c_compiler(cxx, cc)}",
            f"-DCMAKE_CXX_COMPILER={cxx}",
            # Needed for check_compile_commands: verify the real flags.
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Keep every artifact inside the private build directory so nothing
            # can land on, or replace, the dedicated binary in bin/.
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={build_dir / 'bin'}",
        ]
        result = run(configure, build_timeout)
        if not check(f"configure ({build_type}) with the client core enabled",
                     result.returncode == 0,
                     (result.stderr or result.stdout)[-2000:]):
            return

        cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
        if not check("configured cache retains the client-core opt-in",
                     f"{OPTION}:BOOL=ON" in cache,
                     "CMake may have reset the cache after a compiler change; use a fresh --build-dir"):
            return
        check_compile_commands(build_dir)

        targets = ["--target", "KisakCOD-client-core", "KisakCOD-client-netchan-test",
                   "KisakCOD-client-state-test", "KisakCOD-client-packet-test",
                   "KisakCOD-client-auth-test", "KisakCOD-client-headless",
                   "KisakCOD-client-receive-test", "KisakCOD-client-platform-test", "-j", str(jobs)]
        result = run(["cmake", "--build", build_dir] + targets, build_timeout)
        if not check(f"build ({build_type}) client core and runtime tests",
                     result.returncode == 0,
                     (result.stderr or result.stdout)[-2000:]):
            return

        # Second build in the same directory: proves the build directory is
        # reusable (an incremental no-op), which is what keeps CI from
        # re-downloading dependencies on every run.
        result = run(["cmake", "--build", build_dir] + targets, build_timeout)
        check("incremental rebuild in the reused build directory succeeds",
              result.returncode == 0,
              (result.stderr or result.stdout)[-2000:])

        for name, label, sections, minimum in (
            ("KisakCOD-client-netchan-test", "netchan test", (), 15),
            ("KisakCOD-client-packet-test", "client packet test", (), 15),
            ("KisakCOD-client-auth-test", "client authentication/resend test", (), 15),
            ("KisakCOD-client-receive-test", "client receive/lifecycle test", (), 34),
            ("KisakCOD-client-platform-test", "client platform test", (), 10),
            ("KisakCOD-client-state-test", "client state test", ("parser", "input"), 15),
        ):
            binary = build_dir / "bin" / name
            if not check(f"{label} binary was produced in the private build dir", binary.exists()):
                continue
            check_runtime_output(label, run([binary], timeout), sections, minimum)
        result = run([sys.executable, ROOT / "scripts/tests/test_linux_client_headless.py",
                      "--binary", build_dir / "bin/KisakCOD-client-headless"], timeout)
        check("headless real UDP/event tests pass", result.returncode == 0,
              (result.stderr or result.stdout)[-2000:])
        print((result.stdout + result.stderr).rstrip())
    finally:
        # Always runs, including when the build failed or an exception escaped:
        # the dedicated binary must be provably untouched regardless.
        after = sha256(DEDI_BINARY)
        check(
            "dedicated binary in bin/ was not modified by this check",
            before == after,
            f"before={before} after={after}",
        )


def check_default_build_has_no_client_targets(cxx, cc, timeout, build_timeout):
    """With the option unset, the targets must not exist at all."""
    scratch = tempfile.TemporaryDirectory(prefix="kisakcod_client_core_off_")
    try:
        scratch_dir = Path(scratch.name) / "build"
        configure = [
            "cmake", "-S", ROOT, "-B", scratch_dir,
            "-DKISAK_PLATFORM=linux",
            f"-DCMAKE_C_COMPILER={c_compiler(cxx, cc)}",
            f"-DCMAKE_CXX_COMPILER={cxx}",
        ]
        result = run(configure, build_timeout)
        if not check("configure with default options", result.returncode == 0,
                     (result.stderr or result.stdout)[-2000:]):
            return

        result = run(["cmake", "--build", scratch_dir, "--target", "help"], timeout)
        # If `--target help` itself fails, the absence assertions below would
        # pass vacuously against empty output. Require the listing first.
        if not check("default build target listing succeeds", result.returncode == 0,
                     (result.stderr or result.stdout)[-2000:]):
            return
        help_text = result.stdout + result.stderr
        check("default build exposes no client core target",
              "KisakCOD-client-core" not in help_text)
        check("default build exposes no netchan test target",
              "KisakCOD-client-netchan-test" not in help_text)
        check("default build exposes no client state test target",
              "KisakCOD-client-state-test" not in help_text)
        check("default build exposes no client packet test target",
              "KisakCOD-client-packet-test" not in help_text)
        for name in ("KisakCOD-client-auth-test", "KisakCOD-client-headless",
                     "KisakCOD-client-receive-test", "KisakCOD-client-platform-test",
                     "KisakCOD-client-window", "KisakCOD-window-platform", "KisakCOD-window-test", "KisakCOD-client-input", "KisakCOD-client-input-test", "KisakCOD-shutdown-signal-test", "KisakCOD-client-command-test", "KisakCOD-client-runtime-test", "KisakCOD-client-input-startup-test", "KisakCOD-presentation-test", "KisakCOD-frame-deadline-test", "KisakCOD-pixel-resource-test", "KisakCOD-cpu-renderer-test", "KisakCOD-cpu-renderer-fault-test", "KisakCOD-cpu-raw-test", "KisakCOD-material-clone-mp-test", "KisakCOD-material-clone-radiant-test", "KisakCOD-cpu-ui-test", "KisakCOD-material-layered-mp-test", "KisakCOD-material-layered-sp-test", "KisakCOD-material-layered-radiant-test", "KisakCOD-cpu-text-test", "KisakCOD-cpu-menu-test", "KisakCOD-menu-assets-test", "KisakCOD-ui-material-assets-test", "KisakCOD-menu-composition-assets-test", "KisakCOD-iwi-assets-test", "KisakCOD-ui-asset-export", "KisakCOD-font-assets-test", "KisakCOD-menu-input-owner-test", "KisakCOD-menu-control-assets-test", "KisakCOD-menu-script-test", "KisakCOD-ui-audio-test", "KisakCOD-ui-audio-assets-test", "KisakCOD-ui-audio-failure-test", "KisakCOD-menu-input-failure-test", "KisakCOD-cpu-viewport-test", "KisakCOD-ui-time-test", "KisakCOD-cpu-composition-test", "KisakCOD-cpu-traversal-test"):
            check(f"default build exposes no {name} target", name not in help_text)
    finally:
        # TemporaryDirectory is uniquely named, so cleanup can only remove this
        # run's own directory -- never another agent's concurrent build.
        scratch.cleanup()


# ---------------------------------------------------------------------------
# Self test (no cmake, no build)
# ---------------------------------------------------------------------------


def self_test():
    """Focused unit checks for the compiler-name mapping."""
    cases = [
        ("clang++", "clang"),
        ("clang++-19", "clang-19"),
        ("clang++-19.1", "clang-19.1"),
        ("g++", "gcc"),
        ("g++-13", "gcc-13"),
        ("/usr/bin/clang++", "/usr/bin/clang"),
        ("/usr/bin/clang++-19", "/usr/bin/clang-19"),
        ("/usr/bin/g++-13", "/usr/bin/gcc-13"),
        ("clang", "clang"),
        ("gcc", "gcc"),
        ("cc", "cc"),
    ]
    for given, expected in cases:
        got = map_c_compiler_name(given)
        check(f"map {given} -> {expected}", got == expected, f"got {got}")

    # A resolved explicit --cc wins over the derived mapping.
    explicit = c_compiler("clang++-19", "/bin/sh")
    check("explicit --cc is honoured", explicit == "/bin/sh", f"got {explicit}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"),
                        help="C++ compiler (default: clang++ or $CXX)")
    parser.add_argument("--cc", default=None,
                        help="explicit C compiler; default derives it from --cxx")
    parser.add_argument("--build-dir", default=str(ROOT / "build-linux" / "client-core"),
                        help="private, reusable build directory for the opt-in build")
    parser.add_argument("--build-type", default="Debug",
                        help="CMAKE_BUILD_TYPE (default: Debug; the CI matrix also "
                             "builds RelWithDebInfo)")
    parser.add_argument("--jobs", type=int, default=2,
                        help="parallel build jobs (default: 2)")
    parser.add_argument("--timeout", type=int, default=300,
                        help="per-command timeout for short commands (default: 300)")
    parser.add_argument("--build-timeout", type=int, default=1800,
                        help="per-command timeout for cmake configure/build (default: 1800)")
    parser.add_argument("--static-only", action="store_true",
                        help="run only the static wiring checks, skip all building")
    parser.add_argument("--self-test", action="store_true",
                        help="run the focused compiler-mapping unit checks and exit")
    args = parser.parse_args()

    if args.self_test:
        print("== native Linux client core: self test ==")
        self_test()
        print(f"-- {checks} checks, {len(failures)} failures --")
        return 0 if not failures else 1

    print("== native Linux client core: static wiring ==")
    check_option_is_opt_in()
    check_client_tus_present()
    check_no_dedicated_defines()
    check_header_offsets()

    if not args.static_only:
        print("== native Linux client core: build and run ==")
        # Resolved early so a relative --build-dir cannot make CMake interpret
        # CMAKE_RUNTIME_OUTPUT_DIRECTORY relative to the wrong directory.
        build_dir = Path(args.build_dir).expanduser().resolve()
        check_build_and_run(args.cxx, args.cc, build_dir, args.build_type,
                            args.jobs, args.timeout, args.build_timeout)
        check_default_build_has_no_client_targets(args.cxx, args.cc,
                                                  args.timeout, args.build_timeout)

    print(f"-- {checks} checks, {len(failures)} failures --")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
