#!/usr/bin/env python3
"""Compile and run the asset-free checks of the native DXVK D3D9 boundary.

The boundary is ``src/linux/d3d9_native.h``, reached through
``<linux/d3d9_types.h>`` when a translation unit is compiled with
``KISAK_LINUX_NATIVE_D3D9`` (see docs/linux-native-d3d9.md). This runner needs
the DXVK Native **headers** and nothing else from DXVK: no DXVK build, no
``libdxvk_d3d9.so``, no Vulkan, no SDL, no GPU, no game assets, no Steam client
and no CMake configure. Every check compiles explicit command lines with a 32-bit
C++20 compiler.

Checks, per configuration:

  * ``boundary_standin-<cc>`` / ``boundary_native-<cc>`` -- for each header
    compiler (g++ and clang++ by default), ``tests/linux_d3d9_boundary/
    boundary_header_check.cpp`` built with ``-Wall -Wextra -Werror`` in each mode
    and run. The binary is then disassembled: the engine's ``__stdcall``,
    ``WINAPI`` and ``CALLBACK`` probes must end in ``ret $0x8`` (callee pops its
    two arguments) and the cdecl probe in a plain ``ret``. In native mode a class
    implementing DXVK's ``IDirect3DQuery9`` is called through the DXVK vtable, and
    its ``Issue`` method must also end in a plain ``ret``: DXVK Native's COM
    convention on i386, which is what ``libdxvk_d3d9.so`` implements.
  * ``reject_*-<cc>`` -- four sources that must **not** compile, each with the
    reason it must fail: including ``d3d9_native.h`` directly, including
    ``<d3d9.h>`` ahead of the boundary, including DXVK next to ``compat.h`` with
    no firewall (the conflict the boundary exists for), and calling a D3D9 method
    without the opt-in.
  * ``engine_layout_standin`` / ``engine_layout_native`` / ``engine_layout_compare``
    -- the D3D9-bearing engine structures (GfxImage, MaterialVertexDeclaration,
    DxGlobals, GfxCmdBufState, ...) checked against their documented x86-32 sizes
    in both views, and the two printed layouts required to be identical.
  * ``renderer_proof-<unit>`` -- each production renderer unit in
    ``scripts/platform/linux/native_d3d9_renderer_sources.txt`` compiled
    unmodified (``-c``) through the boundary to an ELF32 i386 object.
  * ``renderer_standin_control`` -- the same units must fail with the stand-in, so
    the list stays a list of units that actually need the D3D9 API.

The engine-header checks use ``--engine-cxx`` (clang++ by default, the compiler
the engine builds with), ``-std=gnu++20`` as CMake gives every engine target, and
the flags of ``KisakCOD-client-core`` without Tracy, which needs a network fetch.
The boundary's own test sources are compiled as strict ``-std=c++20``. The CMake
targets in ``scripts/platform/linux/native_d3d9.cmake`` build the same proofs with
Tracy.

DXVK headers come from either ``--dxvk-include-dir`` (an installed
``<prefix>/include/dxvk``) or ``--dxvk-source`` (a DXVK source checkout with the
``include/native/directx`` submodule), which is staged into the work directory
exactly as DXVK's meson install lays it out. With a source checkout the version
in ``meson.build`` must be 2.7.1, the validated release, unless
``--allow-dxvk-version`` says otherwise.

What this runner does not claim
-------------------------------
A pass says the boundary compiles, keeps the engine's conventions and layouts,
and that the listed renderer units compile through it. It does not claim that a
device was created, that anything was drawn, that the units link, or that the
hosted workflow ran: the workflow invokes this script and is not verified by it.

Usage:
    python3 scripts/tests/run_native_d3d9_boundary_checks.py --config Debug \\
        --dxvk-source "$RUNNER_TEMP/dxvk-source"
    python3 scripts/tests/run_native_d3d9_boundary_checks.py --config RelWithDebInfo \\
        --dxvk-include-dir /tmp/kisak-dxvk-prefix/include/dxvk --log-dir artifacts/d3d9
    python3 scripts/tests/run_native_d3d9_boundary_checks.py --list
    python3 scripts/tests/run_native_d3d9_boundary_checks.py \\
        --dxvk-source "$RUNNER_TEMP/dxvk-source" --stage-dxvk-headers "$RUNNER_TEMP/dxvk-include/dxvk"
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]

CONFIG_FLAGS = {
    "Debug": ("-O0",),
    "RelWithDebInfo": ("-O2",),
}
# Engine units also get the definitions their CMake targets add per configuration.
ENGINE_CONFIG_DEFINES = {
    "Debug": ("-D_DEBUG",),
    "RelWithDebInfo": (),
}

TARGET_MACHINE = 3  # EM_386
ELF_TYPE_REL = 1
ELF_TYPE_EXEC = 2
ELF_TYPE_DYN = 3

NATIVE_DEFINE = "-DKISAK_LINUX_NATIVE_D3D9"
# The boundary's own test sources are held to strict ISO C++20.
BASE_FLAGS = ("-m32", "-std=c++20", "-g", "-fms-extensions")
# Engine units get what CMake gives every engine target (cxx_std_20 with compiler
# extensions on): gnu++20. It matters: only the GNU dialect predefines `linux`,
# which deps/binklib/radbase.h uses to detect the platform.
ENGINE_BASE_FLAGS = ("-m32", "-std=gnu++20", "-g", "-fms-extensions")
# The boundary's own test sources are held to every warning.
STRICT_FLAGS = ("-Wall", "-Wextra", "-Werror")
# KisakCOD-client-core's definitions and options, minus Tracy.
ENGINE_DEFINES = ("-DKISAK_MP", "-DKISAK_OPENAL", "-D_FILE_OFFSET_BITS=64", "-D_GNU_SOURCE")
ENGINE_FLAGS = (
    "-msse2",
    "-mfpmath=sse",
    "-fno-strict-aliasing",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wno-c++11-narrowing",
    "-Wno-invalid-token-paste",
    "-Wno-ignored-attributes",
    "-Wno-format-security",
)

TEST_DIR = "tests/linux_d3d9_boundary"
HEADER_CHECK = f"{TEST_DIR}/boundary_header_check.cpp"
LAYOUT_CHECK = f"{TEST_DIR}/engine_layout_check.cpp"
PROOF_LIST = "scripts/platform/linux/native_d3d9_renderer_sources.txt"

REQUIRED_DXVK_HEADERS = ("d3d9.h", "d3d9types.h", "d3d9caps.h", "windows.h", "windows_base.h", "unknwn.h")
EXPECTED_DXVK_VERSION = "2.7.1"
DXVK_WSI_HEADERS = ("native_wsi.h", "native_sdl3.h", "native_sdl2.h", "native_glfw.h")

# Probe symbol -> the only return instruction its body may contain.
STDCALL_RET = "ret $0x8"
CDECL_RET = "ret"
CONVENTION_PROBES = {
    "kisak_d3d9_probe_stdcall": STDCALL_RET,
    "kisak_d3d9_probe_winapi": STDCALL_RET,
    "kisak_d3d9_probe_callback": STDCALL_RET,
    "kisak_d3d9_probe_cdecl": CDECL_RET,
}
# ProbeQuery::Issue(unsigned int): a DXVK-declared COM method implemented locally.
NATIVE_COM_PROBES = {
    "_ZN10ProbeQuery5IssueEj": CDECL_RET,
}

# (name, source, needs native mode, compilers' output must match this)
REJECT_CASES = (
    (
        "reject_direct_native_include",
        f"{TEST_DIR}/reject_direct_native_include.cpp",
        True,
        r"the single D3D9 entry point",
    ),
    (
        "reject_d3d9_before_boundary",
        f"{TEST_DIR}/reject_d3d9_before_boundary.cpp",
        True,
        r"a D3D9 header was included before the boundary",
    ),
    (
        "reject_unfirewalled_dxvk",
        f"{TEST_DIR}/reject_unfirewalled_dxvk.cpp",
        True,
        r"conflicting declaration .*HWND|typedef redefinition with different types",
    ),
    (
        "reject_standin_method_call",
        f"{TEST_DIR}/reject_standin_method_call.cpp",
        False,
        r"incomplete type",
    ),
)

FAILURE_MARKER = "FAIL"
DEFAULT_RUN_TIMEOUT = 120
DEFAULT_COMPILE_TIMEOUT = 600


class Step:
    """One subprocess and what its outcome must be."""

    def __init__(self, argv, cwd, timeout, kind, expect_failure=None):
        self.argv = [str(part) for part in argv]
        self.cwd = Path(cwd)
        self.timeout = timeout
        # compile / run / disassemble / reject: used for log names and messages.
        self.kind = kind
        # None: must exit 0. A regex: must exit non-zero (not time out) and print
        # text matching it. "" means any non-zero exit is the expected outcome.
        self.expect_failure = expect_failure

    def describe(self):
        return " ".join(shlex.quote(part) for part in self.argv)


class Check:
    """A named check: its steps in order, and a verifier over their outputs."""

    def __init__(self, name, description, steps, verify=None, artifact=None, elf_type=None):
        self.name = name
        self.description = description
        self.steps = steps
        # verify(outputs, results) -> (ok, detail); outputs are the steps' texts.
        self.verify = verify
        # A file the check produces that must be ELF32/i386 of elf_type.
        self.artifact = artifact
        self.elf_type = elf_type


def compiler_tag(cxx):
    """A short, file-name-safe label for a compiler: g++ -> gcc, clang++ -> clang."""
    base = Path(cxx).name
    if "clang" in base:
        return "clang"
    if base.startswith("g++") or base.startswith("gcc") or base.endswith("-g++"):
        return "gcc"
    return re.sub(r"[^A-Za-z0-9]+", "_", base).strip("_") or "cxx"


def read_proof_list(repo):
    """The renderer units of the compile proof, as repository-relative paths."""
    path = repo / PROOF_LIST
    units = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        units.append(line)
    return units


def parse_disassembly(text):
    """Map each function symbol in ``objdump -d`` output to its return instructions."""
    functions = {}
    current = None
    header = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    for line in text.splitlines():
        match = header.match(line.strip())
        if match:
            current = match.group(1)
            functions.setdefault(current, [])
            continue
        if current is None:
            continue
        if not line.strip():
            current = None
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        instruction = " ".join(parts[-1].split())
        if instruction == "ret" or instruction.startswith("ret "):
            functions[current].append(instruction)
    return functions


def check_conventions(disassembly, expectations):
    """Every probe exists and each of its returns is the expected instruction."""
    functions = parse_disassembly(disassembly)
    problems = []
    for symbol, expected in expectations.items():
        returns = functions.get(symbol)
        if returns is None:
            problems.append(f"{symbol}: not found in the disassembly")
        elif not returns:
            problems.append(f"{symbol}: no return instruction found")
        elif any(found != expected for found in returns):
            problems.append(f"{symbol}: returns with {sorted(set(returns))}, expected '{expected}'")
    return problems


def compare_layouts(standin_output, native_output):
    """Both layout prints, minus their one mode line, must be identical and passing."""
    def body(text):
        lines = [line for line in text.splitlines() if line.strip()]
        if not lines or not lines[0].startswith("# "):
            return None
        return lines[1:]

    standin, native = body(standin_output), body(native_output)
    if standin is None or native is None:
        return False, "a layout print is missing its header line"
    if not standin or standin[-1] != "PASS engine D3D9 layouts":
        return False, "stand-in layout print did not finish with its PASS line"
    if not native or native[-1] != "PASS engine D3D9 layouts":
        return False, "native layout print did not finish with its PASS line"
    if standin != native:
        differing = [f"stand-in '{a}' / native '{b}'" for a, b in zip(standin, native) if a != b]
        if len(standin) != len(native):
            differing.append(f"{len(standin)} stand-in lines vs {len(native)} native lines")
        return False, "layouts differ: " + "; ".join(differing[:5])
    return True, f"{len(standin) - 1} layout/enumerator lines identical"


def failure_lines(output):
    return [line for line in output.splitlines() if FAILURE_MARKER in line]


def elf_kind(path):
    """(is ELF32 little-endian i386, e_type) for a file, from its header bytes."""
    try:
        header = Path(path).read_bytes()[:20]
    except OSError:
        return False, None
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return False, None
    (elf_type, machine) = struct.unpack_from("<HH", header, 16)
    return header[4] == 1 and header[5] == 1 and machine == TARGET_MACHINE, elf_type


def dxvk_source_version(source):
    """The version in DXVK's meson.build, or None."""
    try:
        text = (Path(source) / "meson.build").read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r"project\(\s*'dxvk'.*?version\s*:\s*'([^']+)'", text, re.S)
    return match.group(1) if match else None


