#!/usr/bin/env python3
"""Compile-census the multiplayer client sources for native i386 Linux.

This is the compile proof that has to exist before a client source joins a Linux
target. It performs **no CMake configure and no engine build**: it asks CMake's
script mode for the exact source lists the Windows ``KisakCOD-mp`` target uses
(``scripts/common_files.cmake`` + ``scripts/mp/mp_files.cmake``), then runs one
syntax-only ``-m32`` compile per production ``.cpp`` with the client's
definitions (``KISAK_MP CINEMA USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL
RELEASE_ASSERTS``), against the unmodified sources. Nothing is linked, nothing is
run, and no GPU, DXVK, SDL, Steam install, credentials or game data is involved.

The result is compared with ``scripts/tests/client_compile_census_blocked.txt``,
the reviewed list of sources that are known not to compile natively yet, each
with the reason. The check is a two-way ratchet:

  * a source that fails and is not on the list is a **regression** -- something
    that compiled natively stopped compiling;
  * a source on the list that now compiles is **stale** -- the list must shrink
    in the same change, so it never overstates what is still blocked.

Either one fails the run. A listed path that the CMake lists no longer contain is
also stale. What is compiled is the client source set minus the groups recorded
in ``EXCLUDED_GROUPS`` below (the Windows platform layer, which ``src/linux``
replaces, and vendored third-party code with its own build rules); a group that
the MP target gains or loses fails the run until this file is updated, so the
census cannot silently stop covering part of the client.

What this does not claim
------------------------
A file that passes was accepted by the compiler front end for i386 with these
definitions. That is not a link, not a run and not behavioural equivalence with
the Windows build. In particular, the 29 renderer sources that need real D3D9
methods are listed as blocked here; the native D3D9 boundary is what unblocks
them, and when it does, entries come off the list.

Usage:
    python3 scripts/tests/run_client_compile_census.py
    python3 scripts/tests/run_client_compile_census.py --cxx clang++ --jobs 4 \\
        --log-dir artifacts/client-census
    python3 scripts/tests/run_client_compile_census.py --list   # print, compile nothing

Exit status: 0 when every compile matches the list, 1 on any regression, stale
entry, group drift, timeout or tool failure, 2 on a usage error.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
SCRIPTS = ROOT / "scripts"
BLOCKED_LIST = SCRIPTS / "tests" / "client_compile_census_blocked.txt"
MP_TARGET = SCRIPTS / "mp" / "CMakeLists.txt"
DEFAULT_TIMEOUT = 300

# Every variable the Windows KisakCOD-mp add_executable() expands. The census
# compiles the engine groups; the excluded ones are named with the reason.
CENSUS_GROUPS = (
    "SRCROOT", "DYNENTITY", "EFFECTSCORE", "AIM_ASSIST", "BGAME", "CGAME",
    "CGAME_MP", "CLIENT", "CLIENT_MP", "COMMON", "DATABASE", "DEVGUI", "GAME",
    "GAME_MP", "GFX_D3D", "GROUPVOICE", "PHYSICS", "PHYSICS_ODE", "QCOMMON",
    "RAGDOLL", "SCRIPT", "SERVER", "SERVER_MP", "SOUND", "STRINGED", "UI",
    "UI_MP", "UNIVERSAL", "XANIM",
)
EXCLUDED_GROUPS = {
    "WIN32_SRC": "Windows platform layer; native Linux replaces it with src/linux",
    "BINKLIB": "vendored Bink video glue under deps/",
    "MSSLIB": "Miles Sound System import library; the Linux client uses OpenAL",
    "ODE": "vendored ODE headers under deps/",
    "SPEEX": "vendored Speex headers under deps/",
    "GROUPVOICE_SPEEX": "vendored Speex C codec, compiled as C with its own rules",
    "ZLIB": "vendored zlib C sources under deps/",
    "STEAM_HEADERS": "Steamworks SDK headers, no sources",
    "SOUND_BACKEND": "declared in the target but never set by the source lists",
}

CLIENT_DEFINES = (
    "-DKISAK_MP", "-DCINEMA", "-DUSE_SEPARATE_BLIT_TEXTURE", "-DKISAK_OPENAL",
    "-DRELEASE_ASSERTS", "-D_FILE_OFFSET_BITS=64", "-D_GNU_SOURCE",
)
# The flags the native Linux targets use (scripts/platform/linux/CMakeLists.txt),
# minus code generation: this is a front-end pass.
CLIENT_FLAGS = (
    "-m32", "-std=gnu++20", "-fms-extensions", "-msse2", "-mfpmath=sse",
    "-fno-strict-aliasing", "-Wno-c++11-narrowing", "-Wno-invalid-token-paste",
    "-Wno-ignored-attributes", "-Wno-format-security", "-fsyntax-only",
)

# The build's generator for src/buildnumber.h (see generate_build_header).
BUILD_HEADER_SCRIPT = SCRIPTS / "extern" / "write_build_number.cmake"

LIST_SCRIPT = """
set(SRC_DIR "{src}")
set(SCRIPTS_DIR "{scripts}")
set(DEPS_DIR "{deps}")
include("${{SCRIPTS_DIR}}/common_files.cmake")
include("${{SCRIPTS_DIR}}/mp/mp_files.cmake")
foreach(group {groups})
  foreach(path ${{${{group}}}})
    message("${{group}}|${{path}}")
  endforeach()
