#!/usr/bin/env python3
"""Check production startup config execution and explicit developer settings.

Command-line splitting, startup-variable dispatch, configuration ordering and the
developer autoexec gate come from common.cpp. Command tokenization, dvar storage
and config-file execution are controlled test doubles; no game assets are used.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, name):
    signature = re.search(
        r"^void\s+(?:__cdecl\s+)?" + re.escape(name)
        + r"\s*\([^;{}]*\)\s*\{", source, re.MULTILINE,
    )
    if not signature:
        raise ValueError(f"Missing production function: {name}")
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
                return source[signature.start():token.end()].replace("__cdecl", "")
    raise ValueError(f"Unterminated production function: {name}")


HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#ifdef NDEBUG
#error This regression requires active assertions.
#endif
#define iassert(value) assert(value)
struct dvar_t { struct { int integer; } current; } developer;
const dvar_t* com_developer = &developer;
int developerScript;
bool inAutoExec, safeMode;
int profileDeveloper, profileScript;
int archiveWrites;
char* com_consoleLines[32];
int com_numConsoleLines;
std::vector<std::string> tokens, queued, executed;
std::vector<bool> autoExecStates;
void Cmd_TokenizeString(const char* text) {
    assert(tokens.empty());
    std::istringstream input(text);
    for (std::string token; input >> token;) tokens.push_back(token);
}
const char* Cmd_Argv(unsigned index) {
    return index < tokens.size() ? tokens[index].c_str() : "";
}
void Cmd_EndTokenizedString() { tokens.clear(); }
int I_stricmp(const char* left, const char* right) {
    std::string a(left), b(right);
    for (char& ch : a) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : b) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return a.compare(b);
}
void Dvar_Set_f() {
    assert(tokens.size() == 3);
    if (tokens[1] == "developer") developer.current.integer = std::atoi(tokens[2].c_str());
    if (tokens[1] == "developer_script") developerScript = std::atoi(tokens[2].c_str());
}
void Dvar_SetA_f() { ++archiveWrites; Dvar_Set_f(); }
void Dvar_SetInAutoExec(bool value) { autoExecStates.push_back(value); inAutoExec = value; }
int CL_ControllerIndexFromClientNum(int client) { assert(client == 0); return 7; }
void Cmd_ExecuteSingleCommand(int client, int controller, char* text) {
    assert(client == 0 && controller == 7 && inAutoExec);
#ifdef KISAK_MP
    assert(std::strcmp(text, "exec autoexec_dev_mp.cfg") == 0);
    developer.current.integer = 1;
#else
    assert(std::strcmp(text, "exec autoexec_dev.cfg") == 0);
    developer.current.integer = 2;
#endif
    executed.push_back(text);
    // Both shipped developer presets explicitly enable developer script blocks.
    developerScript = 1;
}
void Cbuf_AddText(int client, const char* text) { assert(client == 0); queued.emplace_back(text); }
void Cbuf_Execute(int client, int controller) {
    assert(client == 0 && controller == 7 && tokens.empty());
    for (const auto& text : queued) {
        std::string command = text;
        if (!command.empty() && command.back() == '\n') command.pop_back();
        executed.push_back(command);
        if (command == "exec profiles/test.cfg") {
            if (profileDeveloper >= 0) developer.current.integer = profileDeveloper;
            if (profileScript >= 0) developerScript = profileScript;
        }
        if (command.find("exec safemode") == 0) developerScript = 0;
    }
    queued.clear();
}
bool Com_SafeMode() { return safeMode; }
const char* va(const char* format, ...) {
    static char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}
"""

