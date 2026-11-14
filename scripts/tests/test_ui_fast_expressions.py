#!/usr/bin/env python3
"""Check that ui_fastExpressions reads dvars exactly as the text path does.

Menu expressions read dvars with dvarInt, dvarBool, dvarFloat and dvarString.
The retail path formats the dvar's value as text (Dvar_GetVariantString) and
parses the text back with atoi or atof. With ui_fastExpressions 1 the bool, int
and string types are read directly (UI_ReadDvarDirect), and dvarString formats
the dvar it already found instead of looking it up again.

This check compiles the production UI_ReadDvarDirect (src/ui/ui_expressions.cpp)
with the production Dvar_ValueToString and Dvar_GetVariantString
(src/universal/dvar.cpp) and the production dvar types (src/universal/q_shared.h)
on the host, and compares the two paths bit for bit over every dvar type and a
set of awkward values. No game assets are needed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source: str, start: int) -> str:
    """Return a brace-delimited declaration/function, ignoring quoted braces."""
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
        re.DOTALL,
    )
    for token in tokens.finditer(source, opening + 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise ValueError("Unterminated production declaration")


HARNESS_TOP = r"""
#include <bit>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <vector>

typedef uint8_t byte;
typedef uint16_t word;
typedef uint64_t uint64;
#define HIDWORD(x) ((int)((uint64_t)(x) >> 32))
#define LODWORD(x) ((int)(uint64_t)(x))

static bool alwaysfails = false;

struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void MyAssertHandler(const char*, int, int, const char* format, ...) {
    throw std::runtime_error(std::string("assert: ") + format);
}

// q_shared.cpp's va: two rotating 1024-byte buffers, ERR_DROP when too long.
char* va(const char* format, ...) {
    static char buffers[2][1024];
    static int index;
    char* buf = buffers[index];
    index = (index + 1) % 2;
    va_list argptr;
    va_start(argptr, format);
    const int len = vsnprintf(buf, 1024, format, argptr);
    va_end(argptr);
    buf[1023] = 0;
    if (len >= 1024)
        throw GameError("va: too short");
    return buf;
}
"""

HARNESS_BOTTOM = r"""
static std::deque<dvar_s> g_storage;
static std::vector<dvar_s*> g_dvars;

const dvar_s* Dvar_FindVar(const char* dvarName) {
    for (dvar_s* dvar : g_dvars)
        if (!strcasecmp(dvar->name, dvarName))
            return dvar;
    return nullptr;
}

PRODUCTION_FUNCTIONS

static void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

static int g_direct, g_text;

// What GetDvarIntValue / GetDvarBoolValue / GetDvarFloatValue compute without
// ui_fastExpressions.
static bool textPath(const char* name, int* intValue, float* floatValue) {
    try {
        const char* text = Dvar_GetVariantString(name);
        *intValue = atoi(text);
        text = Dvar_GetVariantString(name);
        *floatValue = atof(text);
        return true;
    } catch (const GameError&) {
        return false;  // the game would have stopped with ERR_DROP
    }
}

static void compare(const char* name) {
    int fastInt = 0x5a5a5a5a, textInt = 0;
    float fastFloat = 12345.0f, textFloat = 0.0f;
    const bool textOk = textPath(name, &textInt, &textFloat);
    if (!UI_ReadDvarDirect(name, &fastInt, &fastFloat)) {
        ++g_text;  // ui_fastExpressions keeps the text path for this one
        return;
    }
    ++g_direct;
    check(textOk, std::string("direct read of ") + name + " where the text path stops the game");
    check(fastInt == textInt, std::string("int differs for ") + name + ": "
          + std::to_string(fastInt) + " against " + std::to_string(textInt));
    check(std::bit_cast<uint32_t>(fastFloat) == std::bit_cast<uint32_t>(textFloat),
          std::string("float differs for ") + name);
}

// dvarString with ui_fastExpressions formats the dvar it found; without, it
// looks the name up again. The text must be the same.
static void compareString(const char* name) {
    const dvar_s* dvar = Dvar_FindVar(name);
    if (!dvar || dvar->type == DVAR_TYPE_STRING)
        return;  // both paths are unchanged there
    std::string fast, text;
    bool fastOk = true, textOk = true;
    try { fast = Dvar_ValueToString(dvar, dvar->current); } catch (const GameError&) { fastOk = false; }
    try { text = Dvar_GetVariantString(name); } catch (const GameError&) { textOk = false; }
    check(fastOk == textOk && fast == text, std::string("dvarString differs for ") + name);
}