endforeach()
"""


class CensusError(Exception):
    """A tool or configuration failure, as opposed to a census mismatch."""


def target_groups(cmake_text):
    """The ``${VAR}`` names listed in the KisakCOD-mp add_executable() call."""
    start = cmake_text.find("add_executable(${PROJECT_NAME}")
    if start < 0:
        raise CensusError(f"{MP_TARGET}: no add_executable(${{PROJECT_NAME}} block")
    end = cmake_text.find(")", start + len("add_executable("))
    if end < 0:
        raise CensusError(f"{MP_TARGET}: unterminated add_executable block")
    block = cmake_text[start:end]
    groups = []
    for token in block.split():
        if token.startswith("${") and token.endswith("}") and token != "${PROJECT_NAME}":
            groups.append(token[2:-1])
    return groups


def check_group_coverage(groups):
    """Every target group is either censused or excluded with a reason."""
    known = set(CENSUS_GROUPS) | set(EXCLUDED_GROUPS)
    problems = []
    for group in groups:
        if group not in known:
            problems.append(f"KisakCOD-mp gained group {group}: census it or exclude it with a reason")
    for group in sorted(known - set(groups)):
        problems.append(f"KisakCOD-mp no longer lists group {group}: remove it from the census")
    return problems


def list_sources(cmake, groups):
    """Return [(group, absolute path)] for the .cpp sources of ``groups``."""
    script = LIST_SCRIPT.format(src=SRC.as_posix(), scripts=SCRIPTS.as_posix(),
                                deps=(ROOT / "deps").as_posix(), groups=" ".join(groups))
    with tempfile.TemporaryDirectory(prefix="kisak-census-") as workdir:
        path = Path(workdir) / "list.cmake"
        path.write_text(script, encoding="utf-8")
        completed = subprocess.run([cmake, "-P", str(path)], capture_output=True, text=True,
                                   timeout=120, check=False)
    if completed.returncode != 0:
        raise CensusError(f"cmake -P failed ({completed.returncode}):\n{completed.stderr}")
    return parse_listing(completed.stderr + completed.stdout)


def parse_listing(text):
    sources = []
    seen = set()
    for line in text.splitlines():
        if "|" not in line:
            continue
        group, path = line.split("|", 1)
        if not path.endswith(".cpp"):
            continue
        resolved = Path(path).resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        sources.append((group, resolved))
    return sources


def relative(path):
    return Path(path).resolve().relative_to(ROOT).as_posix()


def load_blocked(path):
    """Parse ``path: reason`` lines; '#' starts a comment."""
    blocked = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if ":" not in line:
            raise CensusError(f"{path.name}:{number}: expected 'path: reason'")
        source, reason = (part.strip() for part in line.split(":", 1))
        if not reason:
            raise CensusError(f"{path.name}:{number}: {source} has no reason")
        if source in blocked:
            raise CensusError(f"{path.name}:{number}: {source} listed twice")
        blocked[source] = reason
    return blocked


def compare(results, blocked):
    """Return (regressions, stale) from {relative path: compiled?} and the list."""
    regressions = sorted(path for path, ok in results.items() if not ok and path not in blocked)
    stale = sorted(path for path in blocked if results.get(path) is not False)
    return regressions, stale


def generate_build_header(cmake, directory):
    """Write buildnumber.h into directory with the build's own generator.

    The engine build writes src/buildnumber.h at build time (it is not in git),
    and five client sources include it. A fresh checkout has none, so the census
    runs the same script into a private directory instead of touching src/.
    """
    completed = subprocess.run(
        [cmake, f"-DSRC_DIR={Path(directory).as_posix()}", "-DBUILD_NUMBER=0", "-P", str(BUILD_HEADER_SCRIPT)],
        capture_output=True, text=True, check=False)
    header = Path(directory) / "buildnumber.h"
    if completed.returncode != 0 or not header.is_file():
        raise CensusError(f"{BUILD_HEADER_SCRIPT.name} did not write buildnumber.h "
                          f"({completed.returncode}):\n{completed.stderr}")
    return header


def compile_one(cxx, source, timeout, generated_dir=None):
    # The generated directory comes first, so <buildnumber.h> never resolves to a
    # stale header a local build left in src/.
    generated = ["-I", str(generated_dir)] if generated_dir else []
    argv = [cxx, *CLIENT_DEFINES, *generated, "-I", str(SRC), "-I", str(ROOT / "deps"), *CLIENT_FLAGS,
            str(source)]
    process = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        return "timeout", argv, output
    return ("ok" if process.returncode == 0 else "error"), argv, output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="seconds per source")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--blocked", type=Path, default=BLOCKED_LIST)
    parser.add_argument("--list", action="store_true", help="print the sources and exit")
    options = parser.parse_args(argv)
    if options.jobs < 1:
        parser.error("--jobs must be at least 1")

    try:
        groups = target_groups(MP_TARGET.read_text(encoding="utf-8"))
        problems = check_group_coverage(groups)
        blocked = load_blocked(options.blocked)
        if shutil.which(options.cmake) is None:
            raise CensusError(f"{options.cmake} not found")
        sources = list_sources(options.cmake, CENSUS_GROUPS)
    except (CensusError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: census setup: {error}")
        return 1
    if not sources:
        print("FAIL: the CMake source lists produced no .cpp files")
        return 1

    if options.list:
        for group, source in sources:
            mark = "blocked" if relative(source) in blocked else "expected"
            print(f"{group:12} {mark:8} {relative(source)}")
        print(f"{len(sources)} sources, {len(blocked)} listed as blocked")
        return 0 if not problems else 1

    if shutil.which(options.cxx) is None:
        print(f"FAIL: compiler {options.cxx} not found")
        return 1
    print(f"census: {len(sources)} client sources, {options.jobs} jobs, {options.cxx}")
    with tempfile.TemporaryDirectory(prefix="kisak-census-generated-") as generated_dir:
        try:
            generate_build_header(options.cmake, generated_dir)
        except (CensusError, OSError, subprocess.SubprocessError) as error:
            print(f"FAIL: census setup: {error}")
            return 1
        with ThreadPoolExecutor(max_workers=options.jobs) as pool:
            outcomes = list(pool.map(
                lambda item: (item, compile_one(options.cxx, item[1], options.timeout, generated_dir)), sources))

    results = {}
    timeouts = []
    for (group, source), (status, command, output) in outcomes:
        path = relative(source)
        results[path] = status == "ok"
        if status == "timeout":
            timeouts.append(path)
        if options.log_dir and status != "ok":
            options.log_dir.mkdir(parents=True, exist_ok=True)
            log = options.log_dir / (path.replace("/", "_") + ".log")
            log.write_text(" ".join(command) + "\n\n" + output, encoding="utf-8")

    regressions, stale = compare(results, blocked)
    compiled = sum(1 for ok in results.values() if ok)
    print(f"compiled {compiled}/{len(results)}; blocked list has {len(blocked)} entries")
    by_group = {}
    for (group, source), _ in outcomes:
        entry = by_group.setdefault(group, [0, 0])
        entry[0 if results[relative(source)] else 1] += 1
    for group in sorted(by_group):
        ok, failed = by_group[group]
        print(f"  {group:12} {ok:4} compile {failed:4} blocked")
    for problem in problems:
        print(f"FAIL: {problem}")
    for path in regressions:
        print(f"FAIL: regression: {path} no longer compiles natively and is not on the blocked list")
    for path in stale:
        state = "now compiles" if path in results else "is not in the client source lists"
        print(f"FAIL: stale: {path} {state}; remove it from {options.blocked.name}")
    for path in timeouts:
        print(f"FAIL: timeout: {path}")
    if problems or regressions or stale or timeouts:
        return 1
    print("PASS: client compile census matches the blocked list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