SCENARIOS = r"""
void startup(std::string commandLine, int profileDev = -1, int profileDevScript = -1,
             bool safe = false, bool profile = true) {
    developer.current.integer = 0;
    developerScript = 0;
    archiveWrites = 0;
    inAutoExec = false;
    autoExecStates.clear();
    tokens.clear();
    queued.clear();
    executed.clear();
    safeMode = safe;
    profileDeveloper = profileDev;
    profileScript = profileDevScript;
    Com_ParseCommandLine(commandLine.data());
    Com_StartupVariable(nullptr);
    Com_ExecStartupConfigs(0, profile ? "profiles/test.cfg" : nullptr);
    Com_StartupVariable(nullptr);
    assert(tokens.empty() && queued.empty() && !inAutoExec);
}
void developerPreset(bool expected) {
    const auto count = std::count_if(executed.begin(), executed.end(), [](const std::string& command) {
        return command.find("exec autoexec_dev") == 0;
    });
    assert(count == (expected ? 1 : 0));
    assert(autoExecStates == (expected ? std::vector<bool>{true, false} : std::vector<bool>{}));
}
int main() {
    setbuf(stdout, nullptr);
    startup("");
    developerPreset(false);
    assert(developer.current.integer == 0 && developerScript == 0);
    std::puts("PASS: normal startup retains retail developer-script defaults");

    startup("+set developer_script 1");
    developerPreset(false);
    assert(developer.current.integer == 0 && developerScript == 1);
    std::puts("PASS: explicit developer_script opt-in is independent of developer presets");

    startup("", -1, 1);
    developerPreset(false);
    assert(developer.current.integer == 0 && developerScript == 1);
    std::puts("PASS: profile script setting survives without enabling developer presets");

    startup("", -1, -1, false, false);
    developerPreset(false);
    assert(executed.size() == 2 && developerScript == 0);
    std::puts("PASS: startup without a profile still executes default and language configs");

    for (int level : {1, 2}) {
        startup("+set developer " + std::to_string(level));
        developerPreset(true);
        assert(developer.current.integer == level && developerScript == 1);
        std::printf("PASS: explicit developer %d enables the platform developer preset\n", level);

        startup("+set developer " + std::to_string(level) + "+set developer_script 0");
        developerPreset(true);
        assert(developer.current.integer == level && developerScript == 0);
        std::printf("PASS: explicit developer_script 0 overrides developer %d preset\n", level);
    }

    startup("+seta developer 1+seta developer_script 0");
    developerPreset(true);
    assert(archiveWrites == 4 && developer.current.integer == 1 && developerScript == 0);
    std::puts("PASS: seta startup overrides retain their precedence");

    startup("", 1);
    developerPreset(true);
    assert(developerScript == 1);
    auto profile = std::find(executed.begin(), executed.end(), "exec profiles/test.cfg");
    auto preset = std::find_if(executed.begin(), executed.end(), [](const std::string& command) {
        return command.find("exec autoexec_dev") == 0;
    });
    assert(profile < preset);
    std::puts("PASS: a profile can explicitly opt into development after defaults load");

    startup("+set developer 1", -1, -1, true);
    developerPreset(true);
    assert(developerScript == 0 && executed.back().find("exec safemode") == 0);
    std::puts("PASS: safe-mode configuration keeps its existing final position");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--source", type=Path, help="Override common.cpp for negative probes")
    args = parser.parse_args()
    source = (args.source or args.repo / "src/qcommon/common.cpp").read_text()
    startup = function(source, "Com_Init_Try_Block_Function")
    # Verify the modeled phases retain their real initialization order. Explicit
    # startup values are replayed after configs and before script settings latch.
    markers = [
        r"Com_StartupVariable\(0\)", r"Com_InitDvars\(\)",
        r"Com_StartupConfigs\(localClientNum\)", r"Com_StartupVariable\(0\)",
        r"Com_SetScriptSettings\(\)",
    ]
    position = 0
    for marker in markers:
        match = re.search(marker, startup[position:])
        if not match:
            raise AssertionError(f"Startup phase ordering changed: {marker}")
        position += match.end()
    production = "\n".join(function(source, name) for name in (
        "Com_ParseCommandLine", "Com_StartupVariable", "Com_RunAutoExec", "Com_ExecStartupConfigs",
    ))
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="kisakcod-developer-config-") as directory:
        root = Path(directory)
        cpp = root / "startup.cpp"
        cpp.write_text(HARNESS + production + SCENARIOS)
        for mode in ("KISAK_SP", "KISAK_MP"):
            executable = root / mode
            subprocess.run(compiler + [
                "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", f"-D{mode}", str(cpp), "-o", str(executable),
            ], check=True)
            print(f"Production developer-config checks: {mode}", flush=True)
            subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
