#!/usr/bin/env python3
"""Exercise shellshock view settings and their camera consumer from source.

Production dvar registration, parameter conversion, domain validation, rounding
and camera-update bodies are extracted. Only the registry, audio metadata and
client state are authored test doubles. No game assets or audio device are used.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <xmmintrin.h>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
#define iassert(condition) assert(condition)
constexpr float EQUAL_EPSILON = 0.001f;
constexpr unsigned DVAR_CHEAT = 128;
constexpr bool alwaysfails = false;
union DvarValue {
    float value;
    float vector[4];
    int integer;
    bool enabled;
    unsigned char color[4];
};
union DvarLimits {
    struct { float min, max; } value;
    struct { int min, max; } integer;
    struct { int stringCount; const char** strings; } enumeration;
};
struct dvar_s {
    struct { float value = 1.0f; int integer = 0; bool enabled = false;
             const char* string = "fixture"; } current;
    float reset = 1.0f;
    DvarLimits domain{};
    unsigned flags = 0;
};
std::map<std::string, dvar_s> variables;
dvar_s* dvar(const char* name) { return &variables[name]; }
void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::runtime_error("Production shellshock invariant failed");
}
const char* va(const char*, ...) { return "Unexpected dvar type"; }
const dvar_s* Dvar_RegisterFloat(const char* name, float value, DvarLimits domain,
                                unsigned flags, const char*) {
    auto var = dvar(name);
    var->current.value = var->reset = value;
    var->domain = domain;
    var->flags = flags;
    return var;
}
const char* Dvar_EnumToString(const dvar_s*) { return "fixture"; }
void I_strncpyz(char* dest, const char* text, unsigned size) {
    assert(std::strlen(text) < size);
    std::strcpy(dest, text);
}
int SND_GetEntChannelCount() { return 0; }
const dvar_s* bg_shock_volume[64]{};

struct cg_s { struct { float viewDelta[2]; } shellshock; } client{};
float cg_perturbations[128][8];
unsigned cameraEnds;
cg_s* CG_GetLocalClientGlobals(int localClientNum) {
    assert(localClientNum == 0);
    return &client;
}
void EndShellShockCamera(int localClientNum) {
    assert(localClientNum == 0);
    ++cameraEnds;
    client.shellshock.viewDelta[0] = client.shellshock.viewDelta[1] = 0.0f;
}
"""