def stage_dxvk_headers(source, destination):
    """Lay out a DXVK source tree's native headers as ``meson install`` does.

    include/native/directx/* and include/native/windows/* land flat in
    <destination>, and the WSI headers in <destination>/wsi. Returns a list of
    problems; empty means the layout is complete.
    """
    source = Path(source)
    native = source / "include" / "native"
    directx = native / "directx"
    windows = native / "windows"
    wsi = native / "wsi"
    if not (directx / "d3d9.h").is_file():
        return [f"{directx} has no d3d9.h: fetch DXVK's include/native/directx submodule"]
    if not (windows / "windows_base.h").is_file():
        return [f"{windows} has no windows_base.h: not a DXVK Native source tree"]
    destination = Path(destination)
    (destination / "wsi").mkdir(parents=True, exist_ok=True)
    for directory in (directx, windows):
        for entry in sorted(directory.iterdir()):
            if entry.name.startswith(".git") or not entry.is_file():
                continue
            shutil.copy2(entry, destination / entry.name)
    for name in DXVK_WSI_HEADERS:
        if (wsi / name).is_file():
            shutil.copy2(wsi / name, destination / "wsi" / name)
    return missing_dxvk_headers(destination)


def missing_dxvk_headers(include_dir):
    include_dir = Path(include_dir)
    return [f"{include_dir} has no {name}" for name in REQUIRED_DXVK_HEADERS if not (include_dir / name).is_file()]


