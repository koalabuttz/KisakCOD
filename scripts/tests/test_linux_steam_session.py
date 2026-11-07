#!/usr/bin/env python3
"""Asset-free, Steam-free checks of the opt-in Linux Steam backend.

KISAK_LINUX_STEAM builds src/linux/steam.cpp's Steam backend, which talks to
the Steam client running on the player's machine through its steamclient.so
(src/linux/steam/steamclient_session.cpp). This test never looks for Steam.

1. Session test. tests/linux_steam/fake_steamclient.cpp is built as a stand-in
   steamclient.so that serves this SDK's ISteamClient and ISteamUser and records
   every call. tests/linux_steam/steamclient_session_test.cpp drives the real
   Session code against it: library lookup, every way opening can fail and what
   is released, the app id handed to Steam, ticket requests bound to the
   server's address, BeginAuthSession, and callback reading (verdicts passed
   on, every callback released). Built 64-bit and, when the toolchain can,
   32-bit (the shipped target); --m32 makes the 32-bit run a requirement. With
   sanitizers unless the toolchain lacks them or --no-sanitize is given.

2. Compile proof (32-bit, front end only). The engine units the flag changes
   are compiled with KISAK_LINUX_STEAM, in the dedicated server's definitions
   and in the multiplayer client's, so a flag-on build cannot rot unnoticed
   while CI builds the default. Like the client compile census it reuses, this
   needs Clang (the engine headers use Clang/MSVC extensions); --cxx g++ runs
   the session test but the proof then fails.

Usage:
    python3 scripts/tests/test_linux_steam_session.py
    python3 scripts/tests/test_linux_steam_session.py --cxx clang++ --m32
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
DEPS = ROOT / "deps"
TESTS = ROOT / "tests" / "linux_steam"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_client_compile_census as census  # noqa: E402

SESSION = SRC / "linux" / "steam" / "steamclient_session.cpp"
FAKE = TESTS / "fake_steamclient.cpp"
TEST = TESTS / "steamclient_session_test.cpp"
WARNINGS = ("-Wall", "-Wextra", "-Werror")
SANITIZE = ("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all")

# The units KISAK_LINUX_STEAM changes, per target.
DEDICATED_DEFINES = ("-DKISAK_MP", "-DKISAK_DEDICATED", "-DDEDICATED", "-D_FILE_OFFSET_BITS=64", "-D_GNU_SOURCE")
DEDICATED_UNITS = ("linux/steam.cpp", "linux/main.cpp", "server_mp/sv_client_mp.cpp")
CLIENT_DEFINES = tuple(define for define in census.CLIENT_DEFINES) + ("-DKISAK_LINUX_MP_CLIENT",)
CLIENT_UNITS = ("linux/steam.cpp", "linux/main.cpp", "client_mp/cl_main_pc_mp.cpp", "client_mp/cl_auth.cpp")


def run(argv, timeout=300):
    completed = subprocess.run([str(part) for part in argv], cwd=ROOT, capture_output=True, text=True,
                               timeout=timeout, check=False)
    return completed.returncode, completed.stdout + completed.stderr


def build_session_test(cxx, bits, sanitize, workdir):
    """Build the stand-in library and the test; returns (ok, sanitized, log)."""
    arch = ("-m32",) if bits == 32 else ()
    fake = workdir / f"fake_steamclient{bits}.so"
    test = workdir / f"steam_session_test{bits}"
    code, log = run([cxx, *arch, "-std=c++20", "-O1", "-g", *WARNINGS, "-fPIC", "-shared",
                     "-fvisibility=hidden", "-I", DEPS, "-I", TESTS, "-o", fake, FAKE])
    if code:
        return False, False, f"stand-in steamclient.so ({bits}-bit) failed to build:\n{log}", None, None
    base = [cxx, *arch, "-std=c++20", "-O1", "-g", *WARNINGS, "-I", SRC, "-I", DEPS, "-I", TESTS,
            "-o", test, TEST, SESSION, "-ldl"]
    if sanitize:
        code, log = run(base[:1] + list(SANITIZE) + base[1:])
        if not code:
            return True, True, "", fake, test
        print(f"note: no sanitizer runtime for {bits}-bit; building unsanitized")
    code, log = run(base)
    if code:
        return False, False, f"session test ({bits}-bit) failed to build:\n{log}", None, None
    return True, False, "", fake, test


def compile_proofs(cxx, workdir):
    """Front-end compile of the flag-on units; returns a list of failure texts."""
    generated = workdir / "generated"
    generated.mkdir(exist_ok=True)
    census.generate_build_header(shutil.which("cmake") or "cmake", generated)
    failures = []
    for name, defines, units in (("dedicated", DEDICATED_DEFINES, DEDICATED_UNITS),
                                 ("client", CLIENT_DEFINES, CLIENT_UNITS)):
        for unit in units:
            status, argv, output = census.compile_one(
                cxx, SRC / unit, census.DEFAULT_TIMEOUT, generated, defines + ("-DKISAK_LINUX_STEAM",))
            print(f"compile proof [{name}] {unit}: {status}")
            if status != "ok":
                failures.append(f"{name} {unit}: {status}\n{' '.join(argv)}\n{output[-4000:]}")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--m32", action="store_true", help="fail if the 32-bit build cannot be produced")
    parser.add_argument("--no-sanitize", action="store_true")
    parser.add_argument("--skip-compile-proof", action="store_true")
    options = parser.parse_args(argv)

    failures = []
    ran = []
    with tempfile.TemporaryDirectory(prefix="kisakcod-steam-") as temporary:
        workdir = Path(temporary)
        for bits in (64, 32):
            ok, sanitized, log, fake, test = build_session_test(options.cxx, bits, not options.no_sanitize, workdir)
            if not ok:
                if bits == 32 and not options.m32:
                    print(f"note: skipping the 32-bit session test:\n{log}")
                    continue
                failures.append(log)
                continue
            code, output = run([test, fake], timeout=120)
            print(output.rstrip())
            ran.append(f"session test {bits}-bit{' sanitized' if sanitized else ''}: "
                       f"{'pass' if code == 0 else 'FAIL'}")
            if code:
                failures.append(f"session test ({bits}-bit) exited {code}")
        if not options.skip_compile_proof:
            proof_failures = compile_proofs(options.cxx, workdir)
            failures.extend(proof_failures)
            ran.append(f"compile proof: {'pass' if not proof_failures else 'FAIL'}")

    for line in ran:
        print(line)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        print("FAIL")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
