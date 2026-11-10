#!/usr/bin/env python3
"""Check the PS Vita build's run plan on the host.

``src/vita/vita_run_plan.h`` decides when a run that nobody watches prints its
figures, sends its timed console commands and quits (docs/vita.md, "Runs nobody
watches"): ``vita_statsEvery``, ``vita_quitAfter``, ``vita_quitAfterMenu``,
``vita_quitAfterMap``, ``vita_atMenu`` and ``vita_atMap``. It makes no engine
call and no Vita call, so this test builds ``tests/vita_client/run_plan_test.cpp``
with the host compiler and runs it: the timed-command parser, the default
once-a-minute figures, each quit timer with its last figures and its wait for
the renderer's and the sound's lines, and a run at an emulator's two frames a
second.

It also reads the sources only the Vita compiles (``src/vita``, ``src/gfx_gxm``)
and the frame log's line for printf length modifiers the Vita's C library does
not know (``%zu``, ``%jd``, ``%td``): it prints the letters and reads every
later argument as the wrong type. That is how the first frame log line on the
Vita came out as "zu frames" with a wild read behind it.

And it reads them for the text settings (``vita_atMenu``, ``vita_atMap``,
``vita_keyboardAnswer``): their text is run as console commands or typed for the
player, and a game server sets a client's dvars by name, so each dvar may be
read in one place only, where start-up hands it to a ``StartText`` that keeps
the first value it is given.

Built with AddressSanitizer and UBSan.

Usage:
    python3 scripts/tests/test_vita_run_plan.py [--cxx c++] [--timeout 300]
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "tests" / "vita_client" / "run_plan_test.cpp",
]

# A conversion with a z, j or t length modifier: "%zu", "%-8zd", "%jx", "%td".
UNKNOWN_ON_VITA = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?[zjt][diouxX]")
VITA_ONLY = ("src/vita", "src/gfx_gxm")
VITA_TOO = ("src/qcommon/com_frame_log.h",)


def unknown_format(text):
    return UNKNOWN_ON_VITA.search(text.replace("%%", ""))


def unknown_formats(root=ROOT):
    """["path:line: text"] for every such conversion the Vita would print."""
    files = [root / name for name in VITA_TOO]
    for folder in VITA_ONLY:
        files += sorted(path for path in (root / folder).rglob("*") if path.suffix in (".cpp", ".h", ".c", ".inl"))
    found = []
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if unknown_format(line.split("//", 1)[0]):
                found.append(f"{path.relative_to(root)}:{number}: {line.strip()}")
    return found


# The dvars whose text is run as commands or typed for the player.
START_TEXT_DVARS = ("vita_atMenu", "vita_atMap", "vita_keyboardAnswer")
TEXT_READ = re.compile(
    r"\b(" + "|".join(START_TEXT_DVARS) + r")\s*->\s*(current|latched|reset)\b"
    r"|Dvar_Get\w*\s*\(\s*\"(" + "|".join(START_TEXT_DVARS) + r")\"")
# The one read allowed: "<start text>.Take(<dvar> ? <dvar>->current.string : "")".
TAKEN_AT_START = re.compile(r"\.Take\(\s*(\w+)\s*\?\s*(\w+)\s*->\s*current\.string\s*:\s*\"\"\s*\)")


def text_setting_reads(root=ROOT):
    """(problems, dvars taken at start-up) for the text settings in the Vita's sources."""
    problems = []
    taken = []
    for path in sorted(path for path in (root / "src" / "vita").rglob("*") if path.suffix in (".cpp", ".h", ".c", ".inl")):
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = line.split("//", 1)[0]
            reads = [match.group(1) or match.group(3) for match in TEXT_READ.finditer(code)]
            if not reads:
                continue
            allowed = TAKEN_AT_START.search(code)
            if allowed and allowed.group(1) == allowed.group(2) and reads == [allowed.group(1)]:
                taken.append(allowed.group(1))
            else:
                problems.append(f"{path.relative_to(root)}:{number}: {line.strip()}")
    return problems, taken


def check_text_settings() -> bool:
    # The rule finds what it is for, and lets the one allowed read through.
    assert TEXT_READ.search('settings.atMap = vita_atMap ? vita_atMap->current.string : "";')
    assert TEXT_READ.search("return vita_keyboardAnswer && *vita_keyboardAnswer->current.string;")
    assert TEXT_READ.search('const char *text = Dvar_GetString("vita_atMenu");')
    assert not TEXT_READ.search("if (vita_atMenu && s_atMenu.Get()[0])")
    assert TAKEN_AT_START.search('if (!s_atMap.Take(vita_atMap ? vita_atMap->current.string : ""))')
    assert not TAKEN_AT_START.search("s_atMap.Take(vita_atMap->current.string)")

    problems, taken = text_setting_reads()
    if problems:
        print("\n".join(problems))
        print("FAIL: a text setting is read outside its start-up StartText; a game server can set the dvar later, "
              "and its text is run as commands or typed for the player")
        return False
    if sorted(taken) != sorted(START_TEXT_DVARS):
        print(f"FAIL: expected one start-up read of each of {', '.join(START_TEXT_DVARS)}, found {', '.join(taken) or 'none'}")
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cxx", default=shutil.which("c++") or "g++")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    assert unknown_format('"%zu frames"') and unknown_format('"%-8zd"') and unknown_format('"%jx"') and unknown_format('"%td"')
    assert not unknown_format('"%u frames, 100%% zoom, %llu, %s to"')
    found = unknown_formats()
    if found:
        print("\n".join(found))
        print("FAIL: the Vita's printf does not know these length modifiers; cast and use %u or %llu")
        return 1
    if not check_text_settings():
        return 1

    with tempfile.TemporaryDirectory(prefix="kisak-vita-run-plan-") as work:
        binary = Path(work) / "run_plan_test"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        if build.returncode:
            sys.stdout.write(build.stdout + build.stderr)
            print("FAIL: the run plan test did not build")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        sys.stdout.write(run.stdout + run.stderr)
        if run.returncode:
            print("FAIL: vita run plan checks failed")
            return 1
    print("PASS: vita run plan")
    return 0


if __name__ == "__main__":
    sys.exit(main())
