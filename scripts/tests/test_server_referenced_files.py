#!/usr/bin/env python3
"""Exercise bounded server file references from the production filesystem code.

The real arrays, setters, shared parser and shutdown functions are extracted.
Command tokenization and string allocation are checked doubles; no assets,
network service or Windows runtime are needed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source: str, name: str) -> str:
    signature = re.search(
        rf"^(?:void|int)\s+(?:__cdecl\s+)?{name}\s*\([^;{{}}]*\)\s*\{{",
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
            if depth == 0:
                return source[signature.start():token.end()].replace("__cdecl", "")
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
#define iassert(condition) assert(condition)
#define ARRAY_COUNT(array) (int(sizeof(array) / sizeof((array)[0])))
constexpr int ERR_DROP = 1;
std::vector<std::string> tokens;
std::set<const char*> strings;
unsigned copies, releases, tokenBegins, tokenEnds;
bool tokenized;
void Cmd_TokenizeString(const char* text) {
    assert(!tokenized);
    tokenized = true;
    ++tokenBegins;
    tokens.clear();
    std::istringstream input(text);
    std::string token;
    while (input >> token) tokens.push_back(token);
}
int Cmd_Argc() { assert(tokenized); return int(tokens.size()); }
const char* Cmd_Argv(int index) {
    assert(tokenized);
    return tokens.at(unsigned(index)).c_str();
}
void Cmd_EndTokenizedString() {
    assert(tokenized);
    tokenized = false;
    ++tokenEnds;
    tokens.clear();
}
const char* CopyString(const char* text) {
    auto copy = new char[std::strlen(text) + 1];
    std::strcpy(copy, text);
    assert(strings.insert(copy).second);
    ++copies;
    return copy;
}
void FreeString(const char* text) {
    assert(strings.erase(text) == 1);
    ++releases;
    delete[] text;
}
struct Drop : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void Com_Error(int code, const char* text) {
    assert(code == ERR_DROP);
    throw Drop(text);
}
"""

