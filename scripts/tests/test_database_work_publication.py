#!/usr/bin/env python3
"""Exercise production DB_LoadXZone publication with a deterministic consumer.

The notification double can consume immediately, before the producer resumes,
or defer consumption until DB_LoadXZone returns. This checks the scheduling
contract without assets, engine linking, or claiming to reproduce a native
thread race. The source assertions remain executable. Both variants run as
native host executables with fatal address/undefined-behavior sanitizers.
Use --source with a pre-fix db_registry.cpp to verify negative coverage.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_linux_fastfile_io import extract


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct XZoneInfo { const char *name; unsigned allocFlags; unsigned freeFlags; };
struct QueuedZone { char name[64]; unsigned flags; };
QueuedZone g_zoneInfo[8];
unsigned g_zoneCount = 0, g_zoneInfoCount = 0, g_loadingAssets = 0;
constexpr int ERR_DROP = 1, CON_CHANNEL_SYSTEM = 0;
struct EngineError : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void Com_Error(int, const char *message) { throw EngineError(message); }
void MyAssertHandler(const char *, int, int, const char *, const char *message) {
    throw EngineError(message);
}
void Com_Printf(int, const char *, ...) {}
void I_strncpyz(char *dest, const char *src, int size) {
    std::strncpy(dest, src, size - 1); dest[size - 1] = '\0';
}
std::vector<XZoneInfo> expected;
std::vector<int> events;
unsigned consumed = 0;
bool immediate = true;
void consume() {
    require(g_zoneInfoCount == expected.size(), "consumer saw incomplete queue count");
    require(g_loadingAssets == expected.size(), "consumer saw inconsistent loading count");
    for (unsigned i = 0; i < expected.size(); ++i) {
        require(std::string(g_zoneInfo[i].name) == std::string(expected[i].name).substr(0, 63),
                "consumer saw incomplete or stale zone name");
        require(g_zoneInfo[i].flags == expected[i].allocFlags, "consumer saw wrong zone flags");
    }
    // Model a completed worker batch, allowing a subsequent submission.
    g_zoneInfoCount = 0;
    g_loadingAssets = 0;
    ++consumed;
}
void Sys_WakeDatabase2() { events.push_back(2); }
void Sys_WakeDatabase() { events.push_back(1); }
void Sys_NotifyDatabase() {
    events.push_back(3);
    require(events == std::vector<int>({2, 1, 3}), "batch notified more than once or before resets");
    require(!expected.empty(), "empty batch notified worker");
    if (immediate) consume();
}
'''

SCENARIOS = r'''
void batch(std::vector<XZoneInfo> input, bool inlineConsumer) {
    expected.clear(); events.clear(); consumed = 0; immediate = inlineConsumer;
    for (auto item : input) if (item.name) expected.push_back(item);
    DB_LoadXZone(input.data(), static_cast<uint32_t>(input.size()));
    if (expected.empty()) {
        require(events.empty(), "empty batch changed completion events");
        require(!g_zoneInfoCount && !g_loadingAssets, "empty batch created work");
    } else {
        require(events == std::vector<int>({2, 1, 3}), "missing batch notification");
        if (!inlineConsumer) consume();
        require(consumed == 1, "batch was not consumed exactly once");
        require(!g_zoneInfoCount && !g_loadingAssets, "producer overwrote consumed state");
    }
}
void rejected(std::vector<XZoneInfo> input, const char *message) {
    events.clear();
    try { DB_LoadXZone(input.data(), static_cast<uint32_t>(input.size())); }
    catch (const EngineError &error) {
        require(std::string(error.what()) == message, "wrong source assertion");
        require(events.empty(), "invalid batch woke worker");
        return;
    }
    throw std::runtime_error("source precondition was not enforced");
}
int main() {
    try {
        for (bool inlineConsumer : {true, false}) {
            batch({}, inlineConsumer);
            batch({{nullptr, 0, 1}, {nullptr, 0, 2}}, inlineConsumer);
            batch({{"common_mp", 1, 0}}, inlineConsumer);
            batch({{"mp_crash", 2, 0}, {nullptr, 0, 1}, {"mp_crash_load", 4, 0}}, inlineConsumer);
            batch({{"a", 1, 0}, {"b", 2, 0}, {"c", 4, 0}, {"d", 8, 0},
                   {"e", 16, 0}, {"f", 32, 0}, {"g", 64, 0}, {"h", 128, 0}}, inlineConsumer);
            std::string longName(100, 'x');
            batch({{longName.c_str(), 256, 0}}, inlineConsumer);
            // Repeated different-sized batches must not expose the old queue tail.
            for (unsigned i = 0; i < 32; ++i) {
                batch({{"mp_backlot", 2, 0}, {"localized_mp_backlot", 4, 0}}, inlineConsumer);
                batch({{"mp_crash", 8, 0}}, inlineConsumer);
            }
            std::puts(inlineConsumer ? "PASS: immediate consumer batches" : "PASS: deferred consumer batches");
        }
        g_zoneCount = 32; rejected({}, "Max zone count exceeded"); g_zoneCount = 0;
        g_zoneInfoCount = 1; rejected({}, "!g_zoneInfoCount"); g_zoneInfoCount = 0;
        g_loadingAssets = 1; rejected({}, "!g_loadingAssets"); g_loadingAssets = 0;
        rejected(std::vector<XZoneInfo>(9, {"overflow", 1, 0}), "zoneInfoCount < ARRAY_COUNT( g_zoneInfo )");
        std::puts("PASS: source preconditions preserved");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2] / "src/database/db_registry.cpp")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a compiler")
    body = extract(args.source.read_text(), "DB_LoadXZone")
    with tempfile.TemporaryDirectory(prefix="kisak-db-publication-") as folder:
        root = Path(folder)
        source = root / "publication.cpp"
        source.write_text(HARNESS + body + SCENARIOS)
        for name, options in (("debug", ["-O0"]), ("release", ["-O2", "-DNDEBUG"])):
            executable = root / name
            subprocess.run(compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                           "-fno-omit-frame-pointer"] + options + [str(source), "-o", str(executable)],
                           check=True, timeout=args.timeout)
            subprocess.run([str(executable)], check=True, timeout=args.timeout)
            print(f"PASS: {name} database work publication (host ASan/UBSan)", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as error:
        print(f"FAIL: {error}", flush=True)
        raise SystemExit(1)
