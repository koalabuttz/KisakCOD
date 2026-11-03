#!/usr/bin/env python3
"""Asset-free checks for the native Linux multiplayer client, KisakCOD-mp.

Two parts:

* Source checks (always): the opt-in target stays off by default and i386-only,
  CINEMA is read only by the unit the target compiles without it, the platform
  units it links are real files, and the stand-ins it links say what they are.
* Run checks (with ``--binary``): start the client from temporary trees that hold
  no game data. ``--help`` and ``--version`` exit before the engine starts. With
  an empty base path the engine must refuse at the file system check. With only
  a one-line stub of fileSysCheck.cfg it must get through the file system and
  the player profile to the client's autoconfigure step, then stop there because
  configure_mp.csv (a game file) is missing. Either way it exits 1, prints each
  console line once (stderr; the fatal error goes to stdout as on the dedicated
  server), and writes nothing outside the temporary tree.

Run on SDL's dummy drivers with the stand-in D3D9 and OpenAL libraries on
LD_LIBRARY_PATH, as CI does. No GPU, Steam install, credentials or game files.

Usage:
  python3 scripts/tests/test_linux_mp_client.py [--binary build/bin/KisakCOD-mp]
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
TARGET_MODULE = ROOT / "scripts" / "platform" / "linux" / "mp_client.cmake"
PLATFORM_LIST = ROOT / "scripts" / "platform" / "linux" / "mp_client_platform_sources.txt"
UNAVAILABLE = SRC / "linux" / "client" / "mp_client_unavailable.cpp"
INPUT = SRC / "linux" / "client" / "mp_client_input.cpp"
WINDOW = SRC / "linux" / "client" / "mp_client_window.cpp"
RUN_TIMEOUT = 120


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


def source_checks(checker):
    module = TARGET_MODULE.read_text(encoding="utf-8")
    checker.require(re.search(r'option\(KISAK_BUILD_LINUX_MP_CLIENT\s+"[^"]*"\s+OFF\)', module) is not None,
                    "KISAK_BUILD_LINUX_MP_CLIENT is off by default")
    checker.require('set(KISAK_OPENAL_INCLUDE_DIR "" CACHE PATH' in module
                    and 'NOT EXISTS "${KISAK_OPENAL_INCLUDE_DIR}/AL/al.h"' in module,
                    "the optional OpenAL header directory is checked for AL/al.h")
    fence = (ROOT / "scripts" / "platform" / "linux" / "platform.cmake").read_text(encoding="utf-8")
    checker.require("KISAK_BUILD_LINUX_MP_CLIENT)" in fence, "the 64-bit configuration refuses the client target")

    definitions = re.search(r"set\(KISAK_MP_CLIENT_DEFINITIONS(.*?)\)", module, re.S)
    checker.require(definitions is not None and "CINEMA" not in definitions.group(1).split(),
                    "the client compiles without CINEMA")
    readers = []
    for path in SRC.rglob("*"):
        if path.suffix in (".cpp", ".h") and path.is_file():
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"^\s*#\s*if(def|ndef)?\b.*\bCINEMA\b", text, re.M):
                readers.append(path.relative_to(ROOT).as_posix())
    checker.require(readers == ["src/gfx_d3d/r_cinematic.cpp"],
                    "CINEMA is read only by r_cinematic.cpp, whose no-Bink branch the client builds",
                    f"preprocessor readers: {readers}")

    entries = []
    for line in PLATFORM_LIST.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            entries.append(line)
    checker.require(entries and all((ROOT / entry).is_file() and entry.startswith("src/win32/") for entry in entries),
                    "every Windows platform unit the client links exists under src/win32", str(entries))

    unavailable = UNAVAILABLE.read_text(encoding="utf-8")
    checker.require("int Sys_IsRemoteDebugClient()\n{\n    return 0;" in unavailable,
                    "no remote script debugger is ever attached")
    checker.require("bool DL_InProgress()\n{\n    return false;" in unavailable,
                    "no HTTP download is ever in progress")
    checker.require("bool Voice_Init()\n{\n    return false;" in unavailable, "voice never initializes")
    checker.require("Steam" not in unavailable and "CL_CDKeyValidate" not in unavailable,
                    "the stand-ins do not touch authentication")

    # The window procedure and input replaced the input stand-ins.
    checker.require('"${SRC_DIR}/linux/client/mp_client_input.cpp"' in module
                    and '"${SRC_DIR}/linux/client/mp_client_window.cpp"' in module,
                    "the client links its window procedure and input")
    checker.require(not re.search(r"\bIN_[A-Za-z]+\s*\(", unavailable),
                    "no input function is left among the stand-ins")
    window_code = INPUT.read_text(encoding="utf-8") + WINDOW.read_text(encoding="utf-8")
    checker.require('Cbuf_AddText(0, "quit\\n");' in window_code and "Com_Quit_f()" not in window_code,
                    "closing the window queues quit instead of shutting down inside the event pump")
    checker.require("Steam" not in window_code and "CL_CDKeyValidate" not in window_code
                    and "connectionState" not in window_code,
                    "the window and input code do not touch authentication or the connection")


def run(binary, arguments, cwd, env):
    return subprocess.run([str(binary), *arguments], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, errors="replace", timeout=RUN_TIMEOUT, check=False)


def tree_files(root):
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def run_checks(checker, binary):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")

    with tempfile.TemporaryDirectory(prefix="kisak-mp-client-") as workdir:
        work = Path(workdir)
        cwd = work / "cwd"
        cwd.mkdir()

        completed = run(binary, ["--help"], cwd, env)
        checker.require(completed.returncode == 0 and "KisakCOD multiplayer client (native Linux)" in completed.stdout
                        and "Usage: KisakCOD-mp" in completed.stdout, "--help prints the client's usage and exits 0",
                        completed.stdout + completed.stderr)
        completed = run(binary, ["--version"], cwd, env)
        checker.require(completed.returncode == 0
                        and completed.stdout == "KisakCOD multiplayer client (native Linux)\n",
                        "--version prints the client banner and exits 0", completed.stdout + completed.stderr)
        checker.require(not any(cwd.iterdir()), "the early exits write nothing")

        # The renderer's windows are SDL3 windows, so another DXVK window system
        # is refused before the engine starts.
        wsi = work / "wsi"
        (wsi / "base").mkdir(parents=True)
        completed = run(binary, ["+set", "fs_basepath", str(wsi / "base"), "+set", "fs_homepath", str(wsi / "home")],
                        cwd, dict(env, DXVK_WSI_DRIVER="Win32"))
        log = completed.stdout + completed.stderr
        checker.require(completed.returncode == 1 and "DXVK_WSI_DRIVER is 'Win32'" in log
                        and "fileSysCheck.cfg" not in log,
                        "another DXVK window system is refused before the engine starts", log[-2000:])
        checker.require(not (wsi / "home").exists() and not any(cwd.iterdir()),
                        "the refusal writes nothing")

        # An empty base path: the engine starts and refuses at the file system check.
        empty = work / "empty"
        (empty / "base").mkdir(parents=True)
        completed = run(binary, ["+set", "fs_basepath", str(empty / "base"), "+set", "fs_homepath",
                                 str(empty / "home")], cwd, env)
        log = completed.stdout + completed.stderr
        checker.require(completed.returncode == 1, "an empty base path exits 1", log[-2000:])
        checker.require(re.search(r"^FATAL ERROR: Error during initialization:\nCouldn't load fileSysCheck\.cfg", log, re.M)
                        is not None, "the empty base path is refused at the file system check", log[-2000:])

        # A stub fileSysCheck.cfg: through the file system and the profile to the
        # client's autoconfigure step, which needs configure_mp.csv.
        stub = work / "stub"
        (stub / "base" / "main").mkdir(parents=True)
        (stub / "base" / "main" / "fileSysCheck.cfg").write_text("// stub written by the check; no game data\n")
        completed = run(binary, ["+set", "fs_basepath", str(stub / "base"), "+set", "fs_homepath",
                                 str(stub / "home")], cwd, env)
        err = completed.stderr
        log = completed.stdout + err
        checker.require(completed.returncode == 1, "the stub tree exits 1", log[-2000:])
        checker.require("----- FS_Startup -----" in err and "========= autoconfigure" in err,
                        "the client reaches its autoconfigure step", err[-2000:])
        checker.require(re.search(r"^FATAL ERROR: Error during initialization:\nEXE_ERR_NOT_FOUND$", log, re.M)
                        is not None, "the missing configure_mp.csv stops it", log[-2000:])
        checker.require(log.count("----- FS_Startup -----") == 1 and log.count("========= autoconfigure") == 1,
                        "each console line is printed once", completed.stdout[-2000:])
        checker.require("Client authentication unavailable" not in err and "localhost" not in err,
                        "nothing tries to connect")

        written = [entry for entry in tree_files(stub) if entry != "base/main/fileSysCheck.cfg"]
        checker.require(all(entry.startswith("home/") for entry in written),
                        "the run writes only under fs_homepath", str(written))
        checker.require(not any(cwd.iterdir()), "nothing lands in the working directory",
                        str(tree_files(cwd)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--binary", type=Path, help="a built KisakCOD-mp to run (source checks only without it)")
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
            print(f"FAIL: {error.cmd[0]} did not exit within {RUN_TIMEOUT}s")
            return 1
    print(f"{checker.count} checks, {checker.failures} failures")
    if checker.failures:
        return 1
    print("PASS: native Linux multiplayer client checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
