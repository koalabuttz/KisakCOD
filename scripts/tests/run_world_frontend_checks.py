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
with ``--gc-sections`` and runs the result. No SDL, GPU, Steam client,
credentials, game assets or network are involved, and none is looked for.

``r_scene.cpp`` (the scene setup the test drives: ``R_InitScene``,
``R_SetViewParmsForScene``) and ``r_buffers.cpp`` (the ``gfxBuf`` storage) call
the D3D9 API, so they are compiled through the native D3D9 boundary:
``KISAK_LINUX_NATIVE_D3D9`` plus the pinned DXVK Native 2.7.1 *headers*
(``--dxvk-source`` or ``--dxvk-include-dir``, exactly as
``run_native_d3d9_boundary_checks.py`` takes them). Only those units see DXVK;
every other unit keeps the type-only stand-in, which is the per-unit opt-in the
boundary documents (docs/linux-native-d3d9.md). No DXVK library is linked and no
D3D9 method is called.

Why an explicit source list: the renderer front end lives beside the D3D9 back
end in ``src/gfx_d3d``, and r_init.cpp does not compile on Linux yet (see
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
    python3 scripts/tests/run_world_frontend_checks.py --config Debug \\
        --dxvk-source <dxvk-2.7.1-checkout>
    python3 scripts/tests/run_world_frontend_checks.py --config RelWithDebInfo \\
        --dxvk-include-dir <prefix>/include/dxvk --log-dir artifacts/world-frontend-RelWithDebInfo
    python3 scripts/tests/run_world_frontend_checks.py --list   # print, run nothing

Requires clang++ with a working ``-m32`` link (clang and g++-multilib on Ubuntu),
the OpenAL headers (libopenal-dev), which the client sound headers include, and
the DXVK Native 2.7.1 headers for the two boundary units.
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
from run_native_d3d9_boundary_checks import (  # noqa: E402  (one way to find the DXVK headers)
    EXPECTED_DXVK_VERSION,
    NATIVE_DEFINE,
    dxvk_source_version,
    missing_dxvk_headers,
    stage_dxvk_headers,
)

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
    # dynamic-entity accessors the per-cell dynamic model walk calls; the draw
    # method and the XSurface, XModel and DObj accessors r_scene.cpp's surface
    # adders call.
    "src/qcommon/cm_load.cpp",
    "src/DynEntity/DynEntity_load_obj.cpp",
    "src/gfx_d3d/r_draw_method.cpp",
    "src/gfx_d3d/r_xsurface.cpp",
    "src/xanim/xmodel_utils.cpp",
    "src/xanim/dobj_utils.cpp",
    # Compiled through the native D3D9 boundary (NATIVE_D3D9_SOURCES): the scene
    # setup and `scene` storage, and the `gfxBuf` storage.
    "src/gfx_d3d/r_scene.cpp",
    "src/gfx_d3d/r_buffers.cpp",
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

# Production units that call the D3D9 API and so see DXVK's real d3d9.h through
# the boundary. Each must also be on the boundary's own proof list
# (scripts/platform/linux/native_d3d9_renderer_sources.txt).
NATIVE_D3D9_SOURCES = ("src/gfx_d3d/r_scene.cpp", "src/gfx_d3d/r_buffers.cpp")

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


def compile_argv(cxx, source, config, output, dxvk_include):
    own = source in (TEST_SOURCE, BOUNDARY_SOURCE)
    native = [NATIVE_DEFINE, "-isystem", str(dxvk_include)] if source in NATIVE_D3D9_SOURCES else []
    return [cxx, *CLIENT_DEFINES, *COMMON_FLAGS, *CONFIG_FLAGS[config],
            *(TEST_FLAGS if own else PRODUCTION_FLAGS), *native, "-c", source, "-o", str(output)]


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
    for source in NATIVE_D3D9_SOURCES:
        if source not in PRODUCTION_SOURCES:
            problems.append(f"{source} is compiled through the boundary but is not a production source")
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
    dxvk = parser.add_mutually_exclusive_group()
    dxvk.add_argument("--dxvk-include-dir", type=Path, help="installed DXVK Native <prefix>/include/dxvk")
    dxvk.add_argument("--dxvk-source", type=Path, help="DXVK source checkout with the include/native/directx submodule")
    parser.add_argument("--allow-dxvk-version", help=f"accept this DXVK source version instead of {EXPECTED_DXVK_VERSION}")
    parser.add_argument("--list", action="store_true", help="print the sources and compile lines, run nothing")
    options = parser.parse_args(argv)

    problems = check_sources()
    if problems:
        for problem in problems:
            print(f"FAIL: source list: {problem}")
        return 1

    if options.list:
        include = options.dxvk_include_dir or Path("<dxvk-include-dir>")
        for source in all_sources():
            print(shlex.join(compile_argv(options.cxx, source, options.config, Path("<obj>"), include)))
        return 0

    if not (options.dxvk_source or options.dxvk_include_dir):
        print("FAIL: pass --dxvk-source or --dxvk-include-dir (the boundary units need the DXVK Native headers)")
        return 1

    with tempfile.TemporaryDirectory(prefix="kisak-world-frontend-") as workdir:
        if options.dxvk_source:
            source_dir = options.dxvk_source.expanduser().resolve()
            version = dxvk_source_version(source_dir)
            wanted = options.allow_dxvk_version or EXPECTED_DXVK_VERSION
            if version != wanted:
                print(f"FAIL: DXVK source version is {version!r}, expected {wanted!r}")
                return 1
            dxvk_include = Path(workdir) / "dxvk-include" / "dxvk"
            problems = stage_dxvk_headers(source_dir, dxvk_include)
        else:
            dxvk_include = options.dxvk_include_dir.expanduser().resolve()
            problems = missing_dxvk_headers(dxvk_include)
        if problems:
            for problem in problems:
                print(f"FAIL: DXVK headers: {problem}")
            return 1

        objects = {source: Path(workdir) / (source.replace("/", "_") + ".o") for source in all_sources()}

        def argv_for(source):
            return compile_argv(options.cxx, source, options.config, objects[source], dxvk_include)

        def build(source):
            return source, run(argv_for(source), options.compile_timeout)

        failed = []
        with ThreadPoolExecutor(max_workers=max(1, options.jobs)) as pool:
            for source, (status, output) in pool.map(build, all_sources()):
                argv = argv_for(source)
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