SCENARIOS = r"""
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(float actual, float expected, const char* message) {
    check(std::isfinite(actual) && std::fabs(actual - expected) < 0.00001f, message);
}
void reset() {
    for (auto& entry : variables) entry.second.current.value = 1.0f;
    RegisterViewKickDvars();
    client = {};
    cameraEnds = 0;
    for (auto& row : cg_perturbations) std::fill(std::begin(row), std::end(row), 1.0f);
}
bool set(const char* name, float value) {
    auto var = dvar(name);
    DvarValue candidate{};
    candidate.value = value;
    if (!Dvar_ValueInDomain(1, candidate, var->domain)) return false;
    var->current.value = value;
    return true;
}
shellshock_parms_t parameters() {
    shellshock_parms_t parms{};
    BG_SetShellShockParmsFromDvars(&parms);
    return parms;
}
void fadeCases() {
    reset();
    auto parms = parameters();
    check(parms.view.fadeTime == 3000, "Default three-second view fade changed");
    near(parms.view.kickRate, 0.001f / 0.75f, "Default kick period changed");
    std::puts("PASS: default view fade and kick period preserved");

    for (auto setting : {0.5f, 1.75f, 20.0f, 1000.0f}) {
        reset();
        check(set("bg_shock_viewKickFadeTime", setting), "Valid custom fade rejected");
        parms = parameters();
        check(parms.view.fadeTime == int(setting * 1000.0f),
              "Custom view fade ignored or seconds not converted to milliseconds");
    }
    std::puts("PASS: custom view fades and maximum duration reach the parameter block");

    reset();
    check(set("bg_shock_viewKickFadeTime", 0.0016f), "Fractional fade rejected");
    check(parameters().view.fadeTime == 2, "Fractional fade does not use engine rounding");
    std::puts("PASS: fade uses existing nearest-millisecond conversion");

    reset();
    check(set("bg_shock_viewKickFadeTime", 1.0f), "One-second fade rejected");
    check(set("bg_shock_viewKickRadius", 1.0f), "Valid radius rejected");
    parms = parameters();
    UpdateShellShockCamera(0, &parms, 1000, 2000);
    near(client.shellshock.viewDelta[0], 1.0f, "View fades before configured interval");
    UpdateShellShockCamera(0, &parms, 1500, 2000);
    near(client.shellshock.viewDelta[0], 0.5f, "Custom fade does not affect camera");
    UpdateShellShockCamera(0, &parms, 2000, 2000);
    near(client.shellshock.viewDelta[0], 0.0f, "Expired shellshock did not stop");
    check(cameraEnds == 1, "Expired camera effect did not end once");
    std::puts("PASS: camera holds then fades over configured interval and ends on expiry");
}
void zeroCases() {
    reset();
    check(set("bg_shock_viewKickPeriod", 0.0f), "Authored zero view-kick period rejected");
    auto parms = parameters();
    near(parms.view.kickRate, 1.0f, "Zero period bypassed existing 1ms divisor floor");
    std::puts("PASS: zero view-kick period accepted with finite existing divisor floor");

    check(set("bg_shock_viewKickPeriod", 0.0005f), "Sub-millisecond period rejected");
    near(parameters().view.kickRate, 1.0f, "Sub-millisecond period bypassed divisor floor");
    check(set("bg_shock_viewKickPeriod", 1000.0f), "Maximum period rejected");
    near(parameters().view.kickRate, 0.000001f, "Maximum period was clamped incorrectly");
    std::puts("PASS: sub-millisecond and maximum periods retain finite conversion");

    reset();
    check(set("bg_shock_viewKickFadeTime", 0.0f), "Authored zero view fade rejected");
    check(set("bg_shock_viewKickRadius", 1.0f), "Valid radius rejected");
    parms = parameters();
    check(parms.view.fadeTime == 0, "Zero fade did not reach parameter block");
    UpdateShellShockCamera(0, &parms, 1999, 2000);
    near(client.shellshock.viewDelta[0], 1.0f, "Zero fade divided by zero or faded early");
    UpdateShellShockCamera(0, &parms, 2000, 2000);
    near(client.shellshock.viewDelta[0], 0.0f, "Zero-fade effect did not end on expiry");
    check(cameraEnds == 1, "Zero-fade effect ended at wrong time");
    std::puts("PASS: zero view fade holds camera effect safely until expiry");

    reset();
    check(set("bg_shock_viewKickFadeTime", 0.0001f), "Small positive fade rejected");
    parms = parameters();
    check(parms.view.fadeTime == 0, "Sub-millisecond fade rounded incorrectly");
    UpdateShellShockCamera(0, &parms, 1, 2);
    check(std::isfinite(client.shellshock.viewDelta[0]), "Rounded zero fade is not safe");
    std::puts("PASS: positive fades rounded to zero are safe in camera consumer");

    for (auto name : {"bg_shock_viewKickPeriod", "bg_shock_viewKickFadeTime"}) {
        reset();
        auto original = dvar(name)->current.value;
        for (auto invalid : {-0.001f, 1000.001f,
                             std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()})
            check(!set(name, invalid) && dvar(name)->current.value == original,
                  "Invalid view-kick setting accepted or previous setting lost");
        check(dvar(name)->flags == DVAR_CHEAT, "View-kick cheat protection changed");
    }
    std::puts("PASS: negatives, excess values and nonfinite settings remain invalid");
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected suite name");
        if (std::string(argv[1]) != "zero") fadeCases();
        if (std::string(argv[1]) != "fade") zeroCases();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Shellshock settings regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def function(source: str, name: str) -> str:
    return extract(source, rf"^(?:inline\s+)?(?:void|int|char|double)\s+(?:__cdecl\s+)?{name}\s*\([^;{{}}]*\)\s*\{{", name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--source", type=Path, default=root / "src/bgame/bg_misc.cpp")
    parser.add_argument("--suite", choices=("all", "fade", "zero"), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    source = args.source.read_text()
    locals_source = (root / "src/bgame/bg_local.h").read_text()
    camera_source = (root / "src/cgame/cg_shellshock.cpp").read_text()
    dvar_source = (root / "src/universal/dvar.cpp").read_text()
    parameters = function(source, "BG_SetShellShockParmsFromDvars")
    registration = re.search(
        r"    minci\.value\.max.*?(?=\n#ifdef KISAK_MP\n    bg_shock_sound =)",
        source, re.DOTALL,
    )
    if registration is None:
        raise ValueError("Shellshock view dvar registration was not found")
    variables = sorted(set(re.findall(r"\bbg_shock_\w+", parameters + registration.group())) - {"bg_shock_volume"})
    parts = [HARNESS]
    parts.extend(f'const dvar_s* {name} = dvar("{name}");' for name in variables)
    parts.append(extract(locals_source, r"^enum ShockViewTypes[^{}]*\{", "ShockViewTypes").replace("__int32", "int32_t") + ";")
    for name in ("shellshock_parms_t_screenblend", "shellshock_parms_t_view", "shellshock_parms_t_sound",
                 "shellshock_parms_t_lookcontrol", "shellshock_parms_t_movement", "shellshock_parms_t"):
        parts.append(extract(locals_source, rf"^(?:const\s+)?struct {name}\b[^{{}}]*\{{", name).removeprefix("const ") + ";")
    parts.append(function((root / "src/qcommon/qcommon.h").read_text(), "SnapFloatToInt"))
    parts.extend(function(dvar_source, name) for name in ("Dvar_VectorInDomain", "Dvar_ValueInDomain"))
    parts.append("void RegisterViewKickDvars() {\nDvarLimits minci{}, mincj{}, minck{};\n" + registration.group() + "\n}")
    parts.append(parameters)
    parts.extend(function(camera_source, name) for name in ("CubicInterpolate", "UpdateShellShockCamera"))
    parts.append(SCENARIOS)
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="kisakcod-shock-settings-") as folder:
        test_source = Path(folder) / "shock.cpp"
        executable = Path(folder) / "shock-test"
        test_source.write_text("\n".join(parts))
        for variant in ("SP", "MP"):
            subprocess.run(compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-parentheses",
                                       "-Wno-unused-parameter", "-fsanitize=undefined,float-divide-by-zero",
                                       "-fno-sanitize-recover=all", f"-DKISAK_{variant}",
                                       str(test_source), "-o", str(executable)], check=True)
            print(f"Shellshock settings {variant}:", flush=True)
            subprocess.run([str(executable), args.suite], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
