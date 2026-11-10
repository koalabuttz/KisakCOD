#!/usr/bin/env python3
"""Run the asset-free native Linux checks against an already configured build.

This runner performs **no configure and no build**. The caller produces the
32-bit dedicated server first -- ``bash build-linux.sh <build-dir> <config>`` or
the Linux CI workflow -- and then points this script at that build directory and
the executable it produced. Everything below runs without game data, a Steam
client, credentials, or a listening game server:

  * bootstrap checks against the explicit ``--binary`` executable;
  * the platform runtime wrapper tests (console input, files, threads, CLI);
  * asset-free lifecycle-driver query parsing and failure cleanup;
  * the script debugger boundary, built from the real Linux implementation;
  * the production fastfile read path, built against the bundled zlib;
  * deterministic network-deadline arithmetic and native UDP in ``tests/linux_net``;
  * the message Huffman tree, which must match the one Windows clients build;
  * the file I/O and virtual-memory self-tests shipped in the platform sources;
  * the script lexer EOF regression, which ends a scan with no local clients;
  * the random-helper ranges, which must stay 15-bit where Linux rand() is 31-bit;
  * the MSVC CRT shims in ``src/linux/compat.h`` that client sources call.

``--arch x86_64``, ``aarch64``, ``macos-arm64`` or ``macos-x86_64`` checks a
64-bit or macOS build instead: the bootstrap and the platform self-tests
(runtime, network deadlines, UDP, file I/O, virtual memory) are built for the
host's own width, and the i386-only checks are left out.

Each command runs in its own process group with its own timeout, so a stuck
check is killed instead of hanging the job. Every command prints the exact argv
it ran, its working directory, its exit status and its output, and the run ends
with one status line per check: nothing is reported as success unless the
process actually exited 0. Temporary files (the compiled self-tests, and the
``TMPDIR`` used by ``tests/linux_net``) live in a per-run temporary directory
that is removed afterwards, so two concurrent runs cannot collide in ``/tmp``.

Usage:
    python3 scripts/tests/run_linux_checks.py \\
        --build-dir build-linux/debug \\
        --binary build-linux/debug/bin/KisakCOD-dedi

    python3 scripts/tests/run_linux_checks.py --list    # print, run nothing

Requires the build's multilib toolchain: ``clang++ -m32`` must link, which means
``clang`` and ``g++-multilib`` are installed.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "scripts" / "tests"
DEFAULT_BUILD_DIR = ROOT / "build-linux"
DEFAULT_TIMEOUT = 300
COMPILER_PREFIX = ("-m32", "-fms-extensions", "-pthread")

# --arch: i386 is the shipped server and runs every check. The 64-bit builds
# (x86_64, aarch64) and macOS run the checks that build for the host's own
# width, so the platform code they exercise (timed waits, sockets, virtual
# memory, files) is tested on that platform; the rest stay i386 checks.
ARCHITECTURES = ("i386", "x86_64", "aarch64", "macos-arm64", "macos-x86_64")
NATIVE_CHECKS = ("bootstrap", "runtime", "net_deadline", "udp", "sys_fileio", "sys_virtualmem", "perf_ab")


def compiler_prefix(arch):
    """Flags every built-from-source self-test starts with."""
    if arch == "i386":
        return COMPILER_PREFIX
    return tuple(flag for flag in COMPILER_PREFIX if flag != "-m32")


class Check:
    """One named check and the commands it runs, in order."""

    def __init__(self, name, description, commands, requires=None):
        self.name = name
        self.description = description
        self.commands = commands
        # A check may need an artefact that only the caller can produce (the
        # compiled server). A missing artefact is reported, not guessed around.
        self.requires = requires


class Command:
    """One subprocess: argv, working directory, extra environment, timeout."""

    def __init__(self, argv, cwd=None, env=None, timeout=DEFAULT_TIMEOUT):
        self.argv = [str(part) for part in argv]
        self.cwd = Path(cwd) if cwd else ROOT
        self.env = {key: str(value) for key, value in (env or {}).items()}
        self.timeout = timeout

    def describe(self):
        return " ".join(shlex.quote(part) for part in self.argv)


def plan_checks(options, workdir, build_dir, binary, python, cxx):
    """The documented command list, in execution order."""
    arch = getattr(options, "arch", "i386")
    prefix = compiler_prefix(arch)
    native = arch != "i386"
    checks = []

    checks.append(
        Check(
            "bootstrap",
            "native bootstrap against the compiled server",
            [Command([python, TESTS / "test_linux_bootstrap.py",
                      *(["--arch", arch] if native else []), binary], timeout=180)],
            requires=binary,
        )
    )

    runtime_argv = [python, TESTS / "test_linux_runtime.py", "--compiler", cxx,
                    "--build-dir", build_dir]
    checks.append(
        Check(
            "runtime",
            "platform runtime wrappers through real reads",
            [Command(runtime_argv, timeout=600)],
        )
    )

    checks.append(
        Check(
            "lifecycle_driver",
            "asset-free tests of lifecycle query validation and failure cleanup",
            [Command([python, TESTS / "test_linux_lifecycle_driver.py"], timeout=60)],
        )
    )

    checks.append(
        Check(
            "handshake_auth",
            "production server authentication parsing and local handshake safeguards",
            [Command([python, TESTS / "test_server_handshake_auth.py", "--cxx", cxx, "--m32"], timeout=300)],
        )
    )

    checks.append(
        Check(
            "remote_input",
            "production rcon password and download name validation",
            [Command([python, TESTS / "test_server_remote_input.py", "--cxx", cxx, "--m32"], timeout=300)],
        )
    )

    checks.append(
        Check(
            "steam_session",
            "opt-in Steam backend against a stand-in steamclient.so, and its flag-on compile proof",
            [Command([python, TESTS / "test_linux_steam_session.py", "--cxx", cxx, "--m32"], timeout=300)],
        )
    )

    checks.append(
        Check(
            "debugger",
            "script debugger boundary without a UI",
            [Command([python, TESTS / "test_linux_debugger.py", "--cxx", cxx], timeout=300)],
        )
    )

    checks.append(
        Check(
            "fastfile",
            "production fastfile read path with bundled zlib",
            [
                Command(
                    [python, TESTS / "test_linux_fastfile_io.py", "--cxx", cxx, "--timeout", "180"],
                    timeout=600,
                )
            ],
        )
    )

    # tests/linux_net/run_tests.sh writes its binary to ${TMPDIR:-/tmp} under a
    # fixed name, so TMPDIR is pointed at this run's temporary directory.
    checks.append(
        Check(
            "net_deadline",
            "fractional network wait deadlines remain positive",
            [Command([python, TESTS / "test_linux_net_deadline.py", "--cxx", cxx,
                      *(["--native"] if native else [])], timeout=180)],
        )
    )

    # The message Huffman tree must match the one Windows clients build, or
    # compressed messages decode into garbage on a live connection.
    checks.append(
        Check(
            "huffman",
            "message Huffman tree matches the Windows client's",
            [Command([python, TESTS / "test_huffman_tree.py", "--cxx", cxx], timeout=300)],
        )
    )

    checks.append(
        Check(
            "udp",
            "native UDP stack in tests/linux_net",
            [
                Command(
                    ["bash", ROOT / "tests" / "linux_net" / "run_tests.sh"],
                    env={"TMPDIR": workdir / "udp", "CXX": cxx,
                         **({"KISAK_TEST_ARCH_FLAGS": ""} if native else {})},
                    timeout=300,
                )
            ],
        )
    )

    fileio_binary = workdir / "sys_fileio_selftest"
    checks.append(
        Check(
            "sys_fileio",
            "file I/O self-test from the platform sources",
            [
                Command(
                    [
                        cxx,
                        *prefix,
                        "-std=c++20",
                        "-I",
                        ROOT / "src",
                        "-DSYS_FILEIO_SELFTEST",
                        ROOT / "src" / "linux" / "sys_fileio.cpp",
                        "-o",
                        fileio_binary,
                    ],
                    timeout=180,
                ),
                Command([fileio_binary], timeout=180),
            ],
        )
    )

    virtualmem_binary = workdir / "sys_virtualmem_selftest"
    checks.append(
        Check(
            "sys_virtualmem",
            "virtual-memory self-test, including fault assertions",
            [
                Command(
                    [
                        cxx,
                        *prefix,
                        "-std=gnu++20",
                        "-D_GNU_SOURCE",
                        "-I",
                        ROOT / "src",
                        "-DKISAK_SYS_VIRTUALMEM_SELFTEST",
                        ROOT / "src" / "linux" / "sys_virtualmem.cpp",
                        ROOT / "src" / "linux" / "sys_fileio.cpp",
                        "-o",
                        virtualmem_binary,
                    ],
                    timeout=180,
                ),
                Command([virtualmem_binary], timeout=180),
            ],
        )
    )

    # Shared regression for the end-of-scan fix: same test the script parser
    # workflow runs, repeated here because the Linux scan path depends on it.
    checks.append(
        Check(
            "lexer_eof",
            "script lexer reaches EOF without local clients",
            [Command([python, TESTS / "test_script_lexer_eof.py", "--repo", ROOT],
                     env={"CXX": cxx}, timeout=300)],
        )
    )

    # Linux rand() returns 31 bits where the Windows CRT returned 15, so every
    # helper that scales it by 32768 must still produce a unit interval.
    checks.append(
        Check(
            "random_range",
            "random helpers keep their 15-bit ranges on Linux",
            [Command([python, TESTS / "test_random_range.py", "--cxx", cxx], timeout=300)],
        )
    )

    # random_range bounds the shape of a grenade launch velocity but never enters
    # the gameplay bodies; this drives the production death drop and item
    # trajectory paths instead.
    checks.append(
        Check(
            "gameplay_lifecycle",
            "death-grenade drop and item launch/trajectory behaviour",
            [Command([python, TESTS / "test_gameplay_lifecycle.py", "--cxx", cxx], timeout=300)],
        )
    )

    checks.append(
        Check(
            "server_test_clients",
            "internal test-client slot and command invariants",
            [Command([python, TESTS / "test_server_test_clients.py", "--cxx", cxx,
                      "--m32", "--sanitize"], timeout=300)],
        )
    )

    checks.append(
        Check(
            "weapon_def_ownership",
            "weapon string ownership in dedicated and client load/shutdown paths",
            [Command([python, TESTS / "test_weapon_def_ownership.py", "--cxx", cxx,
                      "--m32"], timeout=300)],
        )
    )
    checks.append(
        Check(
            "bot_driver",
            "live bot probe rejects incomplete or unhealthy hold samples",
            [Command([python, TESTS / "test_linux_bot_driver.py"], timeout=60)],
        )
    )

    checks.append(
        Check(
            "database_publication",
            "complete database batch publication before worker notification",
            [Command([python, TESTS / "test_database_work_publication.py", "--cxx", cxx], timeout=120)],
        )
    )

    # Client sources call a few MSVC CRT entry points directly; compat.h maps
    # them, and the fail-closed cases must still terminate on Linux.
    checks.append(
        Check(
            "crt_shims",
            "MSVC CRT shims for client sources, including fail-closed cases",
            [Command([python, TESTS / "test_linux_crt_shims.py", "--cxx", cxx], timeout=300)],
        )
    )
    # The native assertion handler stops the process; KISAK_ASSERT_CONTINUE=1 is
    # an opt-in survey mode that reports and returns instead.
    checks.append(
        Check(
            "assert_continue",
            "native assertion handler: fatal by default, opt-in report-and-continue",
            [Command([python, TESTS / "test_linux_assert_continue.py", "--cxx", cxx], timeout=300)],
        )
    )
    checks.append(
        Check(
            "sl_trace",
            "opt-in script string reference trace (KISAK_SL_TRACE) on i386 and x86_64",
            [Command([python, TESTS / "test_linux_sl_trace.py", "--cxx", cxx], timeout=300)],
        )
    )
    # Game structs keep a physics body in a 32-bit field; x86_64 needs an id table.
    checks.append(
        Check(
            "phys_objid",
            "physics object ids in 32-bit fields on i386 and x86_64",
            [Command([python, TESTS / "test_linux_phys_objid.py", "--cxx", cxx], timeout=300)],
        )
    )

    # perf_ab and perf_sample: a laptop run costs battery and a place in a
    # queue, so what they compute is checked here first (docs/perf-ab.md).
    checks.append(
        Check(
            "perf_ab",
            "in-game A/B measuring: window order, statistics, machine readings",
            [Command([python, TESTS / "test_linux_perf_ab.py", "--cxx", cxx,
                      *([] if native else ["--m32"])], timeout=300)],
        )
    )

    if native and not options.only:
        checks = [check for check in checks if check.name in NATIVE_CHECKS]
    if options.only:
        checks = [check for check in checks if check.name in options.only]
    if options.skip:
        checks = [check for check in checks if check.name not in options.skip]
    return checks


def print_plan(checks, build_dir, binary, log_dir):
    """Print the whole command list before anything runs."""
    print("Native Linux checks: asset-free, no server, no credentials", flush=True)
    print("=" * 64, flush=True)
    print(f"repository: {ROOT}", flush=True)
    print(f"build dir:  {build_dir}", flush=True)
    print(f"binary:     {binary}", flush=True)
    if log_dir:
        print(f"logs:       {log_dir}", flush=True)
    print(flush=True)
    print(f"{len(checks)} check(s) in this order:", flush=True)
    for index, check in enumerate(checks, start=1):
        print(f"  {index}. {check.name}: {check.description}", flush=True)
        for part, command in enumerate(check.commands, start=1):
            print(f"     {index}.{part} $ {command.describe()}", flush=True)
            if command.cwd != ROOT:
                print(f"           cwd: {command.cwd}", flush=True)
            if command.env:
                shown = ", ".join(f"{key}={value}" for key, value in sorted(command.env.items()))
                print(f"           env: {shown}", flush=True)
            print(f"           timeout: {command.timeout}s", flush=True)
    print(flush=True)


def run_command(command):
    """Run one command in its own process group and return (status, output, seconds).

    The process group matters on timeout: a test that spawns compilers leaves
    them behind if only the direct child is killed, and those survivors keep the
    job's pipes open.
    """
    environment = dict(os.environ)
    environment.update(command.env)

    print(f"$ {command.describe()}", flush=True)
    if command.cwd != ROOT:
        print(f"  cwd: {command.cwd}", flush=True)

    started = time.monotonic()
    try:
        process = subprocess.Popen(
            command.argv,
            cwd=str(command.cwd),
            env=environment,
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
    """Keep the text of a command for upload; nothing binary, nothing from bin/."""
    if not log_dir:
        return
    path = Path(log_dir) / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def run_checks(checks, log_dir):
    """Run every planned check and return (results, failures)."""
    # Each check records its own outcome so a late failure cannot hide an early
    # one, and a missing artefact is reported as its own status.
    results = []
    failures = 0

    for index, check in enumerate(checks, start=1):
        print(f"[{index}/{len(checks)}] {check.name}: {check.description}", flush=True)
        print("-" * 64, flush=True)

        if check.requires is not None and not Path(check.requires).is_file():
            print(f"  BLOCKED: required artefact is missing: {check.requires}", flush=True)
            print("  This runner does not build; configure and build the target first.", flush=True)
            results.append((check, "blocked", f"missing artefact {check.requires}", 0.0))
            failures += 1
            print(flush=True)
            continue

        status, detail, elapsed = "ok", "", 0.0
        transcript = []
        for part, command in enumerate(check.commands, start=1):
            command_status, output, command_elapsed = run_command(command)
            elapsed += command_elapsed
            transcript.append(f"$ {command.describe()}\n{output}\n")
            write_log(log_dir, f"{index:02d}-{check.name}-{part}.log", f"$ {command.describe()}\n\n{output}\n")

            if command_status != "ok":
                status = command_status
                detail = f"{'command ' + str(part) + ' ' if len(check.commands) > 1 else ''}{command_status}"
                break

        if status != "ok":
            failures += 1
        results.append((check, status, detail, elapsed))
        print(flush=True)

    write_log(
        log_dir,
        "summary.txt",
        "\n".join(f"{status}\t{check.name}\t{detail}" for check, status, detail, _ in results) + "\n",
    )
    return results, failures


def print_summary(results, failures):
    print("Summary", flush=True)
    print("=======", flush=True)
    for check, status, detail, elapsed in results:
        label = {"ok": "PASS", "failed": "FAIL", "timeout": "TIMEOUT", "blocked": "BLOCKED", "error": "ERROR"}.get(
            status, status.upper()
        )
        suffix = f" ({detail})" if detail else ""
        print(f"  {label:<8} {check.name:<16} {elapsed:5.1f}s  {check.description}{suffix}", flush=True)

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
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="configured build directory to take compile flags from (default: build-linux)",
    )
    parser.add_argument(
        "--binary",
        type=Path,
        help="compiled dedicated server to test (default: <build-dir>/bin/KisakCOD-dedi)",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "clang++"),
        help="32-bit C++ compiler for the built-from-source checks (default: clang++)",
    )
    parser.add_argument(
        "--python",
        default=sys.executable or "python3",
        help="interpreter for the Python checks (default: this interpreter)",
    )
    parser.add_argument(
        "--arch",
        choices=ARCHITECTURES,
        default="i386",
        help="build being checked: i386 (default) runs every check; the 64-bit and macOS builds run "
        "the checks that build for the host's own width (" + ", ".join(NATIVE_CHECKS) + ")",
    )
    parser.add_argument("--timeout", type=int, help="override the timeout of every command, in seconds")
    parser.add_argument("--log-dir", type=Path, help="write one text log per command, plus summary.txt")
    parser.add_argument("--only", help="comma-separated check names to run")
    parser.add_argument("--skip", help="comma-separated check names to leave out")
    parser.add_argument("--list", action="store_true", help="print the command list and exit without running")
    options = parser.parse_args()

    known = ["bootstrap", "runtime", "lifecycle_driver", "handshake_auth", "remote_input", "debugger", "fastfile", "net_deadline", "huffman", "udp", "sys_fileio", "sys_virtualmem", "lexer_eof", "random_range", "gameplay_lifecycle", "server_test_clients", "weapon_def_ownership", "bot_driver", "database_publication", "crt_shims", "assert_continue", "sl_trace", "phys_objid", "perf_ab"]
    options.only = parse_names(options.only)
    options.skip = parse_names(options.skip)
    for name in options.only | options.skip:
        if name not in known:
            parser.error(f"unknown check: {name} (known: {', '.join(known)})")
        if options.arch != "i386" and name in options.only and name not in NATIVE_CHECKS:
            parser.error(f"{name} is an i386 check; --arch {options.arch} runs {', '.join(NATIVE_CHECKS)}")
    if options.timeout is not None and options.timeout <= 0:
        parser.error("--timeout must be positive")

    build_dir = options.build_dir.expanduser().resolve()
    binary = (options.binary or build_dir / "bin" / "KisakCOD-dedi").expanduser().resolve()

    # The plan is built inside a real temporary directory so the printed paths
    # are the ones each command will use.
    with tempfile.TemporaryDirectory(prefix="kisakcod-linux-checks-") as temporary:
        workdir = Path(temporary)
        (workdir / "udp").mkdir()

        checks = plan_checks(options, workdir, build_dir, binary, options.python, options.cxx)
        if options.timeout:
            for check in checks:
                for command in check.commands:
                    command.timeout = options.timeout

        print_plan(checks, build_dir, binary, options.log_dir)

        if options.list:
            print("--list: printed the command list, ran nothing.", flush=True)
            return 0

        if any(check.name == "runtime" for check in checks) and not (build_dir / "compile_commands.json").is_file():
            print(f"FAIL: missing {build_dir / 'compile_commands.json'}; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.", flush=True)
            return 1

        if not shutil.which(options.cxx) and not Path(options.cxx).is_file():
            print(f"FAIL: compiler not found: {options.cxx}", flush=True)
            print("Install the multilib toolchain (clang, g++-multilib) or pass --cxx.", flush=True)
            return 1

        # run_checks needs the real binary path, which lives outside workdir, so
        # binary resolution happens before the temporary directory is created.
        started = time.monotonic()
        results, failures = run_checks(checks, options.log_dir)
        elapsed = time.monotonic() - started
        print(f"elapsed: {elapsed:.1f}s", flush=True)
        return print_summary(results, failures)


if __name__ == "__main__":
    sys.exit(main())