static dvar_s* add(const char* name, DvarType type) {
    dvar_s* dvar = &g_storage.emplace_back();
    dvar->name = name;
    dvar->type = (byte)type;
    g_dvars.push_back(dvar);
    return dvar;
}

int main() {
    try {
        static std::vector<std::string> names;
        names.reserve(256);
        auto name = [&](const std::string& n) { names.push_back(n); return names.back().c_str(); };

        for (bool b : {false, true})
            add(name(std::string("b_") + (b ? "1" : "0")), DVAR_TYPE_BOOL)->current.enabled = b;

        for (int i : {0, 1, -1, 7, 255, 16777216, 16777217, -16777217, 123456789, 2147483647,
                      INT_MIN, INT_MIN + 1, 999999999})
            add(name("i_" + std::to_string(i)), DVAR_TYPE_INT)->current.integer = i;

        static std::string longDigits1023(1023, '7'), longDigits1024(1024, '7');
        for (const char* s : {"", "0", "1", "12", "-7", " 42", "+5", "3.75", "-0.5", "1e3", "1e-3",
                              "abc", "12abc", "0x10", "2147483648", "-2147483649", "nan", "inf",
                              "-inf", "  \t9", "1.0000001", "16777217", "0.1",
                              longDigits1023.c_str(), longDigits1024.c_str()})
            add(name("s_" + std::to_string(g_dvars.size())), DVAR_TYPE_STRING)->current.string = s;

        for (float f : {0.0f, -0.0f, 1.0f, 0.5f, 123456.7f, 1e6f, 1e7f, 0.1f, -2.75f, 3.4e38f})
            add(name("f_" + std::to_string(g_dvars.size())), DVAR_TYPE_FLOAT)->current.value = f;

        dvar_s* v2 = add("v2", DVAR_TYPE_FLOAT_2); v2->current.vector[0] = 1.5f; v2->current.vector[1] = 2;
        dvar_s* v3 = add("v3", DVAR_TYPE_FLOAT_3); v3->current.vector[0] = 3; v3->current.vector[2] = -1;
        dvar_s* v4 = add("v4", DVAR_TYPE_FLOAT_4); v4->current.vector[3] = 0.25f;
        dvar_s* c = add("color", DVAR_TYPE_COLOR);
        c->current.color[0] = 255; c->current.color[1] = 128; c->current.color[2] = 1; c->current.color[3] = 0;
        static const char* enumStrings[] = {"off", "2", "on"};
        for (int index = 0; index < 3; ++index) {
            dvar_s* e = add(name("e_" + std::to_string(index)), DVAR_TYPE_ENUM);
            e->domain.enumeration.stringCount = 3;
            e->domain.enumeration.strings = enumStrings;
            e->current.integer = index;
        }

        for (dvar_s* dvar : g_dvars) {
            compare(dvar->name);
            compareString(dvar->name);
        }
        compare("no_such_dvar");
        compare("B_1");  // names are not case sensitive

        check(g_direct == 2 + 13 + 24 + 1 + 1, "the set of directly read dvars changed; audit the fixture");
        std::printf("PASS: %d dvars read directly give the text path's int and float bit for bit; "
                    "%d keep the text path\n", g_direct, g_text);
        std::printf("PASS: dvarString formats the dvar it found exactly as a second lookup would\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ui_fastExpressions check failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="run with address and undefined-behavior sanitizers")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    q_shared = (root / "src/universal/q_shared.h").read_text()
    dvar_cpp = (root / "src/universal/dvar.cpp").read_text()
    ui_cpp = (root / "src/ui/ui_expressions.cpp").read_text()

    types = block(q_shared, q_shared.index("enum DvarType")) + ";\n"
    start = q_shared.index("union DvarValue")
    end = q_shared.index("using dvar_t = dvar_s;", start)
    types += q_shared[start:end] + "using dvar_t = dvar_s;\n"

    functions = [
        block(dvar_cpp, dvar_cpp.index("const char *Dvar_ValueToString(const dvar_s *dvar, DvarValue value)")),
        block(dvar_cpp, dvar_cpp.index("const char *Dvar_GetVariantString(const char *dvarName)")),
        block(ui_cpp, ui_cpp.index("static bool UI_ReadDvarDirect(")),
    ]
    program = HARNESS_TOP + types + HARNESS_BOTTOM.replace("PRODUCTION_FUNCTIONS", "\n\n".join(functions))

    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    with tempfile.TemporaryDirectory(prefix="kisakcod-ui-fast-expressions-") as temp:
        cpp, executable = Path(temp) / "ui_fast_expressions.cpp", Path(temp) / "ui_fast_expressions"
        cpp.write_text(program)
        subprocess.run(shlex.split(args.cxx) + flags + [str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
