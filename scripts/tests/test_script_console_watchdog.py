#!/usr/bin/env python3
"""Exercise production script-console timing without suppressing output or loops.

The SP/MP print/println functions and VM timing helpers are extracted directly.
Console output and time are test doubles so blocking I/O can be separated from
script execution. Complete loading behavior still needs an application check.
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
#include <vector>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
struct { int loading; int starttime; } scrVmGlob{};
struct { int scriptPrintChannel; } level{23};
struct dvar_s { struct { bool enabled; } current; } noSpam{}, scriptTimes{};
const dvar_s* g_NoScriptSpam = &noSpam;
const dvar_s* logScriptTimes = &scriptTimes;
uint32_t now, nativeTime;
std::vector<std::string> formats;
int Sys_Milliseconds() { return static_cast<int>(now); }
int Scr_GetNumParam() { return 2; }
const char* Scr_GetDebugString(int index) {
    assert(index >= 0 && index < 2);
    return index ? "beta" : "alpha";
}
void Com_Printf(int channel, const char* format, ...) {
    assert(channel == level.scriptPrintChannel);
    formats.emplace_back(format);
    now += nativeTime;
}
void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::runtime_error("Production script-console invariant failed");
}
"""

SCENARIOS = r"""
void reset(uint32_t start = 100, bool loading = true) {
    now = start;
    scrVmGlob.starttime = static_cast<int>(now);
    scrVmGlob.loading = loading;
    nativeTime = 8000;
    formats.clear();
    noSpam.current.enabled = false;
    scriptTimes.current.enabled = false;
}

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

uint32_t scriptTime() { return now - static_cast<uint32_t>(scrVmGlob.starttime); }

void run(const char* name, void (*write)(), bool newline) {
    const unsigned expectedCalls = newline ? 3 : 2;
    reset();
    now += 3200;
    write();
    check(formats.size() == expectedCalls && formats[0] == "%s" && formats[1] == "%s"
              && (!newline || formats.back() == "\n"),
          "Script-console output was suppressed or changed");
    check(scriptTime() == 3200 && !Scr_HasTimedOut(),
          "Console I/O was charged to script time or nested print was counted twice");
    now += 1799;
    check(!Scr_HasTimedOut(), "Console operation caused an early timeout");
    ++now;
    check(Scr_HasTimedOut(), "Console operation discarded previous script time");
    std::printf("PASS: %s output preserved, blocking I/O excluded exactly once\n", name);

    reset();
    for (unsigned i = 1; i <= 20; ++i) {
        now += 250;
        write();
        check(scriptTime() == i * 250 && Scr_HasTimedOut() == (i == 20),
              "A printing script loop evaded the five-second watchdog");
    }
    std::printf("PASS: %s repeated output preserves genuine loop timeout\n", name);

    reset(100, false);
    now += 3200;
    write();
    check(scrVmGlob.starttime == 100 && Scr_HasTimedOut(),
          "Console loading adjustment changed normal-runtime accounting");
    std::printf("PASS: %s normal-runtime accounting unchanged\n", name);

    reset();
    noSpam.current.enabled = true;
    now += 4999;
    write();
    check(formats.empty() && scriptTime() == 4999 && !Scr_HasTimedOut(),
          "Suppressed script output changed console behavior or timing");
    ++now;
    check(Scr_HasTimedOut(), "Suppressed output reset the script watchdog");
    std::printf("PASS: %s script-spam suppression unchanged\n", name);

    for (uint32_t start : {0x7ffffff0u, 0xfffff000u}) {
        reset(start);
        now += 4800;
        write();
        check(scriptTime() == 4800 && !Scr_HasTimedOut(),
              "Console timing failed across clock wrap");
        now += 200;
        check(Scr_HasTimedOut(), "Console timing lost the deadline across clock wrap");
    }
    std::printf("PASS: %s signed boundary and millisecond-clock wrap\n", name);
}

int main() {
    try {
        run("SP print", printSP, false);
        run("SP println", printlnSP, true);
        run("MP print", printMP, false);
        run("MP println", printlnMP, true);

        reset();
        scriptTimes.current.enabled = true;
        now += 3200;
        printlnSP();
        check(formats.size() == 4 && formats[0] == "PRINT TIME: %d\n" && scriptTime() == 3200,
              "SP timing output was changed or charged to the script watchdog");
        std::printf("PASS: SP script-time output preserved and excluded exactly once\n");

        reset();
        scriptTimes.current.enabled = noSpam.current.enabled = true;
        now += 3200;
        printlnSP();
        check(formats.size() == 1 && formats[0] == "PRINT TIME: %d\n" && scriptTime() == 3200,
              "SP timing output no longer honors its separate logging setting");
        std::printf("PASS: SP timing output remains independent of spam suppression\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Console watchdog regression failed: %s\n", error.what());
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
        for name in ("print", "println"):
            body = function(source.read_text(), name)
            for called in ("print", "println"):
                body = re.sub(r"\b" + called + r"\s*\(", called + suffix + "(", body)
            parts.append(body)
    parts.append(SCENARIOS)
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="kisakcod-console-watchdog-") as folder:
        test = Path(folder) / "console-watchdog.cpp"
        binary = Path(folder) / "console-watchdog"
        test.write_text("\n".join(parts))
        subprocess.run(compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                                   str(test), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
