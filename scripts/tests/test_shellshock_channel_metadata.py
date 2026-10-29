#!/usr/bin/env python3
"""Exercise shellshock loading without an initialized audio device.

Production registration, channel initialization, shock loading and dvar-file
validation bodies are extracted. The audio-device layer is never called; raw
files, channel parsing and the dvar registry use authored test doubles.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def extract(source: str, pattern: str, name: str) -> str:
    signature = re.search(pattern, source, re.MULTILINE)
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
            if depth == 0:
                return source[signature.start():token.end()].replace("__cdecl", "")
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
#define iassert(condition) assert(condition)
#define sprintf_s std::snprintf
constexpr int SND_MAX_ENTCHANNEL_NAMELENGTH = 64;
constexpr int DVAR_CHEAT = 128;
constexpr int DVAR_SOURCE_INTERNAL = 0;
constexpr int CON_CHANNEL_PLAYERWEAP = 1;
constexpr int CON_CHANNEL_SYSTEM = 2;
constexpr int ERR_DROP = 1;
struct DvarLimits { struct { float min, max; } value; };
struct dvar_s { struct { float value; } current; float reset; };
struct snd_entchannel_info_t { char name[64]; };
const dvar_s* bg_shock_volume[64]{};
char bgShockChannelNames[64][80]{};
std::vector<snd_entchannel_info_t> channels;
std::map<std::string, dvar_s> variables;
std::map<std::string, std::string> files;
std::set<char*> liveBuffers;
unsigned channelLoads, warnings, errors;
bool parseSession;

void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::runtime_error("Production dvar invariant failed");
}
void Com_Error(int, const char*, ...) {
    throw std::runtime_error("Missing channel metadata remains an asset error");
}
void Com_PrintError(int, const char*, ...) { ++errors; }
void Com_PrintWarning(int, const char*, ...) { ++warnings; }
const char* va(const char*, ...) { return "Missing test dvar"; }
void Com_sprintf(char* output, unsigned length, const char* format, ...) {
    va_list args; va_start(args, format);
    std::vsnprintf(output, length, format, args); va_end(args);
}
int SND_GetEntChannelCount() { return int(channels.size()); }
snd_entchannel_info_t* SND_GetEntChannelName(int index) {
    return &channels.at(unsigned(index));
}
const dvar_s* Dvar_RegisterFloat(const char* name, float value, DvarLimits limits,
                                unsigned flags, const char*) {
    assert(limits.value.min == 0.0f && limits.value.max == 1.0f);
    assert(flags == DVAR_CHEAT);
    return &variables.try_emplace(name, dvar_s{{value}, value}).first->second;
}
char* Com_LoadRawTextFile(const char* path) {
    auto found = files.find(path);
    if (found == files.end()) return nullptr;
    auto buffer = new char[found->second.size() + 1];
    std::memcpy(buffer, found->second.c_str(), found->second.size() + 1);
    liveBuffers.insert(buffer);
    return buffer;
}
void Com_UnloadRawTextFile(char* buffer) {
    assert(liveBuffers.erase(buffer) == 1);
    delete[] buffer;
}
void SND_ParseEntChannelFile(const char* buffer) {
    ++channelLoads;
    channels.clear();
    // Only metadata parsing is substituted; no playback/capture device exists.
    std::istringstream input(buffer);
    std::string name;
    while (input >> name) {
        assert(name.size() < SND_MAX_ENTCHANNEL_NAMELENGTH);
        snd_entchannel_info_t channel{};
        std::strcpy(channel.name, name.c_str());
        channels.push_back(channel);
    }
}
dvar_s* Dvar_FindVar(const char* name) {
    auto found = variables.find(name);
    return found == variables.end() ? nullptr : &found->second;
}
void Dvar_Reset(dvar_s* var, int source) {
    assert(source == DVAR_SOURCE_INTERNAL);
    var->current.value = var->reset;
}
void Dvar_SetFromString(dvar_s* var, const char* text) {
    var->current.value = std::strtof(text, nullptr);
}
int I_stricmp(const char* a, const char* b) {
    for (; *a && std::tolower(static_cast<unsigned char>(*a)) ==
                    std::tolower(static_cast<unsigned char>(*b)); ++a, ++b) {}
    return std::tolower(static_cast<unsigned char>(*a)) -
           std::tolower(static_cast<unsigned char>(*b));
}
void Com_BeginParseSession(const char*) { assert(!parseSession); parseSession = true; }
void Com_EndParseSession() { assert(parseSession); parseSession = false; }
const char* token(const char** buffer, bool allowNewline) {
    static std::string result;
    result.clear();
    while (**buffer && std::isspace(static_cast<unsigned char>(**buffer))) {
        if (**buffer == '\n' && !allowNewline) return result.c_str();
        ++*buffer;
    }
    while (**buffer && !std::isspace(static_cast<unsigned char>(**buffer))) result += *(*buffer)++;
    return result.c_str();
}
const char* Com_Parse(const char** buffer) { return token(buffer, true); }
const char* Com_ParseOnLine(const char** buffer) { return token(buffer, false); }
void Com_SkipRestOfLine(const char** buffer) {
    while (**buffer && **buffer != '\n') ++*buffer;
    if (**buffer) ++*buffer;
}
void BG_RegisterShockVolumeDvars();
void SND_InitEntChannels();
"""

