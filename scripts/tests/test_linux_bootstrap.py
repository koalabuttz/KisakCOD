#!/usr/bin/env python3
"""Smoke-test the native Linux dedicated server binary.

The build produces an asset-free ELF32 i386 executable at bin/KisakCOD-dedi. No
game data is needed to prove it is the right artefact and that it reaches its own
startup code, so every check here works from the binary alone:

  * the header bytes say ELF, 32-bit, little-endian, machine 3 (i386);
  * --help and --version answer without touching the engine;
  * an asset-free launch reaches FS_Startup and then fails on the missing
    fileSysCheck.cfg, which is the expected end state for a data-less tree;
  * `+set useFastFile 0` is refused with the server-only diagnostic before the
    filesystem starts;
  * an over-long command line is refused with a length diagnostic instead of
    being truncated silently.

The engine writes its console log relative to the working directory, so every
launch runs with cwd set to a temporary directory: nothing is left behind in the
checkout and the temporary directory is removed afterwards, taking the log with
it.

Usage:
    python3 scripts/tests/test_linux_bootstrap.py [path/to/binary]
"""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = PROJECT_ROOT / "bin" / "KisakCOD-dedi"

# ELF identification indices and header offsets this test reads.
EI_CLASS = 4
EI_DATA = 5
EHDR_MACHINE_OFFSET = 18
ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
EM_386 = 3

HELP_TIMEOUT = 10
LAUNCH_TIMEOUT = 30

# Strings the engine prints once it actually starts, as opposed to the early
# command-line exits. --help and --version must not produce any of them.
ENGINE_START_MARKERS = (
    "KisakCoD4 1.0 build",
    "begin $init",
    "FS_Startup",
    "Couldn't load fileSysCheck.cfg",
)

FILE_SYS_CHECK = "Couldn't load fileSysCheck.cfg"
FASTFILE_REJECTION = "requires useFastFile 1"
CMDLINE_TOO_LONG = "command line too long"


class Checker:
    """Collects results so one failing check does not hide the others."""

    def __init__(self):
        self.failures = 0

    def passed(self, message):
        print(f"PASS: {message}", flush=True)

    def failed(self, message):
        print(f"FAIL: {message}", flush=True)
        self.failures += 1

    def require(self, condition, description, detail=None):
        if condition:
            self.passed(description)
        else:
            self.failed(description if detail is None else f"{description} -- {detail}")
        return bool(condition)


def run(binary, arguments, cwd, timeout):
    """Run the binary and return (returncode, combined output, timed out)."""
    command = [str(binary), *arguments]
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as expired:
        output = expired.output or b""
        if isinstance(output, str):
            output = output.encode("utf-8", "replace")
        return None, output.decode("utf-8", "replace"), True
    except OSError as error:
        # A binary the kernel refuses to run (wrong architecture, no exec
        # permission) is a test failure with an explanation, not a traceback.
        return None, f"could not execute {binary}: {error}", False

    return completed.returncode, completed.stdout.decode("utf-8", "replace"), False


def check_elf_header(checker, binary):
    """Read the header bytes directly: no objdump or `file` involved."""
    try:
        with binary.open("rb") as handle:
            header = handle.read(64)
    except OSError as error:
        checker.failed(f"read {binary}: {error}")
        return

    if len(header) < 20:
        checker.failed(f"{binary} is too short to hold an ELF header ({len(header)} bytes)")
        return

    checker.require(header[:4] == ELF_MAGIC, "ELF magic 0x7f 'E' 'L' 'F'")
    checker.require(
        header[EI_CLASS] == ELFCLASS32,
        "ELFCLASS32 (EI_CLASS == 1)",
        f"EI_CLASS is {header[EI_CLASS]}",
    )
    checker.require(
        header[EI_DATA] == ELFDATA2LSB,
        "ELFDATA2LSB (EI_DATA == 1)",
        f"EI_DATA is {header[EI_DATA]}",
    )

    machine = struct.unpack_from("<H", header, EHDR_MACHINE_OFFSET)[0]
    checker.require(machine == EM_386, "e_machine EM_386 (3, i386)", f"e_machine is {machine}")


