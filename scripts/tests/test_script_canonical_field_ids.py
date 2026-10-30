#!/usr/bin/env python3
"""Check canonical field emission and debugger lookup with real 16-bit IDs.

The production emitter, canonical archive lookup, ordinary string transfer and
VM reader are compiled together. Only allocation, string storage, reference
tracking and error reporting use authored doubles. No game assets are needed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source, start):
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""",
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


def function(source, name):
    signature = re.search(
        r"^(?:[\w:*<>]+\s+)+(?:__cdecl\s+)?" + re.escape(name)
        + r"\s*\([^;{}]*\)\s*\{", source, re.MULTILINE,
    )
    if not signature:
        raise ValueError(f"Missing production function: {name}")
    return block(source, signature.start()).replace("__cdecl", "")


PREFIX = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error Canonical field checks require active assertions.
#endif
#define iassert(condition) assert(condition)
using byte = unsigned char;
static_assert(sizeof(unsigned short) == 2);
"""

DOUBLES = r"""
struct { int developer_statement; short canonicalStrings[65536]; } scrCompilePub;
struct { byte* codePos; bool bConstRefCount; } scrCompileGlob;
struct { bool developer_script; uint16_t canonicalStrCount; } scrVarPub;
struct { ArchivedCanonicalStringInfo* archivedCanonicalStrings; } scrEvaluateGlob;
alignas(unsigned short) std::array<byte, 12> code;
std::vector<std::string> strings;
std::vector<ArchivedCanonicalStringInfo> archive;
std::array<uint32_t, 65536> internForCanonical;
unsigned allocations, errors, addRefs, transferRefs, removeRefs;

