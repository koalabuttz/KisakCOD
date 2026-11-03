#!/usr/bin/env python3
"""Build and run tests/linux_sl_trace/sl_trace_test.cpp for i386 and x86_64.

Checks the opt-in script string reference trace (src/linux/sl_trace.cpp with
KISAK_SL_TRACE): events are kept per string, a freed string's history is
cleared, old events roll off with a note, each event carries a backtrace,
lifetime totals per call stack keep every event, and unknown or out-of-range
strings are handled. Asset-free.

Usage: python3 scripts/tests/test_linux_sl_trace.py [--cxx clang++]
"""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "tests" / "linux_sl_trace" / "sl_trace_test.cpp"
SOURCE = ROOT / "src" / "linux" / "sl_trace.cpp"


def section(text, header):
    """The dump lines that follow one 'leaked string' header."""
    start = text.index(header)
    following = text.find("SL_TRACE leaked string", start + len(header))
    return text[start:following if following >= 0 else len(text)]


def check(text):
    failures = []

    def require(condition, message):
        print(("PASS: " if condition else "FAIL: ") + message)
        if not condition:
            failures.append(message)

    require("sl_trace_test: done" in text, "test ran to the end")
    seven = section(text, "SL_TRACE leaked string 7:")
    require('user 0x00, debug refs 1, 10 chars, text "slot_seven"' in seven, "header names the string")
    events = re.findall(r"SL_TRACE   event \d+ \(seq \d+, thread \d+\): (\S+) user (0x[0-9a-f]+)", seven)
    require(events == [("new", "0x04"), ("add-user", "0x04"), ("add-ref", "0x00")],
            f"freed string's history was cleared; reused slot shows its own 3 events (got {events})")
    require("SlTraceTestTakeReference" in seven, "events carry a backtrace that names the caller")
    sites = re.findall(r"SL_TRACE   site \d+: (\d+) adds, (\d+) removes", seven)
    require("3 call sites over the string's life" in seven and sorted(sites) == [("1", "0")] * 3,
            f"lifetime call-site totals restart when the string is freed (got {sites})")
    nine = section(text, "SL_TRACE leaked string 9:")
    require("4 older events dropped" in nine, "history keeps the newest 16 events and says how many rolled off")
    require(len(re.findall(r"SL_TRACE   event ", nine)) == 16, "16 events printed for the long history")
    require(re.findall(r"SL_TRACE   site \d+: (\d+) adds, (\d+) removes", nine) == [("20", "0")],
            "call-site totals keep every event, not only the newest 16")
    eleven = section(text, "SL_TRACE leaked string 11:")
    require("no recorded events" in eleven, "a string with no events says so")
    outside = section(text, "SL_TRACE leaked string 70000:")
    require("no recorded events" in outside, "out-of-range strings are ignored")
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--timeout", type=int, default=180)
    options = parser.parse_args()
    ok = True
    with tempfile.TemporaryDirectory(prefix="kisak-sl-trace-") as workdir:
        for arch in ("-m32", "-m64"):
            binary = Path(workdir) / f"sl_trace_test{arch}"
            # -rdynamic only so backtrace_symbols can name the test's caller.
            build = [options.cxx, arch, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                     "-DKISAK_SL_TRACE", "-I", str(ROOT / "src"), str(TEST), str(SOURCE),
                     "-rdynamic", "-o", str(binary)]
            print("+", " ".join(build), flush=True)
            if subprocess.run(build, timeout=options.timeout, check=False).returncode != 0:
                print(f"FAIL: sl_trace_test{arch} did not build")
                ok = False
                continue
            completed = subprocess.run([str(binary)], timeout=options.timeout, capture_output=True,
                                       text=True, check=False)
            text = completed.stdout + completed.stderr
            print(f"--- {arch}")
            if completed.returncode != 0:
                print(text)
                print(f"FAIL: sl_trace_test{arch} exited {completed.returncode}")
                ok = False
                continue
            ok = check(text) and ok
    print("PASS: script string trace" if ok else "FAIL: script string trace")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