def header_argv(cxx, repo, config, source, output, dxvk_include, native, compile_only=False, syntax_only=False):
    argv = [cxx, *BASE_FLAGS, *CONFIG_FLAGS[config], *STRICT_FLAGS, "-I", repo / "src"]
    if native:
        argv += [NATIVE_DEFINE, "-isystem", dxvk_include]
    argv.append(repo / source)
    if syntax_only:
        argv.append("-fsyntax-only")
    else:
        if compile_only:
            argv.append("-c")
        argv += ["-o", output]
    return argv


def engine_argv(cxx, repo, config, source, output, dxvk_include, native, compile_only=False, syntax_only=False):
    argv = [
        cxx,
        *ENGINE_BASE_FLAGS,
        *CONFIG_FLAGS[config],
        *ENGINE_DEFINES,
        *ENGINE_CONFIG_DEFINES[config],
        *ENGINE_FLAGS,
        "-I",
        repo / "src",
        "-I",
        repo / "deps",
    ]
    if native:
        argv += [NATIVE_DEFINE, "-isystem", dxvk_include]
    argv.append(repo / source)
    if syntax_only:
        argv.append("-fsyntax-only")
    else:
        if compile_only:
            argv.append("-c")
        argv += ["-o", output]
    return argv


def plan_checks(options, repo, workdir, dxvk_include):
    """Every check, in execution order. Nothing runs here."""
    checks = []
    config = options.config
    compile_timeout = options.compile_timeout
    run_timeout = options.timeout

    for cxx in options.header_cxx:
        tag = compiler_tag(cxx)
        for native in (False, True):
            mode = "native" if native else "standin"
            binary = workdir / f"boundary_{mode}-{tag}"
            expectations = dict(CONVENTION_PROBES)
            if native:
                expectations.update(NATIVE_COM_PROBES)

            def verify(outputs, _results, expectations=expectations, mode=mode):
                run_output = outputs[1]
                if "PASS boundary header checks" not in run_output:
                    return False, "the check did not print its PASS line"
                if f"mode {'native' if mode == 'native' else 'stand-in'}" not in run_output:
                    return False, "the binary was not built in the expected mode"
                problems = check_conventions(outputs[2], expectations)
                if problems:
                    return False, "; ".join(problems)
                return True, f"{len(expectations)} calling-convention probe(s) as expected"

            checks.append(
                Check(
                    f"boundary_{mode}-{tag}",
                    f"boundary header, {mode} mode, {Path(cxx).name}: run and calling conventions",
                    [
                        Step(header_argv(cxx, repo, config, HEADER_CHECK, binary, dxvk_include, native),
                             repo, compile_timeout, "compile"),
                        Step([binary], workdir, run_timeout, "run"),
                        Step([options.objdump, "-d", "--no-show-raw-insn", binary], workdir, run_timeout,
                             "disassemble"),
                    ],
                    verify=verify,
                    artifact=binary,
                    elf_type=(ELF_TYPE_EXEC, ELF_TYPE_DYN),
                )
            )

        for name, source, native, pattern in REJECT_CASES:
            checks.append(
                Check(
                    f"{name}-{tag}",
                    f"must not compile ({Path(cxx).name}): {Path(source).stem.replace('_', ' ')}",
                    [
                        Step(header_argv(cxx, repo, config, source, None, dxvk_include, native, syntax_only=True),
                             repo, compile_timeout, "reject", expect_failure=pattern),
                    ],
                )
            )

    engine_cxx = options.engine_cxx
    layout_outputs = {}
    for native in (False, True):
        mode = "native" if native else "standin"
        binary = workdir / f"engine_layout_{mode}"

        def keep(outputs, _results, mode=mode):
            layout_outputs[mode] = outputs[1]
            if "PASS engine D3D9 layouts" not in outputs[1]:
                return False, "the layout print did not finish"
            return True, "documented sizes and offsets hold"

        checks.append(
            Check(
                f"engine_layout_{mode}",
                f"D3D9-bearing engine layouts, {mode} view ({Path(engine_cxx).name})",
                [
                    Step(engine_argv(engine_cxx, repo, config, LAYOUT_CHECK, binary, dxvk_include, native),
                         repo, compile_timeout, "compile"),
                    Step([binary], workdir, run_timeout, "run"),
                ],
                verify=keep,
                artifact=binary,
                elf_type=(ELF_TYPE_EXEC, ELF_TYPE_DYN),
            )
        )

    def compare(_outputs, results):
        for mode in ("standin", "native"):
            if results.get(f"engine_layout_{mode}") != "ok" or mode not in layout_outputs:
                return False, f"engine_layout_{mode} did not pass, nothing to compare"
        return compare_layouts(layout_outputs["standin"], layout_outputs["native"])

    checks.append(Check("engine_layout_compare", "stand-in and native layouts are byte-identical", [], verify=compare))

    units = read_proof_list(repo) if (repo / PROOF_LIST).is_file() else []
    for unit in units:
        stem = Path(unit).stem
        obj = workdir / "renderer" / f"{stem}.o"
        checks.append(
            Check(
                f"renderer_proof-{stem}",
                f"{unit} compiles unmodified through the boundary",
                [Step(engine_argv(engine_cxx, repo, config, unit, obj, dxvk_include, True, compile_only=True),
                      repo, compile_timeout, "compile")],
                artifact=obj,
                elf_type=(ELF_TYPE_REL,),
            )
        )

    checks.append(
        Check(
            "renderer_standin_control",
            "every proof unit fails without the boundary (it needs the D3D9 API)",
            [
                Step(engine_argv(engine_cxx, repo, config, unit, None, dxvk_include, False, syntax_only=True),
                     repo, compile_timeout, "reject", expect_failure="")
                for unit in units
            ],
        )
    )
    return checks


