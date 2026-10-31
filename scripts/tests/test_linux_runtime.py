#!/usr/bin/env python3
"""Build and run the Linux runtime wrapper tests.

The tests themselves live in src/linux/sys_runtime_test.cpp, which includes the
shipped platform sources so the assertions run against the real wrappers. This
script builds that program with the project's own compile flags and drives the
console cases through a pipe, so Sys_ConsoleInput sees exactly the reads a real
stdin would give it.

The final syntax check compiles each runtime translation unit independently;
missing engine declarations are test failures, not accepted exceptions.
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

PROJECT_ROOT = Path(__file__).resolve().parents[2]
LINUX_SRC = PROJECT_ROOT / "src" / "linux"
BUILD_DIR = PROJECT_ROOT / "build-linux"

NOTICE = "Console: input line longer than 1023 characters discarded"

# Each console case is the bytes a caller writes plus the lines the wrapper must
# return, in order. These are the shapes the review called out: several lines in
# one read, a final line with no terminator, CRLF pairs (also split across reads),
# blank lines, a read-boundary split, and over-long input.
CONSOLE_CASES = (
    ("single write carrying two lines", b"status\nquit\n", ["status", "quit"], False),
    ("final line without a terminator", b"first\nlast", ["first", "last"], False),
    ("CRLF pairs", b"status\r\nquit\r\n", ["status", "quit"], False),
    ("blank lines skipped", b"\n\nstatus\n\n", ["status"], False),
    ("line split across the 512-byte read", b"a" * 700 + b"\nstatus\n", ["a" * 700, "status"], False),
    ("CRLF split across reads", b"d" * 511 + b"\r\n" + b"z\n", ["d" * 511, "z"], False),
    ("longest line that still fits", b"c" * 1023 + b"\ntail\n", ["c" * 1023, "tail"], False),
    ("over-long line dropped", b"b" * 1024 + b"\nafter\n", ["after"], True),
    ("over-long final line dropped", b"b" * 1100, [], True),
)


def project_compile_flags():
    """Flags for the platform sources, taken from the project's build if it exists."""
    commands = BUILD_DIR / "compile_commands.json"
    if commands.is_file():
        for entry in json.loads(commands.read_text()):
            if not entry["file"].endswith("src/linux/sys_posix.cpp"):
                continue

            # Keep the entry's flags verbatim and strip only the driver's own
            # arguments: the compiler, the output pair and the source file. A
            # per-token filter would break options that take a separate value.
            tokens = shlex.split(entry["command"])
            flags = []
            skip_next = False
            for token in tokens[1:]:
                if skip_next:
                    skip_next = False
                    continue
                if token in ("-o", "-MT", "-MF", "-MQ"):
                    skip_next = True
                    continue
                if token in ("-c", "-MD", "-MMD") or token == entry["file"]:
                    continue
                flags.append(token)
            return flags, str(commands)
        raise ValueError(f"{commands} has no native Linux sys_posix.cpp compiler entry")

    # Best effort without a configured build: the engine headers still need the
    # include roots, and 32-bit matches the server.
    return (
        [
            "-std=gnu++20",
            "-m32",
            "-D_DEBUG",
            "-DKISAK_MP",
            "-DKISAK_DEDICATED",
            "-DDEDICATED",
            "-D_GNU_SOURCE",
            "-D_FILE_OFFSET_BITS=64",
            "-fms-extensions",
            f"-I{PROJECT_ROOT / 'src'}",
            f"-I{PROJECT_ROOT / 'deps'}",
            "-ffunction-sections",
            "-fdata-sections",
        ],
        "fallback flags (no configured build found)",
    )


def run(command, **kwargs):
    kwargs.setdefault("cwd", BUILD_DIR if BUILD_DIR.is_dir() else PROJECT_ROOT)
    kwargs.setdefault("timeout", 180)
    return subprocess.run(command, check=False, **kwargs)


