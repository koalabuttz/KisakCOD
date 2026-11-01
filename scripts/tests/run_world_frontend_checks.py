#!/usr/bin/env python3
"""Build and run the asset-free world front-end test for one configuration.

``tests/linux_world_frontend/world_frontend_test.cpp`` runs the production code
that decides which parts of a world are drawn and how they are batched -- DPVS
cell and portal visibility, the per-cell AABB-tree and cull-group walk, the BSP
draw-surface batching and the draw-surface merge -- against a small synthetic
GfxWorld, and checks each result against expectations derived from the geometry.

This runner performs **no CMake configure and no engine build**. It compiles the
test, its link boundary (``frontend_link_boundary.cpp``) and an explicit list of
unmodified production translation units with one 32-bit compiler line, links them
with ``--gc-sections`` and runs the result. No DXVK, SDL, GPU, Steam client,
credentials, game assets or network are involved, and none is looked for.

Why an explicit source list: the renderer front end lives beside the D3D9 back
end in ``src/gfx_d3d``, and r_init.cpp, r_scene.cpp and r_buffers.cpp do not
compile on Linux until the native D3D9 boundary lands (see
``scripts/tests/client_compile_census_blocked.txt``). The list below is the exact
closure that links: every production unit here compiles unmodified, and
everything else those units can reach is defined by the boundary file as host
services, zero-initialized storage, or functions that abort with their name.

Configurations are explicit: ``Debug`` is ``-O0 -D_DEBUG`` and ``RelWithDebInfo``
is ``-O2 -DNDEBUG``, both with ``-g`` and with ``RELEASE_ASSERTS`` so production
assertions stay live (the boundary's assertion handler fails the run).

What a pass does not claim
--------------------------
No device, texture, shader or pixel is involved: a pass says the front end
selects and batches the synthetic world's surfaces as the test states, on this
compiler and configuration. It says nothing about drawing a real map, and the
hosted workflow that invokes this script is not verified by it.

Usage:
    python3 scripts/tests/run_world_frontend_checks.py --config Debug
    python3 scripts/tests/run_world_frontend_checks.py --config RelWithDebInfo \\
        --log-dir artifacts/world-frontend-RelWithDebInfo
    python3 scripts/tests/run_world_frontend_checks.py --list   # print, run nothing

Requires clang++ with a working ``-m32`` link (clang and g++-multilib on Ubuntu)
and the OpenAL headers (libopenal-dev), which the client sound headers include.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_client_compile_census import CLIENT_DEFINES  # noqa: E402  (one client define set)

TEST_SOURCE = "tests/linux_world_frontend/world_frontend_test.cpp"
BOUNDARY_SOURCE = "tests/linux_world_frontend/frontend_link_boundary.cpp"

# Unmodified production units, the exact link closure of the test. Add a unit
# only when the link needs it, and prefer linking an owner over defining its
# symbol in the boundary file.
PRODUCTION_SOURCES = (
    # Renderer front end under test.
    "src/gfx_d3d/r_dpvs.cpp",
    "src/gfx_d3d/r_dpvs_static.cpp",
    "src/gfx_d3d/r_dpvs_dynmodel.cpp",
    "src/gfx_d3d/r_dpvs_sceneent.cpp",
    "src/gfx_d3d/r_dpvs_entity.cpp",
    "src/gfx_d3d/r_add_bsp.cpp",
    "src/gfx_d3d/r_add_cmdbuf.cpp",
    "src/gfx_d3d/r_pretess.cpp",
    "src/gfx_d3d/r_workercmds.cpp",
    "src/gfx_d3d/r_drawsurf.cpp",
    "src/gfx_d3d/r_rendercmds.cpp",
    # Renderer support the front end calls or R_RegisterDvars reaches.
    "src/gfx_d3d/r_dvars.cpp",
    "src/gfx_d3d/r_utils.cpp",
    "src/gfx_d3d/r_warn.cpp",
    "src/gfx_d3d/r_debug.cpp",
    "src/gfx_d3d/r_debug_alloc.cpp",
    "src/gfx_d3d/r_sky.cpp",
    # Engine runtime: math, dvars, commands, strings, memory.
    "src/universal/com_math.cpp",
    "src/universal/com_convexhull.cpp",
    "src/universal/dvar.cpp",
    "src/universal/dvar_cmds.cpp",
    "src/universal/dvar_game_commands.cpp",
    "src/universal/q_shared.cpp",
    "src/universal/com_shared.cpp",
    "src/universal/com_strings.cpp",
    "src/universal/com_memory.cpp",
    "src/qcommon/cmd.cpp",
    "src/qcommon/mem_track.cpp",
    "src/script/scr_stringlist.cpp",
    "src/script/scr_memorytree.cpp",
    # Owners linked instead of faked: the collision map storage and the
    # dynamic-entity accessors the per-cell dynamic model walk calls.
    "src/qcommon/cm_load.cpp",
    "src/DynEntity/DynEntity_load_obj.cpp",
    # Native platform layer, as the dedicated Linux target links it.
    "src/linux/sys_threads.cpp",
    "src/linux/sys_critical.cpp",
    "src/linux/sys_posix.cpp",
    "src/linux/sys_time.cpp",
    "src/linux/sys_memory.cpp",
    "src/linux/sys_virtualmem.cpp",
    "src/linux/sys_fileio.cpp",
    "src/linux/sys_console.cpp",
)

CONFIG_FLAGS = {
    "Debug": ("-O0", "-D_DEBUG"),
    "RelWithDebInfo": ("-O2", "-DNDEBUG"),
}

# The flags the native Linux targets compile production code with
# (scripts/platform/linux/CMakeLists.txt). Production warnings are the census's
# business, so they are not turned into errors here.
COMMON_FLAGS = (
    "-m32", "-std=gnu++20", "-g", "-fms-extensions", "-msse2", "-mfpmath=sse",
    "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
)
PRODUCTION_FLAGS = (
    "-I", "src", "-I", "deps",
    "-Wno-c++11-narrowing", "-Wno-invalid-token-paste", "-Wno-ignored-attributes",
    "-Wno-format-security",
)
# The two test units are this change's own code: every warning is an error.
# Engine headers are system headers for them, so only the test code is judged.
TEST_FLAGS = ("-isystem", "src", "-isystem", "deps", "-Wall", "-Wextra", "-Werror")
LINK_FLAGS = ("-m32", "-Wl,--gc-sections", "-pthread")

PASS_LINE = "PASS: world front end"
DEFAULT_COMPILE_TIMEOUT = 600
DEFAULT_RUN_TIMEOUT = 120
EM_386 = 3


def all_sources():
    """Test first, then its boundary, then production units."""
    return (TEST_SOURCE, BOUNDARY_SOURCE, *PRODUCTION_SOURCES)


def compile_argv(cxx, source, config, output):
    own = source in (TEST_SOURCE, BOUNDARY_SOURCE)
    return [cxx, *CLIENT_DEFINES, *COMMON_FLAGS, *CONFIG_FLAGS[config],
            *(TEST_FLAGS if own else PRODUCTION_FLAGS), "-c", source, "-o", str(output)]


def run(argv, timeout):
    """Run argv from the repository root in its own process group."""
    try:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, start_new_session=True)
    except OSError as error:
        return "error", f"could not start {argv[0]}: {error}\n"
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        return "timeout", output
    return ("ok" if process.returncode == 0 else f"exit {process.returncode}"), output


def is_elf32_i386(path):
    with open(path, "rb") as handle:
        header = handle.read(20)
    return (len(header) == 20 and header[:4] == b"\x7fELF" and header[4] == 1
            and struct.unpack_from("<H", header, 18)[0] == EM_386)


def check_sources():
    problems = []
    seen = set()
    for source in all_sources():
        if source in seen:
            problems.append(f"{source} is listed twice")
        seen.add(source)
        if not (ROOT / source).is_file():
            problems.append(f"{source} does not exist")
    return problems


def write_log(log_dir, name, text):
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / name).write_text(text, encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--config", choices=sorted(CONFIG_FLAGS), default="Debug")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--compile-timeout", type=int, default=DEFAULT_COMPILE_TIMEOUT)
    parser.add_argument("--run-timeout", type=int, default=DEFAULT_RUN_TIMEOUT)
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--list", action="store_true", help="print the sources and compile lines, run nothing")
    options = parser.parse_args(argv)

    problems = check_sources()
    if problems:
        for problem in problems:
            print(f"FAIL: source list: {problem}")
        return 1

    if options.list:
        for source in all_sources():
            print(shlex.join(compile_argv(options.cxx, source, options.config, Path("<obj>"))))
        return 0

    with tempfile.TemporaryDirectory(prefix="kisak-world-frontend-") as workdir:
        objects = {source: Path(workdir) / (source.replace("/", "_") + ".o") for source in all_sources()}

        def build(source):
            return source, run(compile_argv(options.cxx, source, options.config, objects[source]),
                               options.compile_timeout)

        failed = []
        with ThreadPoolExecutor(max_workers=max(1, options.jobs)) as pool:
            for source, (status, output) in pool.map(build, all_sources()):
                argv = compile_argv(options.cxx, source, options.config, objects[source])
                write_log(options.log_dir, source.replace("/", "_") + ".log",
                          f"$ {shlex.join(argv)}\n{output}")
                if status != "ok":
                    failed.append(source)
                    print(f"FAIL: compile {source} ({status})\n$ {shlex.join(argv)}\n{output}", flush=True)
        if failed:
            print(f"FAIL: {len(failed)} of {len(all_sources())} units did not compile ({options.config})")
            return 1
        print(f"compiled {len(all_sources())} units ({options.config})", flush=True)

        binary = Path(workdir) / "world_frontend_test"
        link = [options.cxx, *[str(objects[source]) for source in all_sources()], *LINK_FLAGS, "-o", str(binary)]
        status, output = run(link, options.compile_timeout)
        write_log(options.log_dir, "link.log", f"$ {shlex.join(link)}\n{output}")
        if status != "ok":
            print(f"FAIL: link ({status})\n{output}")
            return 1
        if not is_elf32_i386(binary):
            print("FAIL: the test binary is not an ELF32 i386 executable")
            return 1

        status, output = run([str(binary)], options.run_timeout)
        write_log(options.log_dir, "world_frontend_test.log", output)
        sys.stdout.write(output)
        if status != "ok" or PASS_LINE not in output or "\nFAIL" in "\n" + output:
            print(f"FAIL: world front-end test ({status}, {options.config})")
            return 1
    print(f"PASS: world front-end checks ({options.config})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