def run_step(step):
    """Run one step in its own process group; return (status, output, seconds)."""
    print(f"$ {step.describe()}", flush=True)
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            step.argv,
            cwd=str(step.cwd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            start_new_session=True,
        )
    except OSError as error:
        print(f"  cannot start: {error}", flush=True)
        return "error", f"cannot start {step.argv[0]}: {error}", time.monotonic() - started

    try:
        output, _ = process.communicate(timeout=step.timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except OSError:
            process.kill()
        output, _ = process.communicate()
    elapsed = time.monotonic() - started
    output = output or ""

    # Disassembly is long and only the verifier reads it; it goes to the log.
    if output and step.kind != "disassemble":
        print(output if output.endswith("\n") else output + "\n", flush=True)

    if timed_out:
        print(f"  TIMEOUT after {step.timeout}s", flush=True)
        return "timeout", output, elapsed

    if step.expect_failure is None:
        if process.returncode != 0:
            print(f"  exit {process.returncode}", flush=True)
            return "failed", output, elapsed
        print(f"  ok ({elapsed:.1f}s)", flush=True)
        return "ok", output, elapsed

    if process.returncode == 0:
        print("  compiled, but this source must be rejected", flush=True)
        return "failed", output, elapsed
    if step.expect_failure and not re.search(step.expect_failure, output):
        print(f"  rejected, but not for the expected reason (/{step.expect_failure}/)", flush=True)
        return "failed", output, elapsed
    print(f"  rejected as expected (exit {process.returncode}, {elapsed:.1f}s)", flush=True)
    return "ok", output, elapsed


def write_log(log_dir, name, text):
    if not log_dir:
        return
    path = Path(log_dir) / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def run_checks(checks, log_dir):
    results = []
    statuses = {}
    for index, check in enumerate(checks, start=1):
        print(f"[{index}/{len(checks)}] {check.name}: {check.description}", flush=True)
        print("-" * 64, flush=True)
        status, detail, elapsed = "ok", "", 0.0
        outputs = []
        for part, step in enumerate(check.steps, start=1):
            step_status, output, step_elapsed = run_step(step)
            elapsed += step_elapsed
            outputs.append(output)
            write_log(log_dir, f"{index:03d}-{check.name}-{part}-{step.kind}.log", f"$ {step.describe()}\n\n{output}\n")
            if step_status != "ok":
                status, detail = step_status, f"{step.kind} step {part} {step_status}"
                break
            if step.kind == "run" and failure_lines(output):
                status, detail = "failed", f"{len(failure_lines(output))} failure line(s) reported"
                break

        if status == "ok" and check.artifact is not None:
            is_target, elf_type = elf_kind(check.artifact)
            if not Path(check.artifact).is_file():
                status, detail = "failed", "nothing was produced"
            elif not is_target:
                status, detail = "failed", "output is not ELF32/i386"
            elif check.elf_type and elf_type not in check.elf_type:
                status, detail = "failed", f"unexpected ELF type {elf_type}"

        if status == "ok" and check.verify is not None:
            ok, detail = check.verify(outputs, statuses)
            if not ok:
                status = "failed"
            print(f"  {'ok' if ok else 'FAILED'}: {detail}", flush=True)

        statuses[check.name] = status
        results.append((check, status, detail, elapsed))
        print(flush=True)

    write_log(
        log_dir,
        "summary.txt",
        "\n".join(f"{status}\t{check.name}\t{detail}" for check, status, detail, _ in results) + "\n",
    )
    return results, sum(1 for _, status, _, _ in results if status != "ok")


def print_summary(results, failures):
    print("Summary", flush=True)
    print("=======", flush=True)
    for check, status, detail, elapsed in results:
        label = {"ok": "PASS", "failed": "FAIL", "timeout": "TIMEOUT", "error": "ERROR"}.get(status, status.upper())
        suffix = f" ({detail})" if detail else ""
        print(f"  {label:<8} {check.name:<44} {elapsed:6.1f}s{suffix}", flush=True)
    print(flush=True)
    if failures:
        print(f"{failures} of {len(results)} check(s) failed", flush=True)
        return 1
    print(f"all {len(results)} check(s) passed", flush=True)
    return 0


def print_plan(checks, repo, config, workdir, dxvk_include, dxvk_version, log_dir):
    print("Native D3D9 boundary checks: DXVK headers only, no device, no assets", flush=True)
    print("=" * 64, flush=True)
    print(f"repository:  {repo}", flush=True)
    print(f"config:      {config} ({' '.join(CONFIG_FLAGS[config])})", flush=True)
    print("target:      ELF32/i386", flush=True)
    print(f"dxvk:        {dxvk_include} (version {dxvk_version})", flush=True)
    print(f"work dir:    {workdir}", flush=True)
    if log_dir:
        print(f"logs:        {log_dir}", flush=True)
    print(flush=True)
    print(f"{len(checks)} check(s) in this order:", flush=True)
    for index, check in enumerate(checks, start=1):
        print(f"  {index}. {check.name}: {check.description}", flush=True)
        for part, step in enumerate(check.steps, start=1):
            expectation = "" if step.expect_failure is None else "  [must fail]"
            print(f"     {index}.{part} $ {step.describe()}{expectation}", flush=True)
    print(flush=True)


def tool_exists(tool):
    return bool(shutil.which(tool)) or Path(tool).is_file()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", choices=sorted(CONFIG_FLAGS), default="Debug")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--dxvk-include-dir", type=Path, help="installed DXVK Native <prefix>/include/dxvk")
    source.add_argument("--dxvk-source", type=Path, help="DXVK source checkout with the include/native/directx submodule")
    parser.add_argument("--stage-dxvk-headers", type=Path, metavar="DEST",
                        help="with --dxvk-source: only lay the headers out in DEST/ (as meson install does) and exit")
    parser.add_argument("--allow-dxvk-version", help=f"accept this DXVK source version instead of {EXPECTED_DXVK_VERSION}")
    parser.add_argument("--header-cxx", action="append", metavar="CXX",
                        help="compiler for the boundary header checks, repeatable (default: g++ and clang++)")
    parser.add_argument("--engine-cxx", default="clang++", help="compiler for engine-header checks (default: clang++)")
    parser.add_argument("--objdump", default="objdump", help="disassembler for the calling-convention probes")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--work-dir", type=Path, help="directory for binaries (default: a per-run temporary directory)")
    parser.add_argument("--timeout", type=int, default=DEFAULT_RUN_TIMEOUT)
    parser.add_argument("--compile-timeout", type=int, default=DEFAULT_COMPILE_TIMEOUT)
    parser.add_argument("--log-dir", type=Path, help="write one text log per step, plus summary.txt")
    parser.add_argument("--only", help="comma-separated check names, or prefixes ending in '*'")
    parser.add_argument("--skip", help="comma-separated check names, or prefixes ending in '*'")
    parser.add_argument("--list", action="store_true", help="print the plan and exit without running")
    options = parser.parse_args(argv)
    options.header_cxx = options.header_cxx or ["g++", "clang++"]
    if options.timeout <= 0 or options.compile_timeout <= 0:
        parser.error("timeouts must be positive")

    if options.stage_dxvk_headers:
        if not options.dxvk_source:
            parser.error("--stage-dxvk-headers needs --dxvk-source")
        dxvk_source = options.dxvk_source.expanduser().resolve()
        version = dxvk_source_version(dxvk_source)
        wanted = options.allow_dxvk_version or EXPECTED_DXVK_VERSION
        if version != wanted:
            print(f"FAIL: DXVK source version is {version!r}, expected {wanted!r}", flush=True)
            return 1
        destination = options.stage_dxvk_headers.expanduser().resolve()
        problems = stage_dxvk_headers(dxvk_source, destination)
        for problem in problems:
            print(f"FAIL: {problem}", flush=True)
        if problems:
            return 1
        print(f"staged DXVK {version} native headers in {destination}", flush=True)
        return 0

    repo = options.repo_root.expanduser().resolve()
    if not repo.is_dir():
        print(f"FAIL: repository root is not a directory: {repo}", flush=True)
        return 1

    temporary = None
    if options.work_dir:
        workdir = options.work_dir.expanduser().resolve()
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="kisakcod-d3d9-boundary-")
        workdir = Path(temporary.name)

    try:
        dxvk_version = "not recorded in an include directory"
        if options.dxvk_source:
            dxvk_source = options.dxvk_source.expanduser().resolve()
            dxvk_version = dxvk_source_version(dxvk_source)
            wanted = options.allow_dxvk_version or EXPECTED_DXVK_VERSION
            if dxvk_version != wanted:
                print(f"FAIL: DXVK source version is {dxvk_version!r}, expected {wanted!r}", flush=True)
                return 1
            dxvk_include = workdir / "dxvk-include" / "dxvk"
            problems = stage_dxvk_headers(dxvk_source, dxvk_include)
        elif options.dxvk_include_dir:
            dxvk_include = options.dxvk_include_dir.expanduser().resolve()
            problems = missing_dxvk_headers(dxvk_include)
        elif options.list:
            dxvk_include, problems = Path("<dxvk-include-dir>"), []
        else:
            print("FAIL: pass --dxvk-source or --dxvk-include-dir (DXVK Native headers are required)", flush=True)
            return 1
        for problem in problems:
            print(f"FAIL: {problem}", flush=True)
        if problems:
            return 1

        checks = plan_checks(options, repo, workdir, dxvk_include)
        known = [check.name for check in checks]

        def select(value):
            names = set()
            for item in (part.strip() for part in (value or "").split(",")):
                if not item:
                    continue
                matched = [name for name in known if name == item or (item.endswith("*") and name.startswith(item[:-1]))]
                if not matched:
                    parser.error(f"unknown check: {item}")
                names.update(matched)
            return names

        only, skip = select(options.only), select(options.skip)
        chosen = [check for check in checks if (not only or check.name in only) and check.name not in skip]
        if not chosen:
            parser.error("no checks selected")

        print_plan(chosen, repo, options.config, workdir, dxvk_include, dxvk_version, options.log_dir)
        if options.list:
            print("--list: printed the plan, ran nothing.", flush=True)
            return 0

        sources = {HEADER_CHECK, LAYOUT_CHECK, PROOF_LIST, *(case[1] for case in REJECT_CASES)}
        sources.update(read_proof_list(repo) if (repo / PROOF_LIST).is_file() else [])
        missing = sorted(path for path in sources if not (repo / path).is_file())
        if missing:
            for path in missing:
                print(f"FAIL: source is missing: {repo / path}", flush=True)
            return 1
        if not read_proof_list(repo):
            print(f"FAIL: {PROOF_LIST} lists no renderer unit", flush=True)
            return 1
        for tool in (*options.header_cxx, options.engine_cxx, options.objdump):
            if not tool_exists(tool):
                print(f"FAIL: tool not found: {tool}", flush=True)
                return 1

        for check in chosen:
            if check.artifact is not None:
                Path(check.artifact).parent.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        results, failures = run_checks(chosen, options.log_dir)
        print(f"elapsed: {time.monotonic() - started:.1f}s", flush=True)
        return print_summary(results, failures)
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