SCENARIOS = r"""
void reset() {
    assert(liveBuffers.empty());
    assert(!parseSession);
    channels.clear(); variables.clear(); files.clear();
    std::fill(std::begin(bg_shock_volume), std::end(bg_shock_volume), nullptr);
    std::memset(bgShockChannelNames, 0, sizeof(bgShockChannelNames));
    channelLoads = warnings = errors = 0;
    for (auto name : bgShockDvarNames) variables[name] = {{0.5f}, 0.5f};
    files["soundaliases/channels.def"] = "fixture_fx fixture_voice\n";
}
std::string shock(float fx = 0.25f, float voice = 0.75f) {
    std::ostringstream output;
    for (auto name : bgShockDvarNames) output << name << " 0.5\n";
    output << "bg_shock_volume_fixture_fx " << fx << "\n";
    output << "bg_shock_volume_fixture_voice " << voice << "\n";
    return output.str();
}
void requireVolumes(float fx = 0.25f, float voice = 0.75f) {
    if (SND_GetEntChannelCount() != 2 || !bg_shock_volume[0] || !bg_shock_volume[1])
        throw std::runtime_error("Dedicated shock loading did not register channel volumes");
    if (bg_shock_volume[0]->current.value != fx || bg_shock_volume[1]->current.value != voice)
        throw std::runtime_error("Shock channel settings were lost or reordered");
    assert(liveBuffers.empty());
    assert(!parseSession);
}
int main() {
    try {
        reset(); files["shock/fixture.shock"] = shock();
        assert(BG_LoadShellShockDvars("fixture") == 1);
        requireVolumes();
        assert(channelLoads == 1 && warnings == 0 && errors == 0);
        std::puts("PASS: dedicated loading registers real channel metadata without audio");

        auto original = bg_shock_volume[0];
        files["shock/second.shock"] = shock(0.125f, 0.875f);
        assert(BG_LoadShellShockDvars("second") == 1);
        requireVolumes(0.125f, 0.875f);
        assert(bg_shock_volume[0] == original && channelLoads == 1);
        std::puts("PASS: repeated shock loads preserve channel registration and apply values");

        reset(); SND_InitEntChannels();
        files["shock/client.shock"] = shock();
        assert(BG_LoadShellShockDvars("client") == 1);
        requireVolumes(); assert(channelLoads == 1 && warnings == 0);
        std::puts("PASS: initialized clients reuse their existing channel metadata");

        reset(); files["shock/default.shock"] = shock();
        assert(BG_LoadShellShockDvars("missing") == 1);
        requireVolumes(); assert(errors == 1 && warnings == 0);
        std::puts("PASS: missing named shock retains the validated default fallback");

        reset(); files["shock/fixture.shock"] = shock() + "bg_shock_volume_typo 0.25\n";
        assert(BG_LoadShellShockDvars("fixture") == 1);
        requireVolumes();
        assert(warnings == 1 && variables.count("bg_shock_volume_typo") == 0);
        std::puts("PASS: unknown channel names still produce validation warnings");

        reset(); files.erase("soundaliases/channels.def");
        bool missingRejected = false;
        try { BG_LoadShellShockDvars("fixture"); }
        catch (const std::runtime_error& error) {
            if (std::string(error.what()) != "Missing channel metadata remains an asset error") throw;
            missingRejected = true;
        }
        assert(missingRejected && channelLoads == 0 && liveBuffers.empty());
        std::puts("PASS: missing channel definitions remain an explicit asset error");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Shellshock metadata regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--source", type=Path, default=root / "src/bgame/bg_misc.cpp")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    source = args.source.read_text()
    parts = [HARNESS]
    parts.append(extract(source, r"^const char \*bgShockDvarNames\[27\] =\s*\{", "shock dvar names") + ";")
    inputs = (
        (source, "BG_RegisterShockVolumeDvars"),
        ((root / "src/sound/snd.cpp").read_text(), "SND_InitEntChannels"),
        ((root / "src/universal/dvar.cpp").read_text(), "Com_LoadDvarsFromBuffer"),
        (source, "BG_LoadShellShockDvars"),
    )
    for contents, name in inputs:
        parts.append(extract(contents, rf"^(?:void|int)\s+(?:__cdecl\s+)?{name}\s*\([^;{{}}]*\)\s*\{{", name))
    parts.append(SCENARIOS)
    with tempfile.TemporaryDirectory(prefix="kisakcod-shock-metadata-") as folder:
        test_source = Path(folder) / "shock.cpp"
        executable = Path(folder) / "shock-test"
        test_source.write_text("\n".join(parts))
        compiler = shlex.split(args.cxx)
        if not compiler:
            parser.error("--cxx must specify a C++ compiler")
        # The unchanged production dvar loader uses a signed read counter.
        compiled = subprocess.run(compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", str(test_source), "-o", str(executable)], check=False)
        if compiled.returncode:
            return 1
        tested = subprocess.run([str(executable)], check=False)
        return 0 if tested.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