def build_runtime_test(workdir, compiler="clang++"):
    flags, source = project_compile_flags()
    executable = workdir / "sys_runtime_test"

    compiled = run(
        [
            compiler,
            *flags,
            "-DSYS_RUNTIME_TEST",
            str(LINUX_SRC / "sys_runtime_test.cpp"),
            str(LINUX_SRC / "sys_posix.cpp"),
            "-Wl,--gc-sections",
            "-pthread",
            "-o",
            str(executable),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if compiled.returncode:
        print("failed to build the runtime test:", flush=True)
        print(compiled.stdout, flush=True)
        return None, source

    return executable, source


def run_self_contained_modes(executable):
    """Modes that need no input: directory removal, command line, thread wrappers."""
    failures = 0
    for mode in ("files", "cli", "threads", "render", "render-pending"):
        completed = run(
            [str(executable), mode],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=180,
        )
        print(completed.stdout, end="", flush=True)
        if completed.returncode:
            print(f"  {mode} mode reported failures (exit {completed.returncode})", flush=True)
            failures += 1
    return failures


def run_console_cases(executable):
    """Feed each case through a pipe so the wrapper performs real reads."""
    failures = 0
    for name, data, expected, wants_notice in CONSOLE_CASES:
        completed = run(
            [str(executable), "console", "--expect", str(len(expected))],
            input=data,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=180,
        )
        output = completed.stdout.decode("utf-8", "replace")
        lines = output.splitlines()

        returned = [line[len("LINE:"):] for line in lines if line.startswith("LINE:")]
        extra = [line for line in lines if line.startswith("EXTRA:")]
        notice = any(NOTICE in line for line in lines)

        problems = []
        if completed.returncode != 0:
            problems.append(f"exit status {completed.returncode}")
        if returned != expected:
            problems.append(f"returned {describe(returned)} want {describe(expected)}")
        if extra:
            problems.append(f"returned unexpected extra lines {describe(extra)}")
        if notice != wants_notice:
            problems.append("over-long notice " + ("missing" if wants_notice else "unexpected"))

        if problems:
            print(f"  FAIL console: {name}: {'; '.join(problems)}", flush=True)
            print(indent(output), flush=True)
            failures += 1
        else:
            print(f"  ok console: {name}", flush=True)

    return failures


def describe(lines):
    def shorten(line):
        return line if len(line) <= 24 else f"{line[:12]}...({len(line)} chars)"

    return "[" + ", ".join(repr(shorten(line)) for line in lines) + "]"


def indent(text):
    return "".join("      " + line + "\n" for line in text.splitlines())


def build_and_run_primitive_selftest(workdir, compiler="g++"):
    """The engine-free primitives have their own standalone self-test."""
    executable = workdir / "sys_selftest"
    compiled = run(
        [
            compiler,
            "-std=c++20",
            "-DSYS_POSIX_SELFTEST",
            "-pthread",
            str(LINUX_SRC / "sys_posix.cpp"),
            str(LINUX_SRC / "sys_selftest.cpp"),
            "-o",
            str(executable),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if compiled.returncode:
        print("failed to build the primitive self-test:", flush=True)
        print(compiled.stdout, flush=True)
        return 1

    completed = run([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    print(completed.stdout, end="", flush=True)
    return 0 if completed.returncode == 0 else 1


def check_translation_units(compiler="clang++"):
    """Syntax-check the platform sources and report build limits separately."""
    flags, _ = project_compile_flags()

    clean = []
    for source in ("sys_posix.cpp", "sys_console.cpp", "sys_files.cpp", "sys_threads.cpp", "main.cpp"):
        completed = run(
            [compiler, *flags, "-fsyntax-only", str(LINUX_SRC / source)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60,
        )
        if completed.returncode:
            print(f"  FAIL {source}: translation unit does not compile:", flush=True)
            print(indent(completed.stdout), flush=True)
            return 1
        clean.append(source)

    print(f"  compiles cleanly: {', '.join(clean)}", flush=True)
    return 0


def main():
    global BUILD_DIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++", help="C++ compiler for the runtime tests")
    parser.add_argument("--build-dir", type=Path, default=BUILD_DIR,
                        help="configured CMake directory containing compile_commands.json")
    parser.add_argument("--skip-build-check", action="store_true", help="skip the per-file syntax check")
    arguments = parser.parse_args()
    BUILD_DIR = arguments.build_dir.resolve()
    try:
        project_compile_flags()
    except (ValueError, OSError, KeyError) as error:
        parser.error(str(error))

    print("Linux runtime wrapper tests", flush=True)
    print("===========================", flush=True)

    failures = 0

    with tempfile.TemporaryDirectory(prefix="kisakcod-runtime-") as temporary:
        workdir = Path(temporary)

        executable, flag_source = build_runtime_test(workdir, arguments.compiler)
        if executable is None:
            return 1
        print(f"built runtime test with {flag_source}", flush=True)

        print("\nengine-free primitives", flush=True)
        failures += build_and_run_primitive_selftest(workdir, arguments.compiler)

        print("\nengine-facing wrappers", flush=True)
        failures += run_self_contained_modes(executable)

        print("\nconsole input through a pipe", flush=True)
        failures += run_console_cases(executable)

    if not arguments.skip_build_check:
        print("\ntranslation unit build check", flush=True)
        failures += check_translation_units(arguments.compiler)

    print(flush=True)
    if failures:
        print(f"{failures} check group(s) failed", flush=True)
        return 1

    print("all Linux runtime checks passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
