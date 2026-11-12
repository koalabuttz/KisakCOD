#!/usr/bin/env python3
"""Check the opt-in speed build options: KISAK_LINUX_CPU, KISAK_LINUX_LTO and no assertions.

Asset-free, and nothing is built: a source check, two compiles to assembly
and two CMake configures into a temporary directory.

  * Clang really does fuse ``a * b + c`` into one instruction at the CPU level
    the test asks for, and ``-ffp-contract=off`` really does stop it. This is
    the reason KISAK_LINUX_CPU adds that flag: a fused multiply-add rounds once
    where the baseline build rounds twice, and client prediction has to land
    on the server's floats.
  * With both options on, every compile command of the build directory carries
    ``-march=<level>``, ``-ffp-contract=off`` (after ``-march``, and never
    turned back on later in the command) and ``-flto=thin``, and every link
    uses lld.
  * With neither option, no compile command carries ``-march`` or ``-flto``
    and no link names a linker: the default build is unchanged.
  * A build without assertions (KISAK_LINUX_CLIENT_ASSERTS=OFF, as Windows
    release builds are) drops whatever an ``iassert`` does. The marks code
    counts its users of ``g_markThread`` inside ``iassert``, so every change
    of that count has to be inside one, or the count only goes one way: laptop
    perf run 6's build without assertions stopped at map load on exactly that.

Usage:
    python3 scripts/tests/test_linux_speed_build.py [--arch i386|x86_64]

Requires cmake, ninja and clang; ``--arch i386`` also needs the multilib
toolchain the i386 build needs.
"""

import argparse
import json
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPU_LEVEL = "x86-64-v3"

PROBE = """\
float MulAdd(float a, float b, float c) { return a * b + c; }
double MulSub(double a, double b, double c) { return c - a * b; }
"""


def fail(message):
    print(f"FAIL: {message}", flush=True)
    sys.exit(1)


def run(argv, **kwargs):
    result = subprocess.run([str(part) for part in argv], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", **kwargs)
    if result.returncode != 0:
        print(result.stdout, flush=True)
        fail(f"exit status {result.returncode}: {' '.join(shlex.quote(str(part)) for part in argv)}")
    return result.stdout


def fused_count(cxx, workdir, arch_flags, extra):
    """How many fused multiply-add instructions the probe compiles to."""
    source = workdir / "fused_probe.cpp"
    source.write_text(PROBE)
    assembly = run([cxx, *arch_flags, "-O2", f"-march={CPU_LEVEL}", *extra, "-S", "-o", "-", source])
    return sum(1 for line in assembly.splitlines()
               if line.strip().startswith(("vfmadd", "vfmsub", "vfnmadd", "vfnmsub")))


def configure(workdir, name, arch, cc, cxx, extra):
    build_dir = workdir / name
    run(["cmake", "-S", ROOT, "-B", build_dir, "-G", "Ninja",
         "-DKISAK_PLATFORM=linux", f"-DKISAK_LINUX_ARCH={arch}",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *extra])
    commands = [shlex.split(entry["command"])
                for entry in json.loads((build_dir / "compile_commands.json").read_text())]
    if len(commands) < 100:
        fail(f"{name}: only {len(commands)} compile commands; the server's sources are missing")
    links = [line for line in (build_dir / "build.ninja").read_text().splitlines()
             if line.lstrip().startswith("LINK_FLAGS")]
    if not links:
        fail(f"{name}: no link lines found in build.ninja")
    return commands, links


def check_mark_counter():
    path = ROOT / "src" / "EffectsCore" / "fx_marks.cpp"
    changes = 0
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if re.search(r"Interlocked(Increment|Decrement)\b.*g_markThread", line):
            changes += 1
            if not line.strip().startswith("iassert("):
                fail(f"{path.relative_to(ROOT)}:{number} changes g_markThread outside iassert, "
                     "so a build without assertions keeps only the other half")
    if changes < 2:
        fail(f"found {changes} changes of g_markThread in {path.relative_to(ROOT)}; the check is stale")
    print(f"ok: all {changes} changes of g_markThread are inside iassert", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--arch", choices=("i386", "x86_64"), default="i386")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    options = parser.parse_args()
    arch_flags = ["-m32", "-msse2", "-mfpmath=sse"] if options.arch == "i386" else ["-m64"]
    check_mark_counter()

    with tempfile.TemporaryDirectory(prefix="kisak-speed-build-") as temporary:
        workdir = Path(temporary)

        fused = fused_count(options.cxx, workdir, arch_flags, [])
        if fused == 0:
            fail(f"-march={CPU_LEVEL} alone did not fuse a multiply-add, so this test "
                 "cannot tell whether -ffp-contract=off works")
        kept = fused_count(options.cxx, workdir, arch_flags, ["-ffp-contract=off"])
        if kept != 0:
            fail(f"-ffp-contract=off left {kept} fused multiply-add instruction(s)")
        print(f"ok: -march={CPU_LEVEL} fuses {fused} multiply-adds of the probe, "
              "-ffp-contract=off none", flush=True)

        commands, links = configure(workdir, "speed", options.arch, options.cc, options.cxx,
                                    [f"-DKISAK_LINUX_CPU={CPU_LEVEL}", "-DKISAK_LINUX_LTO=ON"])
        march = f"-march={CPU_LEVEL}"
        for command in commands:
            shown = command[-1] if command else "?"
            if march not in command:
                fail(f"speed: no {march} in the compile command of {shown}")
            contract = [part for part in command if part.startswith("-ffp-contract=")]
            if not contract or contract[-1] != "-ffp-contract=off":
                fail(f"speed: {shown} is compiled with {contract or 'no -ffp-contract flag'}; "
                     "the last one must be -ffp-contract=off")
            if any(part in ("-ffast-math", "-Ofast", "-funsafe-math-optimizations") for part in command):
                fail(f"speed: {shown} is compiled with a fast-math flag")
            if "-flto=thin" not in command:
                fail(f"speed: no -flto=thin in the compile command of {shown}")
        for line in links:
            if "-fuse-ld=lld" not in line:
                fail(f"speed: a link does not use lld: {line.strip()}")
        print(f"ok: both options on: {len(commands)} compile commands carry {march}, "
              f"-ffp-contract=off and -flto=thin; {len(links)} links use lld", flush=True)

        commands, links = configure(workdir, "default", options.arch, options.cc, options.cxx, [])
        for command in commands:
            for part in command:
                if part.startswith(("-march=", "-flto")):
                    fail(f"default: {command[-1]} is compiled with {part}")
        for line in links:
            if "-fuse-ld=" in line or "-flto" in line:
                fail(f"default: a link carries a speed option: {line.strip()}")
        print(f"ok: neither option: {len(commands)} compile commands and {len(links)} links "
              "carry none of the speed flags", flush=True)

    print("PASS", flush=True)


if __name__ == "__main__":
    main()
