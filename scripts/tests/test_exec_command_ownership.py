#!/usr/bin/env python3
"""Exercise production config execution across hunk and fastfile invalidation.

The disk/fastfile exec functions and synchronous command parser are extracted
from production. Authored file/asset/zone services model map restart ownership;
no game assets, renderer or Windows process are needed.
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
        rf"^(?:static\s+)?(?:void|char\s*\*?)\s*__cdecl\s+{name}\s*\([^;{{}}]*\)\s*\{{",
        source, re.MULTILINE,
    )
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
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
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
#define iassert(condition) assert(condition)
constexpr int ERR_FATAL = 1;
constexpr int ERR_DROP = 2;
constexpr int CON_CHANNEL_SYSTEM = 1;
constexpr int ASSET_TYPE_RAWFILE = 1;
struct RawFile { const char* name; int len; const char* buffer; };
union XAssetHeader { RawFile* rawfile; };
struct Asset {
    std::vector<char> bytes;
    RawFile raw{};
    explicit Asset(const std::string& text) : bytes(text.begin(), text.end()) {
        bytes.push_back(0); raw.len = int(text.size()); raw.buffer = bytes.data();
    }
};
struct TemporaryFile {
    std::unique_ptr<char[]> data;
    unsigned size;
    bool magic = true;
    bool released = false;
};
std::map<std::string, std::string> files;
std::map<std::string, std::unique_ptr<Asset>> assets;
std::vector<TemporaryFile> temporary;
std::map<void*, int> zone;
std::vector<std::string> commands;
unsigned fsReads, fsFrees, zoneAllocs, zoneFrees, peakZone, databaseReads;
bool failZone, requireReleasedFile;
bool minimumLoaded = true;
bool g_anyFastFileLoaded = true;
int cmd_wait;
static char* s_execBuffers[CMD_MAX_NESTING]{};
static uint32_t s_execBufferCount;
struct CmdArgs {};
struct CmdArgsPrivate {};
CmdArgs cmd_args, sv_cmd_args;
CmdArgsPrivate cmd_argsPrivate, sv_cmd_argsPrivate;
bool cmd_insideCBufExecute[1]{};
unsigned resetContexts;
std::jmp_buf abortTarget;
bool abortArmed, cleanupDuringFree;
unsigned repeatAborts;
std::vector<void*> allocationOrder, releaseOrder;
void Cmd_ComErrorCleanup();
void Cmd_ResetArgs(CmdArgs* args, CmdArgsPrivate* storage) {
    if (args == &cmd_args && storage == &cmd_argsPrivate) resetContexts |= 1;
    else if (args == &sv_cmd_args && storage == &sv_cmd_argsPrivate) resetContexts |= 2;
    else throw std::runtime_error("Unexpected command context reset");
}

[[noreturn]] void Com_Error(int code, const char*, ...) {
    assert(code == ERR_FATAL || code == ERR_DROP);
    if (code == ERR_DROP && abortArmed) std::longjmp(abortTarget, 1);
    throw std::runtime_error("Command-buffer allocation failure was reported");
}
void Com_Printf(int, const char*, ...) {}
bool DB_IsMinimumFastFileLoaded() { return minimumLoaded; }
XAssetHeader DB_FindXAssetHeader(int kind, const char* name) {
    assert(kind == ASSET_TYPE_RAWFILE); ++databaseReads;
    auto found = assets.find(name);
    return {found == assets.end() ? nullptr : &found->second->raw};
}
int FS_ReadFile(const char* name, void** buffer) {
    ++fsReads;
    auto found = files.find(name);
    if (found == files.end()) { *buffer = nullptr; return -1; }
    TemporaryFile file{};
    file.size = unsigned(found->second.size() + 1);
    file.data = std::make_unique<char[]>(file.size);
    std::memcpy(file.data.get(), found->second.c_str(), file.size);
    *buffer = file.data.get();
    temporary.push_back(std::move(file));
    return int(found->second.size());
}
void FS_FreeFile(char* buffer) {
    for (auto& file : temporary) if (file.data.get() == buffer) {
        if (!file.magic)
            throw std::runtime_error("Hunk_FreeTempMemory: bad magic after map restart");
        if (file.released) throw std::runtime_error("File buffer was freed twice");
        file.released = true; file.magic = false; ++fsFrees;
        std::fill_n(file.data.get(), file.size, '#');
        return;
    }
    throw std::runtime_error("An unowned file buffer was freed");
}
void* Z_Malloc(int size, const char*, int type) {
    assert(size > 0 && type == 10);
    if (failZone) return nullptr;
    void* result = std::malloc(unsigned(size)); assert(result);
    assert(zone.emplace(result, size).second); ++zoneAllocs;
    allocationOrder.push_back(result);
    peakZone = std::max(peakZone, unsigned(zone.size()));
    return result;
}
void Z_Free(void* memory, int type) {
    assert(type == 10);
    if (zone.erase(memory) != 1) throw std::runtime_error("Zone buffer was not owned");
    releaseOrder.push_back(memory);
    if (cleanupDuringFree) { cleanupDuringFree = false; Cmd_ComErrorCleanup(); }
    std::free(memory); ++zoneFrees;
}
void clearMapMemory() {
    // Retain backing storage while poisoning it so the old parser's invalid
    // lifetime is deterministic, without dereferencing host-freed memory.
    for (auto& file : temporary) if (!file.released) {
        file.magic = false;
        std::fill_n(file.data.get(), file.size, '#');
    }
    for (auto& pair : assets) {
        auto& bytes = pair.second->bytes;
        std::fill(bytes.begin(), bytes.end(), '#'); bytes.back() = 0;
    }
}
void Cbuf_ExecuteBuffer(int32_t, int32_t, const char*);
char Cmd_ExecFromDisk(int32_t, int32_t, const char*);
char Cmd_ExecFromFastFile(int32_t, int32_t, const char*);
void Cmd_ExecuteSingleCommand(int32_t local, int32_t controller, char* text) {
    assert(local == 0 && controller == 7);
    if (requireReleasedFile) for (const auto& file : temporary)
        if (!file.released) throw std::runtime_error("Disk exec retained a temporary file at dispatch");
    // No nontrivial automatic object may remain alive across the real longjmp.
    commands.emplace_back(text);
    if (std::strcmp(text, "abort") == 0) {
        assert(abortArmed); std::longjmp(abortTarget, 1);
    }
    if (std::strcmp(text, "restart") == 0) clearMapMemory();
    else if (std::strcmp(text, "wait 2") == 0) cmd_wait = 2;
    else if (std::strncmp(text, "disk ", 5) == 0) {
        assert(Cmd_ExecFromDisk(local, controller, text + 5) == 1);
    } else if (std::strncmp(text, "fast ", 5) == 0) {
        assert(Cmd_ExecFromFastFile(local, controller, text + 5) == 1);
    }
}
"""