byte* TempMallocAlignStrict(unsigned size) {
    assert(size == sizeof(unsigned short));
    ++allocations;
    return code.data() + 4;
}
const char* SL_ConvertToString(uint32_t id) {
    assert(id && id <= strings.size());
    return strings[id - 1].c_str();
}
void SL_AddRefToString(uint32_t id) {
    assert(id && id < 65536);
    ++addRefs;
}
void SL_TransferRefToUser(uint32_t id, unsigned user) {
    assert(id && id < 65536 && user == 2);
    ++transferRefs;
}
void Scr_CompileRemoveRefToString(uint32_t id) {
    assert(id && id < 65536);
    ++removeRefs;
}
void CompileError(uint32_t pos, const char* text) {
    assert(pos == 0 && std::string(text) == "unknown field");
    ++errors; // Debugger evaluation records the error and returns.
}
"""

SCENARIOS = r"""
void reset(int mode = SCR_DEV_EVALUATE, bool constantRefs = false) {
    code.fill(0xA5);
    scrCompilePub.developer_statement = mode;
    scrCompileGlob = {nullptr, constantRefs};
    scrVarPub.developer_script = false;
    allocations = errors = addRefs = transferRefs = removeRefs = 0;
}
void payload(uint16_t expected) {
    assert(allocations == 1 && scrCompileGlob.codePos == code.data() + 4);
    const char* position = reinterpret_cast<const char*>(scrCompileGlob.codePos);
    assert(Scr_ReadUnsignedShort(&position) == expected);
    assert(position == reinterpret_cast<const char*>(code.data() + 6));
    for (unsigned i = 0; i < code.size(); ++i)
        if (i < 4 || i >= 6) assert(code[i] == 0xA5);
}
void evaluate(uint16_t canonical, bool constantRefs = false) {
    reset(SCR_DEV_EVALUATE, constantRefs);
    EmitCanonicalString(internForCanonical[canonical]);
    payload(canonical);
    assert(errors == 0 && addRefs == 0 && transferRefs == 0 && removeRefs == 0);
}
int main() {
    try {
        // Sorted names intentionally have a different order from their IDs,
        // exercising the real archive search rather than an identity mapping.
        strings.reserve(65535);
        archive.reserve(65535);
        for (unsigned i = 0; i < 65535; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "authored_field_%05u", i);
            strings.emplace_back(name);
        }
        for (unsigned i = 0; i < 65535; ++i) {
            const uint16_t canonical = uint16_t((i * 256u) % 65535u + 1u);
            archive.push_back({canonical, strings[i].c_str()});
            internForCanonical[canonical] = i + 1;
        }
        scrEvaluateGlob.archivedCanonicalStrings = archive.data();
        scrVarPub.canonicalStrCount = 65535;
        evaluate(0x100);
        evaluate(0x200);
        evaluate(0xFF00);
        std::puts("PASS: debugger accepts known canonical IDs with a zero low byte");

        for (unsigned id = 1; id <= 65535; ++id) evaluate(uint16_t(id));
        std::puts("PASS: all 65535 nonzero IDs round-trip through archive, emitter and VM reader");

        for (uint16_t id : {uint16_t(1), uint16_t(0x100), uint16_t(0xFFFF)})
            evaluate(id, true);
        std::puts("PASS: debugger lookup preserves archived string references in both ref modes");

        scrVarPub.canonicalStrCount = 2;
        archive[1].canonicalStr = 2;
        for (bool constantRefs : {false, true}) {
            reset(SCR_DEV_EVALUATE, constantRefs);
            EmitCanonicalString(3); // Interned, but absent from the two-entry archive.
            payload(0);
            assert(errors == 1 && addRefs == 0 && transferRefs == 0 && removeRefs == 0);
        }
        std::puts("PASS: missing fields still emit zero and report exactly one unknown-field error");

        for (int mode : {SCR_DEV_NO, SCR_DEV_YES}) {
            for (bool constantRefs : {false, true}) {
                reset(mode, constantRefs);
                std::fill(std::begin(scrCompilePub.canonicalStrings), std::end(scrCompilePub.canonicalStrings), 0);
                for (unsigned id = 1; id < 256; ++id) scrCompilePub.canonicalStrings[id] = short(id);
                scrVarPub.canonicalStrCount = 255;
                EmitCanonicalString(256);
                payload(256);
                assert(scrVarPub.canonicalStrCount == 256 && scrCompilePub.canonicalStrings[256] == 256);
                assert(errors == 0 && addRefs == unsigned(constantRefs) && transferRefs == 1 && removeRefs == 0);
            }
        }
        std::puts("PASS: ordinary and developer compilation retain registration and reference ownership");

        for (int mode : {SCR_DEV_NO, SCR_DEV_YES}) {
            reset(mode, true);
            scrCompilePub.canonicalStrings[3] = short(0xAB00);
            scrVarPub.canonicalStrCount = 0xAB00;
            EmitCanonicalString(3);
            payload(0xAB00);
            assert(errors == 0 && scrVarPub.canonicalStrCount == 0xAB00);
            assert(addRefs == 1 && transferRefs == 1 && removeRefs == 0);
        }
        std::puts("PASS: existing high-bit canonical IDs retain their full two-byte payload");

        reset(SCR_DEV_IGNORE, true);
        EmitCanonicalString(1);
        assert(allocations == 1 && errors == 0 && addRefs == 0 && transferRefs == 0 && removeRefs == 1);
        assert(std::all_of(code.begin(), code.end(), [](byte value) { return value == 0xA5; }));
        std::puts("PASS: ignored developer statements preserve string cleanup and leave output bytes untouched");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--repo", type=Path, default=root)
    parser.add_argument("--source", type=Path, help="Override the active compiler source for negative probes")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    header = (args.repo / "src/script/scr_compiler.h").read_text()
    enum_start = header.rfind("enum : __int32", 0, header.index("SCR_DEV_NO"))
    modes = block(header, enum_start).replace("__int32", "int") + ";\n"
    archive_header = (args.repo / "src/script/scr_evaluate.h").read_text()
    archive_type = block(archive_header, archive_header.index("struct ArchivedCanonicalStringInfo")) + ";\n"
    source = (args.source or args.repo / "src/script/scr_compiler2.cpp").read_text()
    program = PREFIX + modes + archive_type + DOUBLES
    for file, name in [
        ("scr_evaluate.cpp", "Scr_CompileCanonicalString"),
        ("scr_main.cpp", "SL_TransferToCanonicalString"),
        ("scr_vm.cpp", "Scr_ReadUnsignedShort"),
    ]:
        program += function((args.repo / "src/script" / file).read_text(), name) + "\n"
    program += function(source, "EmitCanonicalString") + SCENARIOS
    with tempfile.TemporaryDirectory(prefix="kisakcod-canonical-fields-") as folder:
        cpp = Path(folder) / "canonical_fields.cpp"
        executable = Path(folder) / "canonical_fields"
        cpp.write_text(program)
        for variant in ("SP", "MP"):
            subprocess.run(shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", f"-DKISAK_{variant}", str(cpp), "-o", str(executable),
            ], check=True)
            print(f"Canonical field ID checks: {variant}", flush=True)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
