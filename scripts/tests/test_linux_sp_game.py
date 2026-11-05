#!/usr/bin/env python3
"""Asset-free checks for the native Linux singleplayer game, KisakCOD-sp.

Two parts:

* Source checks (always): the opt-in target stays off by default and i386-only,
  builds from the singleplayer source lists with KISAK_SP and the singleplayer
  census list, and links the same platform layer and stand-ins as KisakCOD-mp
  with their multiplayer-only entries left out.
* Run checks (with ``--binary``): start the game from temporary trees that hold
  no game data. ``--help`` and ``--version`` exit before the engine starts. With
  an empty base path the engine must refuse at the file system check. With only
  a one-line stub of fileSysCheck.cfg it must get through the file system to the
  autoconfigure step, then stop because configure.csv (a game file) is missing.
  Either way it exits 1 and writes nothing outside the temporary tree.

Run on SDL's dummy drivers with the stand-in D3D9 and OpenAL libraries on
LD_LIBRARY_PATH, as CI does. No GPU, Steam install, credentials or game files.

Usage:
  python3 scripts/tests/test_linux_sp_game.py [--binary build/bin/KisakCOD-sp]
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
TARGET_MODULE = ROOT / "scripts" / "platform" / "linux" / "sp_game.cmake"
LINUX_CMAKE = ROOT / "scripts" / "platform" / "linux" / "CMakeLists.txt"
UNAVAILABLE = SRC / "linux" / "client" / "mp_client_unavailable.cpp"
THREADS = SRC / "linux" / "sys_threads.cpp"
MAIN = SRC / "linux" / "main.cpp"
SP_BLOCKED = ROOT / "scripts" / "tests" / "sp_compile_census_blocked.txt"
RUN_TIMEOUT = 120
STUB_TEXT = "// stub written by the check; no game data\n"


class Checker:
    def __init__(self):
        self.failures = 0
        self.count = 0

    def require(self, condition, name, detail=""):
        self.count += 1
        if condition:
            print(f"PASS: {name}")
        else:
            self.failures += 1
            print(f"FAIL: {name}")
            if detail:
                print("      " + detail.replace("\n", "\n      "))


def guarded(text, marker, guard):
    """True when marker sits inside the nearest enclosing #ifdef guard block."""
    start = text.find(marker)
    if start < 0:
        return False
    before = text[:start]
    return before.rfind(f"#ifdef {guard}") > before.rfind("#endif")


def source_checks(checker):
    module = TARGET_MODULE.read_text(encoding="utf-8")
    checker.require(re.search(r'option\(KISAK_BUILD_LINUX_SP_GAME\s+"[^"]*"\s+OFF\)', module) is not None,
                    "KisakCOD-sp is opt-in and off by default")
    checker.require("include(${SCRIPTS_DIR}/platform/linux/sp_game.cmake)" in LINUX_CMAKE.read_text(encoding="utf-8"),
                    "the Linux build reads the singleplayer module")
    checker.require('KISAK_LINUX_ARCH STREQUAL "x86_64"' in module and "i386-only" in module,
                    "the singleplayer game is refused on the x86_64 track")
    checker.require("sp/sp_files.cmake" in module and "sp_compile_census_blocked.txt" in module,
                    "it builds from the singleplayer lists and census")
    definitions = module[module.index("set(KISAK_SP_GAME_DEFINITIONS"):]
    definitions = definitions[:definitions.index(")")]
    checker.require("KISAK_SP" in definitions.split() and "KISAK_MP" not in definitions.split()
                    and "CINEMA" not in definitions.split(),
                    "it compiles with KISAK_SP, not KISAK_MP, and leaves CINEMA to the cinematic unit")
    checker.require(SP_BLOCKED.is_file(), "the singleplayer census list exists")
    checker.require('"${SRC_DIR}/linux/client/msvc_rand.cpp"' in module,
                    "it links MSVC's rand (effects assume a 15-bit rand)")
    checker.require('"${SRC_DIR}/linux/client/mp_client_unavailable.cpp"' in module,
                    "it links the client stand-ins")

    unavailable = UNAVAILABLE.read_text(encoding="utf-8")
    for marker in ("void Sys_Listen_f()", "bool DL_InProgress()", "bool Voice_Init()",
                   "KISAK_UNREACHABLE(int, DL_DownloadLoop"):
        checker.require(guarded(unavailable, marker, "KISAK_MP"),
                        f"{marker.split('(')[0].split()[-1]} stays multiplayer-only")
    checker.require(not guarded(unavailable, "int Sys_IsRemoteDebugClient()", "KISAK_MP"),
                    "singleplayer also has no remote script debugger attached")

    threads = THREADS.read_text(encoding="utf-8")
    for name in ("Sys_SpawnServerThread", "Sys_WaitStartServer", "Sys_ServerCompleted", "Sys_IsServerThread",
                 "Sys_SpawnServerDemoThread"):
        checker.require(re.search(rf"\b{name}\(", threads) is not None and guarded(threads, f" {name}(", "KISAK_SP"),
                        f"{name} is native and singleplayer-only")
    checker.require('"SERVER_DEMO"' in threads, "the singleplayer thread names are all there")

    main = MAIN.read_text(encoding="utf-8")
    checker.require("#if defined(KISAK_LINUX_MP_CLIENT) || defined(KISAK_LINUX_SP_GAME)" in main,
                    "both games take the client paths of the Linux main")
    step = main.index("WinMain's client step")
    nearest_if = main.rfind("\n#if", 0, step)
    checker.require(main.startswith("\n#if defined(KISAK_LINUX_MP_CLIENT)\n", nearest_if)
                    and main.rfind("#endif", 0, step) < nearest_if,
                    "only multiplayer reads stats at start-up, as WinMain")


