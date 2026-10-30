#!/usr/bin/env python3
"""Exercise the production command shutdown body with a checked allocator.

The renderer's unrelated lighting/debug/scene shutdown functions are stubbed.
This checks command-buffer ownership, not the complete renderer lifecycle;
startup/quit and video restart still require an application-level smoke check.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def shutdown_function(source: str) -> str:
    signature = re.search(
        r"void\s+__cdecl\s+R_ShutdownRenderCommands\s*\(\s*\)\s*\{", source
    )
    if signature is None:
        raise ValueError("R_ShutdownRenderCommands definition was not found")
    # Ignore braces inside comments and quoted strings while finding the body.
    tokens = re.compile(
        r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""",
        re.DOTALL,
    )
    depth = 1
    for token in tokens.finditer(source, signature.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start() : token.end()].replace("__cdecl", "")
    raise ValueError("R_ShutdownRenderCommands body was not terminated")


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif

struct GfxCmdArray {
    uint8_t* cmds;
    int usedTotal;
    int usedCritical;
    void* lastCmd;
};
struct DebugGlobals {};
struct Backend {
    GfxCmdArray* commands;
    DebugGlobals debugGlobals;
};
Backend s_backEndData[2]{};
GfxCmdArray g_frontEndCmds[2]{};
GfxCmdArray g_debugFrontEndCmds{};
std::set<void*> live;
unsigned frees;

void R_ShutdownModelLightingGlobals() {}
void R_ShutdownDebugEntry(DebugGlobals*) {}
void R_ShutdownSceneBuffers() {}
void R_FreeGlobalVariable(void* pointer) {
    if (!pointer) return;
    assert(live.erase(pointer) == 1 &&
           "Each free must refer to a unique command-buffer allocation");
    ++frees;
    delete[] static_cast<uint8_t*>(pointer);
}
uint8_t* allocate() {
    auto pointer = new uint8_t[64];
    assert(live.insert(pointer).second);
    return pointer;
}
"""

SCENARIOS = r"""
void scenario(const char* name, int count, bool debug, int binding0, int binding1) {
    s_backEndData[0].commands =
        binding0 < 0 ? nullptr : &g_frontEndCmds[binding0];
    s_backEndData[1].commands =
        binding1 < 0 ? nullptr : &g_frontEndCmds[binding1];
    for (int i = 0; i < count; ++i)
        g_frontEndCmds[i] = {allocate(), 12, 8, reinterpret_cast<void*>(1)};
    if (debug)
        g_debugFrontEndCmds = {allocate(), 9, 4, reinterpret_cast<void*>(1)};
    frees = 0;
    R_ShutdownRenderCommands();
    assert(live.empty() &&
           "Shutdown must free buffers even when no backend frame has bound them");
    assert(frees == unsigned(count + debug));
    for (auto& commands : g_frontEndCmds) {
        assert(!commands.cmds && !commands.lastCmd);
        assert(!commands.usedTotal && !commands.usedCritical);
    }
    assert(!g_debugFrontEndCmds.cmds && !g_debugFrontEndCmds.lastCmd);
    assert(!g_debugFrontEndCmds.usedTotal && !g_debugFrontEndCmds.usedCritical);
    R_ShutdownRenderCommands();
    assert(frees == unsigned(count + debug) &&
           "Repeated command cleanup must not free an allocation twice");
    std::printf("PASS: %s\n", name);
}

int main() {
    scenario("shutdown before command allocation", 0, false, -1, -1);
    scenario("interrupted initialization after one allocation", 1, false, -1, -1);
    scenario("allocated buffers before any backend frame", 2, true, -1, -1);
    scenario("first frame leaves the other backend unbound", 2, true, -1, 0);
    scenario("both backend frames alias one command array", 2, true, 0, 0);
    scenario("normal complete frame cycle", 2, true, 0, 1);
    scenario("swapped backend and command indices", 2, true, 1, 0);
    scenario("command allocation after completed cleanup", 2, true, -1, 0);
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "src/gfx_d3d/r_rendercmds.cpp",
        help="Production source to test; defaults to this checkout",
    )
    args = parser.parse_args()
    function = shutdown_function(args.source.read_text())
    with tempfile.TemporaryDirectory(prefix="kisakcod-command-ownership-") as folder:
        root = Path(folder)
        test_source = root / "shutdown.cpp"
        executable = root / "shutdown-test"
        test_source.write_text(HARNESS + "\n" + function + "\n" + SCENARIOS)
        compiler = shlex.split(args.cxx)
        if not compiler:
            parser.error("--cxx must specify a C++ compiler")
        compiled = subprocess.run(
            compiler
            + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
               str(test_source), "-o", str(executable)],
            check=False,
        )
        if compiled.returncode:
            return 1
        tested = subprocess.run([str(executable)], check=False)
        if tested.returncode:
            print(
                f"Renderer command ownership regression failed ({tested.returncode})",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