def no_engine_start(checker, output, label):
    found = [marker for marker in ENGINE_START_MARKERS if marker in output]
    checker.require(
        not found,
        f"{label} does not start the engine",
        "engine output present: " + ", ".join(repr(marker) for marker in found),
    )


def check_help(checker, binary, workdir):
    code, output, timed_out = run(binary, ["--help"], workdir, HELP_TIMEOUT)

    if timed_out:
        checker.failed(f"--help did not exit within {HELP_TIMEOUT}s")
        return

    checker.require(code == 0, f"--help exits 0 (got {code})", last_lines(output))
    checker.require(
        "native Linux" in output,
        "--help names the native Linux server",
        last_lines(output),
    )
    checker.require("Usage:" in output, "--help prints usage", last_lines(output))
    # A usage dump is not a server start; if it prints engine output the early
    # exit has regressed into a real run.
    no_engine_start(checker, output, "--help")


def check_version(checker, binary, workdir):
    code, output, timed_out = run(binary, ["--version"], workdir, HELP_TIMEOUT)

    if timed_out:
        checker.failed(f"--version did not exit within {HELP_TIMEOUT}s")
        return

    checker.require(code == 0, f"--version exits 0 (got {code})", last_lines(output))
    checker.require(output.strip() != "", "--version prints a non-empty banner")
    no_engine_start(checker, output, "--version")


def check_asset_free_launch(checker, binary, workdir):
    """Start with no game data: FS starts, then fileSysCheck.cfg is missing."""
    arguments = [
        "+set", "fs_basepath", str(workdir),
        "+set", "fs_homepath", str(workdir),
        "+set", "net_ip", "127.0.0.1",
        "+set", "net_noudp", "1",
    ]
    code, output, timed_out = run(binary, arguments, workdir, LAUNCH_TIMEOUT)

    if timed_out:
        checker.failed(f"asset-free launch did not exit within {LAUNCH_TIMEOUT}s")
        return

    # Signals surface as negative return codes; an exited server reports 1.
    checker.require(
        code == 1,
        "asset-free launch exits 1 rather than crashing or being signalled",
        f"returncode {code}",
    )
    checker.require(
        "KisakCoD4 1.0 build" in output and "linux-x86" in output,
        "launch prints the KisakCoD4 build banner for linux-x86",
        last_lines(output),
    )
    checker.require("FS_Startup" in output, "launch reaches FS_Startup", last_lines(output))
    checker.require(
        FILE_SYS_CHECK in output,
        "launch reports the missing fileSysCheck.cfg",
        last_lines(output),
    )
    # Reaching the filesystem error means the run went past the CLI and into the
    # engine; the build banner also rules out a --help style early exit.
    checker.require(
        "FS_Startup" in output and "KisakCoD4 1.0 build" in output,
        "launch performed a real startup rather than an early exit",
    )
    checker.require(
        (workdir / "main" / "console_mp.log").is_file(),
        "console log uses a native directory path",
        f"temporary root entries: {[entry.name for entry in workdir.iterdir()]}",
    )
    checker.require(
        not any("\\" in str(entry.relative_to(workdir)) for entry in workdir.rglob("*")),
        "filesystem startup creates no literal backslash path names",
    )


