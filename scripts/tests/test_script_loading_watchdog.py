#!/usr/bin/env python3
"""Check production precache timing without hiding genuine script timeouts.

The VM timing helpers and SP/MP precache functions are extracted from production.
A fake millisecond clock and item registry separate script work from synchronous
asset work. A licensed-asset application check still validates the complete VM.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source: str, name: str) -> str:
    signature = re.search(
        r"^(?:static\s+)?(?:void|bool)\s+(?:__cdecl\s+)?"
        + re.escape(name) + r"\s*\([^;{}]*\)\s*\{",
        source, re.MULTILINE,
    )
    if signature is None:
        raise ValueError(f"{name} definition was not found")
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
            if not depth:
                return source[signature.start():token.end()].replace("__cdecl", "")
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
struct { int loading; int starttime; } scrVmGlob{};
struct { bool initializing; } level{};
uint32_t now, nativeTime;
unsigned nativeCalls;
bool missingItem;
struct gitem_s {} item;
int Sys_Milliseconds() { return static_cast<int>(now); }
const char* Scr_GetString(unsigned index) { assert(index == 0); return "test_weapon"; }
const gitem_s* G_FindItem(const char* name, int model) {
    assert(std::string(name) == "test_weapon" && model == 0);
    ++nativeCalls;
    now += nativeTime;
    return missingItem ? nullptr : &item;
}
const char* va(const char*, const char*) { return "unknown item 'test_weapon'"; }
[[noreturn]] void Scr_Error(const char* message) { throw std::runtime_error(message); }
[[noreturn]] void Scr_ParamError(unsigned index, const char* message) {
    assert(index == 0);
    throw std::runtime_error(message);
}
"""

SCENARIOS = r"""
void reset(uint32_t start = 100, bool loading = true) {
    now = start;
    scrVmGlob.starttime = static_cast<int>(now);
    scrVmGlob.loading = loading;
    level.initializing = true;
    nativeTime = 0;
    nativeCalls = 0;
    missingItem = false;
}

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void run(const char* name, void (*precache)()) {
    reset();
    now += 3200;
    nativeTime = 30000;
    precache();
    check(!Scr_HasTimedOut(), "Synchronous loading work was charged to the script watchdog");
    now += 1799;
    check(!Scr_HasTimedOut(), "Script timed out before its five-second budget");
    ++now;
    check(Scr_HasTimedOut(), "Precache discarded script time already spent");
    std::printf("PASS: %s loading time excluded, previous script budget preserved\n", name);

    reset();
    nativeTime = 600;
    for (unsigned i = 1; i <= 42; ++i) {
        now += 120;
        precache();
        check(Scr_HasTimedOut() == (i * 120 >= 5000),
              "Repeated precache calls hid an accumulating script loop");
    }
    check(nativeCalls == 42, "Repeated precache fixture did not call production bridge");
    std::printf("PASS: %s interleaved script loop still times out\n", name);

    reset();
    now += 4999;
    check(!Scr_HasTimedOut(), "Pure script loop timed out early");
    ++now;
    check(Scr_HasTimedOut(), "Pure script loop detection was disabled during loading");
    std::printf("PASS: %s pure script loop retains five-second boundary\n", name);

    reset(100, false);
    nativeTime = 6000;
    precache();
    check(scrVmGlob.starttime == 100 && Scr_HasTimedOut(),
          "Loading adjustment changed normal runtime watchdog accounting");
    std::printf("PASS: %s normal-runtime accounting unchanged\n", name);

    for (uint32_t start : {0x7ffffff0u, 0xfffff000u}) {
        reset(start);
        now += 4800;
        nativeTime = 20000;
        precache();
        check(!Scr_HasTimedOut(), "Loading accounting failed across clock wrap");
        now += 200;
        check(Scr_HasTimedOut(), "Script deadline lost across clock wrap");
    }
    std::printf("PASS: %s signed boundary and millisecond-clock wrap\n", name);

    reset();
    missingItem = true;
    nativeTime = 6000;
    bool rejected = false;
    try { precache(); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) == "unknown item 'test_weapon'";
    }
    check(rejected, "Unknown precached item no longer reports its parameter error");
    check(!Scr_HasTimedOut(), "Failed lookup charged loading time before reporting its error");
    std::printf("PASS: %s unknown item remains rejected\n", name);

    reset();
    level.initializing = false;
    rejected = false;
    try { precache(); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected && nativeCalls == 0, "Runtime precache restriction was lost");
    std::printf("PASS: %s precache restriction unchanged\n", name);
}

int main() {
    try {
        run("SP", Scr_PrecacheItemSP);
        run("MP", Scr_PrecacheItemMP);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Loading watchdog regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    root = Path(__file__).resolve().parents[2] / "src"
    parser.add_argument("--vm-source", type=Path, default=root / "script/scr_vm.cpp")
    parser.add_argument("--sp-source", type=Path, default=root / "game/g_scr_main.cpp")
    parser.add_argument("--mp-source", type=Path, default=root / "game_mp/g_scr_main_mp.cpp")
    args = parser.parse_args()
    vm = args.vm_source.read_text()
    parts = [HARNESS, function(vm, "Scr_HasTimedOut"), function(vm, "Scr_AccountForLoadingTime")]
    for source, suffix in ((args.sp_source, "SP"), (args.mp_source, "MP")):
        body = function(source.read_text(), "Scr_PrecacheItem")
        parts.append(body.replace("Scr_PrecacheItem(", f"Scr_PrecacheItem{suffix}("))
    parts.append(SCENARIOS)
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="kisakcod-loading-watchdog-") as folder:
        test = Path(folder) / "loading-watchdog.cpp"
        binary = Path(folder) / "loading-watchdog"
        test.write_text("\n".join(parts))
        subprocess.run(compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                                   str(test), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