SCENARIOS = r"""
void reset() {
    if (!zone.empty()) throw std::runtime_error("Zone command buffer leaked");
    for (const auto& file : temporary)
        if (!file.released) throw std::runtime_error("Temporary file buffer leaked");
    files.clear(); assets.clear(); temporary.clear(); commands.clear();
    fsReads = fsFrees = zoneAllocs = zoneFrees = peakZone = databaseReads = 0;
    failZone = requireReleasedFile = false;
    minimumLoaded = g_anyFastFileLoaded = true; cmd_wait = 0;
    assert(s_execBufferCount == 0);
    for (auto buffer : s_execBuffers) assert(!buffer);
    allocationOrder.clear(); releaseOrder.clear(); resetContexts = 0;
    abortArmed = cleanupDuringFree = false; repeatAborts = 0;
}
void expect(std::initializer_list<const char*> wanted) {
    if (commands != std::vector<std::string>(wanted.begin(), wanted.end()))
        throw std::runtime_error("Restart invalidated command text or changed synchronous ordering");
    if (!zone.empty() || fsReads != fsFrees)
        throw std::runtime_error("Command execution left an owned allocation");
}
void addAsset(const char* name, const std::string& text) {
    assets.emplace(name, std::make_unique<Asset>(text));
}
int main() {
    try {
        reset(); files["restart.cfg"] = "before;restart;after\n";
        assert(Cmd_ExecFromDisk(0, 7, "restart.cfg") == 1);
        expect({"before", "restart", "after"});
        assert(zoneAllocs == 1 && zoneFrees == 1);
        std::puts("PASS: disk commands survive hunk reset and retain trailing commands");

        reset(); requireReleasedFile = true;
        files["startup.cfg"] = "set before;set after\n";
        assert(Cmd_ExecFromDisk(0, 7, "startup.cfg") == 1);
        expect({"set before", "set after"});
        std::puts("PASS: disk temporary storage is released before synchronous dispatch");

        reset(); addAsset("restart.cfg", "before;restart;after\n");
        assert(Cmd_ExecFromFastFile(0, 7, "restart.cfg") == 1);
        expect({"before", "restart", "after"});
        assert(zoneAllocs == 1 && zoneFrees == 1);
        std::puts("PASS: fastfile commands survive asset unload and retain trailing commands");

        reset(); requireReleasedFile = true;
        files["outer.cfg"] = "outer-before;disk inner.cfg;outer-after\n";
        files["inner.cfg"] = "inner-before;restart;inner-after\n";
        assert(Cmd_ExecFromDisk(0, 7, "outer.cfg") == 1);
        expect({"outer-before", "disk inner.cfg", "inner-before", "restart", "inner-after", "outer-after"});
        assert(peakZone == 2 && zoneAllocs == 2 && zoneFrees == 2);
        std::puts("PASS: nested disk execution keeps both command owners alive across restart");

        reset(); addAsset("outer.cfg", "outer-before;fast inner.cfg;outer-after\n");
        addAsset("inner.cfg", "inner-before;restart;inner-after\n");
        assert(Cmd_ExecFromFastFile(0, 7, "outer.cfg") == 1);
        expect({"outer-before", "fast inner.cfg", "inner-before", "restart", "inner-after", "outer-after"});
        assert(peakZone == 2 && zoneAllocs == 2 && zoneFrees == 2);
        std::puts("PASS: nested fastfile execution owns text independently of both unloaded assets");

        reset(); files["empty.cfg"] = "";
        assert(Cmd_ExecFromDisk(0, 7, "empty.cfg") == 1);
        expect({}); assert(fsFrees == 1 && zoneAllocs == 1 && zoneFrees == 1);
        std::puts("PASS: empty disk config remains a successful balanced execution");

        reset(); addAsset("empty.cfg", "");
        assert(Cmd_ExecFromFastFile(0, 7, "empty.cfg") == 1);
        expect({}); assert(zoneAllocs == 1 && zoneFrees == 1);
        std::puts("PASS: empty fastfile config remains a successful balanced execution");

        reset(); assert(Cmd_ExecFromDisk(0, 7, "missing.cfg") == 0);
        assert(commands.empty() && zoneAllocs == 0 && fsFrees == 0);
        std::puts("PASS: missing disk config does not allocate or execute");

        reset(); assert(Cmd_ExecFromFastFile(0, 7, "missing.cfg") == 0);
        assert(commands.empty() && zoneAllocs == 0 && databaseReads == 1);
        std::puts("PASS: missing fastfile config does not allocate or execute");

        reset(); minimumLoaded = g_anyFastFileLoaded = false;
        addAsset("early.cfg", "unexpected\n");
        assert(Cmd_ExecFromFastFile(0, 7, "early.cfg") == 0);
        assert(commands.empty() && zoneAllocs == 0 && databaseReads == 0);
        std::puts("PASS: fastfile readiness still gates config execution");

        reset(); files["wait.cfg"] = "before;wait 2;after\n";
        assert(Cmd_ExecFromDisk(0, 7, "wait.cfg") == 1);
        expect({"before", "wait 2", "after"}); assert(cmd_wait == 2);
        std::puts("PASS: existing synchronous wait and startup-config ordering are preserved");

        reset(); files["quoted.cfg"] = "set text \"a;b\";tail\n";
        assert(Cmd_ExecFromDisk(0, 7, "quoted.cfg") == 1);
        expect({"set text \"a;b\"", "tail"});
        std::puts("PASS: quoted semicolons retain the production parser behavior");

        reset(); failZone = true; files["oom.cfg"] = "unexpected\n";
        bool failed = false;
        try { Cmd_ExecFromDisk(0, 7, "oom.cfg"); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed && commands.empty() && fsFrees == 1 && zone.empty());
        std::puts("PASS: disk allocation failure releases temporary storage without dispatch");

        reset(); failZone = true; addAsset("oom.cfg", "unexpected\n");
        failed = false;
        try { Cmd_ExecFromFastFile(0, 7, "oom.cfg"); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed && commands.empty() && zone.empty());
        std::puts("PASS: fastfile allocation failure remains an explicit error without dispatch");

        reset(); files["repeat.cfg"] = "before;restart;after\n";
        for (unsigned i = 0; i < 32; ++i)
            assert(Cmd_ExecFromDisk(0, 7, "repeat.cfg") == 1);
        assert(zone.empty() && zoneAllocs == 32 && zoneFrees == 32 && fsFrees == 32);
        std::puts("PASS: repeated disk restarts release every temporary and zone owner");

        reset(); files["abort.cfg"] = "abort;unreachable\n"; abortArmed = true;
        cmd_insideCBufExecute[0] = true;
        if (setjmp(abortTarget) == 0) {
            Cmd_ExecFromDisk(0, 7, "abort.cfg");
            throw std::runtime_error("Abort command unexpectedly returned");
        }
        abortArmed = false;
        assert(zone.size() == 1 && fsFrees == 1);
        Cmd_ComErrorCleanup(); Cmd_ComErrorCleanup();
        assert(zone.empty() && zoneFrees == 1 && resetContexts == 3 && !cmd_insideCBufExecute[0]);
        std::puts("PASS: nonlocal disk-command errors release retained text exactly once");

        reset(); addAsset("abort.cfg", "abort;unreachable\n"); abortArmed = true;
        if (setjmp(abortTarget) == 0) {
            Cmd_ExecFromFastFile(0, 7, "abort.cfg");
            throw std::runtime_error("Abort command unexpectedly returned");
        }
        abortArmed = false;
        assert(zone.size() == 1); Cmd_ComErrorCleanup();
        assert(zone.empty() && zoneFrees == 1);
        std::puts("PASS: nonlocal fastfile-command errors release retained text");

        reset(); files["outer.cfg"] = "disk inner.cfg;unreachable\n";
        files["inner.cfg"] = "abort;unreachable\n"; abortArmed = true;
        if (setjmp(abortTarget) == 0) {
            Cmd_ExecFromDisk(0, 7, "outer.cfg");
            throw std::runtime_error("Nested abort unexpectedly returned");
        }
        abortArmed = false;
        assert(zone.size() == 2 && fsFrees == 2); Cmd_ComErrorCleanup();
        assert(zone.empty() && zoneFrees == 2);
        assert(releaseOrder == std::vector<void*>(allocationOrder.rbegin(), allocationOrder.rend()));
        std::puts("PASS: nested nonlocal errors release command owners in reverse order");

        reset(); addAsset("outer.cfg", "disk inner.cfg;unreachable\n");
        files["inner.cfg"] = "abort;unreachable\n"; abortArmed = true;
        if (setjmp(abortTarget) == 0) {
            Cmd_ExecFromFastFile(0, 7, "outer.cfg");
            throw std::runtime_error("Nested abort unexpectedly returned");
        }
        abortArmed = false; cleanupDuringFree = true;
        Cmd_ComErrorCleanup();
        assert(zone.empty() && zoneFrees == 2 && !cleanupDuringFree && s_execBufferCount == 0);
        std::puts("PASS: clearing ownership before free makes reentrant error cleanup idempotent");

        reset();
        for (unsigned i = 0; i < CMD_MAX_NESTING; ++i)
            files["nest" + std::to_string(i)] = i + 1 == CMD_MAX_NESTING
                ? "last\n" : "disk nest" + std::to_string(i + 1) + "\n";
        assert(Cmd_ExecFromDisk(0, 7, "nest0") == 1);
        assert(peakZone == CMD_MAX_NESTING && zoneAllocs == CMD_MAX_NESTING && zone.empty());
        std::puts("PASS: the configured maximum nesting owns and releases every buffer");

        reset();
        for (unsigned i = 0; i <= CMD_MAX_NESTING; ++i)
            files["nest" + std::to_string(i)] = "disk nest" + std::to_string(i + 1) + "\n";
        abortArmed = true;
        if (setjmp(abortTarget) == 0) {
            Cmd_ExecFromDisk(0, 7, "nest0");
            throw std::runtime_error("Excess nesting unexpectedly returned");
        }
        abortArmed = false; Cmd_ComErrorCleanup();
        assert(zone.empty() && zoneAllocs == CMD_MAX_NESTING && zoneFrees == CMD_MAX_NESTING);
        // The surrounding Com_ErrorCleanup resets hunk/file bookkeeping too.
        for (auto& file : temporary) { file.released = true; file.magic = false; }
        std::puts("PASS: excess nesting drops safely and error cleanup releases bounded owners");

        reset(); files["abort.cfg"] = "abort\n";
        while (repeatAborts < 32) {
            abortArmed = true;
            if (setjmp(abortTarget) == 0) {
                Cmd_ExecFromDisk(0, 7, "abort.cfg");
                throw std::runtime_error("Repeated abort unexpectedly returned");
            }
            abortArmed = false; Cmd_ComErrorCleanup(); ++repeatAborts;
            assert(zone.empty());
        }
        assert(zoneAllocs == 32 && zoneFrees == 32 && fsFrees == 32);
        std::puts("PASS: repeated nonlocal errors do not accumulate copied command buffers");
        reset();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Exec command ownership regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "src/qcommon/cmd.cpp")
    parser.add_argument("--header", type=Path,
                        default=Path(__file__).resolve().parents[2] / "src/qcommon/cmd.h")
    args = parser.parse_args()
    source = args.source.read_text()
    nesting = re.search(r"^#define\s+CMD_MAX_NESTING\s+(\d+)", args.header.read_text(), re.MULTILINE)
    if nesting is None:
        raise ValueError("CMD_MAX_NESTING definition was not found")
    names = ["Cbuf_ExecuteBuffer", "Cmd_ComErrorCleanup", "Cmd_ExecFromDisk", "Cmd_ExecFromFastFile"]
    if re.search(r"^static char \*__cdecl Cmd_AllocExecBuffer", source, re.MULTILINE):
        names[:0] = ["Cmd_AllocExecBuffer", "Cmd_FreeExecBuffer"]
    functions = "\n".join(extract(source, name) for name in names)
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")
    with tempfile.TemporaryDirectory(prefix="kisakcod-exec-command-ownership-") as folder:
        root = Path(folder)
        generated = root / "exec-ownership.cpp"
        generated.write_text(f"#define CMD_MAX_NESTING {nesting[1]}\n" + HARNESS + "\n" + functions + "\n" + SCENARIOS)
        for kind in ("SP", "MP"):
            executable = root / f"exec-ownership-{kind.lower()}"
            compiled = subprocess.run(compiler + [
                "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-parentheses", "-Wno-sign-compare",
                f"-DKISAK_{kind}", str(generated), "-o", str(executable),
            ], check=False)
            if compiled.returncode:
                return 1
            tested = subprocess.run([str(executable)], check=False)
            if tested.returncode:
                return 1
            print(f"Completed {kind} config-execution ownership checks", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