def run(binary, arguments, cwd, env):
    return subprocess.run([str(binary), *arguments], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, errors="replace", timeout=RUN_TIMEOUT, check=False)


def tree_files(root):
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def run_checks(checker, binary):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")

    with tempfile.TemporaryDirectory(prefix="kisak-sp-game-") as workdir:
        work = Path(workdir)
        cwd = work / "cwd"
        cwd.mkdir()
        unused_home = work / "xdg-unused"
        env["XDG_DATA_HOME"] = str(unused_home)

        completed = run(binary, ["--help"], cwd, env)
        checker.require(completed.returncode == 0 and "KisakCOD singleplayer (native Linux)" in completed.stdout
                        and "Usage: KisakCOD-sp" in completed.stdout, "--help prints the game's usage and exits 0",
                        completed.stdout + completed.stderr)
        completed = run(binary, ["--version"], cwd, env)
        checker.require(completed.returncode == 0 and completed.stdout == "KisakCOD singleplayer (native Linux)\n",
                        "--version prints the singleplayer banner and exits 0", completed.stdout + completed.stderr)
        checker.require(not any(cwd.iterdir()), "the early exits write nothing")

        empty = work / "empty"
        (empty / "base").mkdir(parents=True)
        completed = run(binary, ["+set", "fs_basepath", str(empty / "base"), "+set", "fs_homepath",
                                 str(empty / "home")], cwd, env)
        log = completed.stdout + completed.stderr
        checker.require(completed.returncode == 1, "an empty base path exits 1", log[-2000:])
        checker.require(re.search(r"^FATAL ERROR: Error during initialization:\nCouldn't load fileSysCheck\.cfg", log, re.M)
                        is not None, "the empty base path is refused at the file system check", log[-2000:])

        stub = work / "stub"
        (stub / "base" / "main").mkdir(parents=True)
        (stub / "base" / "main" / "fileSysCheck.cfg").write_text(STUB_TEXT)
        completed = run(binary, ["+set", "fs_basepath", str(stub / "base"), "+set", "fs_homepath",
                                 str(stub / "home")], cwd, env)
        err = completed.stderr
        log = completed.stdout + err
        checker.require(completed.returncode == 1, "the stub tree exits 1", log[-2000:])
        checker.require("----- FS_Startup -----" in err and "========= autoconfigure" in err,
                        "the game reaches its autoconfigure step", err[-2000:])
        checker.require(re.search(r"^FATAL ERROR: Error during initialization:\nEXE_ERR_NOT_FOUND$", log, re.M)
                        is not None, "the missing configure.csv stops it", log[-2000:])
        checker.require(log.count("----- FS_Startup -----") == 1, "each console line is printed once",
                        completed.stdout[-2000:])
        written = [entry for entry in tree_files(stub) if entry != "base/main/fileSysCheck.cfg"]
        checker.require(all(entry.startswith("home/") for entry in written),
                        "the run writes only under fs_homepath", str(written))
        checker.require(not any(cwd.iterdir()), "nothing lands in the working directory", str(tree_files(cwd)))
        checker.require(not unused_home.exists(), "a run given fs_homepath leaves the default home folder alone")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--binary", type=Path, help="a built KisakCOD-sp to run (source checks only without it)")
    options = parser.parse_args(argv)

    checker = Checker()
    source_checks(checker)
    if options.binary:
        if not options.binary.is_file():
            print(f"FAIL: {options.binary} does not exist")
            return 1
        try:
            run_checks(checker, options.binary.resolve())
        except subprocess.TimeoutExpired as error:
            print(f"FAIL: {error.cmd[0]} did not exit within {RUN_TIMEOUT} s")
            return 1
    print(f"{checker.count} checks, {checker.failures} failures")
    if checker.failures:
        return 1
    print("PASS: native Linux singleplayer game checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