def check_fastfile_rejected(checker, binary, workdir):
    """Server-only builds must refuse loose-asset loading before FS starts."""
    arguments = [
        "+set", "fs_basepath", str(workdir),
        "+set", "fs_homepath", str(workdir),
        "+set", "net_ip", "127.0.0.1",
        "+set", "net_noudp", "1",
        "+set", "useFastFile", "0",
    ]
    code, output, timed_out = run(binary, arguments, workdir, LAUNCH_TIMEOUT)

    if timed_out:
        checker.failed(f"useFastFile 0 launch did not exit within {LAUNCH_TIMEOUT}s")
        return

    checker.require(
        code == 1,
        "useFastFile 0 is refused with exit 1",
        f"returncode {code}",
    )
    checker.require(
        FASTFILE_REJECTION in output,
        "useFastFile 0 prints the requires-useFastFile-1 diagnostic",
        last_lines(output),
    )
    # Com_InitDvars refuses before FS_InitFilesystem, so the filesystem never
    # starts on this path.
    checker.require(
        "FS_Startup" not in output,
        "useFastFile 0 is rejected before FS_Startup",
        last_lines(output),
    )


def check_oversized_command_line(checker, binary):
    """An argument that cannot fit must be refused, not silently truncated."""
    with tempfile.TemporaryDirectory(prefix="kisakcod-bootstrap-cmdline-") as temporary:
        workdir = Path(temporary)
        code, output, timed_out = run(binary, ["+set", "x" * 4000], workdir, HELP_TIMEOUT)

        if timed_out:
            checker.failed(f"oversized command line did not exit within {HELP_TIMEOUT}s")
            return

        checker.require(
            code == 1,
            "oversized command line exits 1",
            f"returncode {code}",
        )
        checker.require(
            CMDLINE_TOO_LONG in output,
            "oversized command line reports the length problem",
            last_lines(output),
        )
        checker.require(
            re.search(r"fit in \d+ bytes", output) is not None,
            "oversized command line diagnostic states the byte limit",
            last_lines(output),
        )
        # The refusal happens before any subsystem starts, so the engine must not
        # have written its console log into the working directory.
        leftovers = sorted(path.name for path in workdir.iterdir())
        checker.require(
            not leftovers,
            "oversized command line writes no engine files",
            f"found {leftovers}",
        )


def last_lines(output, count=8):
    """Short tail of the captured output for failure messages."""
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        return "no output"
    tail = lines[-count:]
    return " | ".join(tail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary",
        nargs="?",
        default=str(DEFAULT_BINARY),
        help="dedicated server binary (default: bin/KisakCOD-dedi)",
    )
    arguments = parser.parse_args()

    binary = Path(arguments.binary).expanduser().resolve()

    print("Native Linux dedicated server bootstrap smoke test", flush=True)
    print("===================================================", flush=True)
    print(f"binary: {binary}", flush=True)

    if not binary.is_file():
        print(f"FAIL: binary not found at {binary}", flush=True)
        return 1

    checker = Checker()

    print("\nbinary format", flush=True)
    check_elf_header(checker, binary)

    print("\ncommand-line exits", flush=True)
    with tempfile.TemporaryDirectory(prefix="kisakcod-bootstrap-cli-") as temporary:
        cli_dir = Path(temporary)
        # Anything the kernel refuses to run fails here once, rather than
        # repeating the same error for every launch below.
        probe_code, probe_output, probe_timeout = run(binary, ["--version"], cli_dir, HELP_TIMEOUT)
        if probe_code is None and not probe_timeout:
            print(f"FAIL: {probe_output}", flush=True)
            print("\n1 bootstrap check(s) failed", flush=True)
            return 1

        check_help(checker, binary, cli_dir)
        check_version(checker, binary, cli_dir)
        check_oversized_command_line(checker, binary)

    print("\nengine startup without assets", flush=True)
    with tempfile.TemporaryDirectory(prefix="kisakcod-bootstrap-run-") as temporary:
        run_dir = Path(temporary)
        check_asset_free_launch(checker, binary, run_dir)
        check_fastfile_rejected(checker, binary, run_dir)

    print(flush=True)
    if checker.failures:
        print(f"{checker.failures} bootstrap check(s) failed", flush=True)
        return 1

    print("all native Linux bootstrap checks passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