SCENARIOS = r"""
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string list(unsigned count, bool names, unsigned seed = 0) {
    std::string text;
    for (unsigned i = 0; i < count; ++i) {
        if (i) text += ' ';
        if (names) text += "mods/fixture/file_";
        text += std::to_string(seed + i);
    }
    return text;
}
void verify(unsigned expected, unsigned seed = 0, bool iwd = false) {
    const auto count = iwd ? fs_numServerReferencedIwds : fs_numServerReferencedFFs;
    const auto sums = iwd ? fs_serverReferencedIwds : fs_serverReferencedFFCheckSums;
    const auto names = iwd ? fs_serverReferencedIwdNames : fs_serverReferencedFFNames;
    const auto capacity = iwd ? ARRAY_COUNT(fs_serverReferencedIwdNames) : ARRAY_COUNT(fs_serverReferencedFFNames);
    check(count == int(expected) && count <= capacity, "Referenced file count exceeded array capacity");
    for (unsigned i = 0; i < expected; ++i) {
        check(sums[i] == int(seed + i), "Checksum order changed");
        check(names[i] && std::string(names[i]) == "mods/fixture/file_" + std::to_string(seed + i),
              "Name/checksum association changed");
    }
    for (int i = count; i < capacity; ++i)
        check(names[i] == nullptr, "Old file name survived shorter server reference list");
    check(!tokenized && tokenBegins == tokenEnds, "Successful parsing leaked command-token context");
}
void set(unsigned sums, unsigned names, unsigned seed = 0, bool iwd = false) {
    auto sumText = list(sums, false, seed);
    auto nameText = list(names, true, seed);
    if (iwd) FS_ServerSetReferencedIwds(sumText.data(), nameText.data());
    else FS_ServerSetReferencedFFs(sumText.data(), nameText.data());
    // Tokenizer memory and both inputs are intentionally invalidated on return.
    std::fill(sumText.begin(), sumText.end(), 'x');
    std::fill(nameText.begin(), nameText.end(), 'x');
}
void shutdown() {
    FS_ShutdownServerReferencedFFs();
    FS_ShutdownServerReferencedIwds();
    verify(0);
    verify(0, 0, true);
    check(strings.empty() && copies == releases, "Server references leaked or freed an allocation twice");
}
void mismatch(unsigned sums, unsigned names) {
    set(4, 4);
    const auto before = copies;
    bool dropped = false;
    try { set(sums, names); }
    catch (const Drop& error) {
        check(std::string(error.what()) == "file sum/name mismatch", "Unexpected malformed-list error");
        dropped = true;
        // Com_Error's caller normally resets the tokenizer on the nonlocal-error path.
        if (tokenized) Cmd_EndTokenizedString();
    }
    check(dropped && copies == before && strings.empty(),
          "Mismatched reference list accepted, allocated names, or retained previous server names");
    verify(0);
}
int main() {
    try {
        set(0, 0);
        verify(0);
        std::puts("PASS: empty server reference list remains empty");

        set(1, 1, 100);
        verify(1, 100);
        std::puts("PASS: one reference retains independent name ownership and checksum");

        set(32, 32, 200);
        verify(32, 200);
        std::puts("PASS: complete 32-entry fastfile list fits both arrays");

        set(33, 33, 300);
        verify(32, 300);
        std::puts("PASS: 33-entry fastfile list is bounded by real array capacity");

        set(1024, 1024, 400);
        verify(32, 400);
        std::puts("PASS: oversized fastfile list preserves first 32 name/checksum pairs");

        for (unsigned repetition = 0; repetition < 100; ++repetition) {
            for (auto count : {32u, 1u, 31u, 0u, 33u, 0u}) {
                set(count, count, repetition * 100);
                verify(std::min(count, 32u), repetition * 100);
            }
        }
        check(strings.empty() && copies == releases, "Repeated server changes leaked reference strings");
        std::puts("PASS: repeated long, short and empty transitions release all previous names");

        mismatch(3, 2);
        mismatch(2, 3);
        std::puts("PASS: unequal bounded name/checksum lists still drop without allocating names");

        mismatch(1, 0);
        mismatch(0, 1);
        std::puts("PASS: missing name or checksum list still drops and clears previous server names");

        set(33, 34, 500);
        verify(32, 500);
        std::puts("PASS: existing per-list truncation semantics retain matched prefix pairs");

        std::string checksums = "-2147483648 0 2147483647";
        std::string names = "minimum zero maximum";
        FS_ServerSetReferencedFFs(checksums.data(), names.data());
        check(fs_numServerReferencedFFs == 3 && fs_serverReferencedFFCheckSums[0] == (-2147483647 - 1)
                  && fs_serverReferencedFFCheckSums[1] == 0 && fs_serverReferencedFFCheckSums[2] == 2147483647,
              "Signed checksum representation changed");
        std::puts("PASS: complete signed checksum range is preserved");

        set(1024, 1024, 1000, true);
        verify(1024, 1000, true);
        set(33, 33, 2000);
        verify(32, 2000);
        verify(1024, 1000, true);
        set(1025, 1025, 3000, true);
        verify(1024, 3000, true);
        verify(32, 2000);
        std::puts("PASS: IWD capacity stays 1024 and fastfile updates do not alter IWD references");

        shutdown();
        shutdown();
        std::puts("PASS: repeated shutdown is safe and all owned strings are released exactly once");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Server file reference regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--source", type=Path, default=root / "src/qcommon/files.cpp")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    source = args.source.read_text()
    parts = [HARNESS]
    for name in ("fs_numServerReferencedIwds", "fs_serverReferencedIwdNames", "fs_serverReferencedIwds",
                 "fs_numServerReferencedFFs", "fs_serverReferencedFFNames", "fs_serverReferencedFFCheckSums"):
        declaration = re.search(rf"^(?:int|const char \*)\s*{name}(?:\[\d+\])?;", source, re.MULTILINE)
        if declaration is None:
            raise ValueError(f"{name} declaration was not found")
        parts.append(declaration.group())
    for name in ("FS_ShutdownServerFileReferences", "FS_ShutdownServerReferencedIwds", "FS_ServerSetReferencedFiles",
                 "FS_ServerSetReferencedIwds", "FS_ShutdownServerReferencedFFs", "FS_ServerSetReferencedFFs"):
        parts.append(extract(source, name))
    parts.append(SCENARIOS)
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="kisakcod-server-references-") as folder:
        test = Path(folder) / "references.cpp"
        executable = Path(folder) / "references-test"
        test.write_text("\n".join(parts))
        subprocess.run(compiler + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                   "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                                   "-fno-sanitize-recover=all", str(test), "-o", str(executable)], check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
