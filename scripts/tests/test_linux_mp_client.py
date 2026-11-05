#!/usr/bin/env python3
"""Asset-free checks for the native Linux multiplayer client, KisakCOD-mp.

Two parts:

* Source checks (always): the opt-in target stays off by default (i386, or
  x86_64 only through KISAK_LINUX_ARCH=x86_64),
  CINEMA is read only by the unit the target compiles without it, the platform
  units it links are real files, and the stand-ins it links say what they are.
* Run checks (with ``--binary``): start the client from temporary trees that hold
  no game data. ``--help`` and ``--version`` exit before the engine starts. With
  an empty base path the engine must refuse at the file system check. With only
  a one-line stub of fileSysCheck.cfg it must get through the file system and
  the player profile to the client's autoconfigure step, then stop there because
  configure_mp.csv (a game file) is missing. Either way it exits 1, prints each
  console line once (stderr; the fatal error goes to stdout as on the dedicated
  server), and writes nothing outside the temporary tree. Without fs_homepath
  the client writes to $XDG_DATA_HOME/KisakCOD (or ~/.local/share/KisakCOD), never
  to the game folder, and refuses to start when it has neither.

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
MAIN = SRC / "linux" / "main.cpp"
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
    d3d9_module = (ROOT / "scripts" / "platform" / "linux" / "native_d3d9.cmake").read_text(encoding="utf-8")
    checker.require("KISAK_BUILD_LINUX_MP_CLIENT" not in fence
                    and 'if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT KISAK_LINUX_ARCH STREQUAL "x86_64")' in d3d9_module,
                    "a 64-bit toolchain builds the client and its D3D9 boundary only under KISAK_LINUX_ARCH=x86_64")
    checker.require("KISAK_BUILD_WORLD_DRAW_PROBE)" in fence, "the 64-bit configuration still refuses the renderer probes")

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

    # Quitting frees the key bindings (script strings) before SL_Shutdown's leak
    # check, as win_main.cpp's Sys_Quit does.
    main_code = MAIN.read_text(encoding="utf-8")
    quit_body = main_code[main_code.index("void Sys_Quit()"):]
    quit_body = quit_body[:quit_body.index("\n}\n")]
    checker.require(re.search(r"#ifdef KISAK_LINUX_GAME_CLIENT\s+(//[^\n]*\s+)*IN_Shutdown\(\);\s+Key_Shutdown\(\);\s+#endif",
                              quit_body) is not None
                    and quit_body.index("Key_Shutdown();") < quit_body.index("SL_Shutdown();"),
                    "the client's quit frees the key bindings before the script string leak check")


def run(binary, arguments, cwd, env):
    return subprocess.run([str(binary), *arguments], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, errors="replace", timeout=RUN_TIMEOUT, check=False)


STUB_TEXT = "// stub written by the check; no game data\n"


def write_stub_files(root, files):
    """Writes each relative path under root with the given text (one-line stubs, no game data)."""
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def make_stub_tree(base):
    """A base path holding only a one-line stub of fileSysCheck.cfg."""
    write_stub_files(base, {"main/fileSysCheck.cfg": STUB_TEXT})


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
        # The client's default home folder, which no run that passes
        # fs_homepath may touch.
        unused_home = work / "xdg-unused"
        env["XDG_DATA_HOME"] = str(unused_home)

        completed = run(binary, ["--help"], cwd, env)
        checker.require(completed.returncode == 0 and "KisakCOD multiplayer client (native Linux)" in completed.stdout
                        and "Usage: KisakCOD-mp" in completed.stdout, "--help prints the client's usage and exits 0",
                        completed.stdout + completed.stderr)
        checker.require("$XDG_DATA_HOME/KisakCOD" in completed.stdout and "not the game folder" in completed.stdout,
                        "--help says where configs and profiles go", completed.stdout)
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
        make_stub_tree(stub / "base")
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
        checker.require(not unused_home.exists(), "a run given fs_homepath leaves the default home folder alone")

        # Player profiles: the client writes them under fs_homepath/players, so
        # it reads them from there before the game folder's players/.
        profiles = work / "profiles"
        make_stub_tree(profiles / "base")
        write_stub_files(profiles, {
            "base/players/profiles/active.txt": "GameFolder\n",
            "base/players/profiles/GameFolder/config_mp.cfg": STUB_TEXT,
            "home/players/profiles/active.txt": "HomeFolder\n",
            "home/players/profiles/HomeFolder/config_mp.cfg": STUB_TEXT,
        })
        before = {entry: (profiles / "base" / entry).read_bytes() for entry in tree_files(profiles / "base")}
        completed = run(binary, ["+set", "fs_basepath", str(profiles / "base"), "+set", "fs_homepath",
                                 str(profiles / "home")], cwd, env)
        err = completed.stderr
        search = err[err.find("Current search path:"):]
        home_players, base_players = str(profiles / "home" / "players"), str(profiles / "base" / "players")
        checker.require(home_players in search and base_players in search
                        and search.index(home_players) < search.index(base_players),
                        "fs_homepath/players comes before the game folder's players/ in the search path",
                        search[:2000])
        checker.require("profiles/HomeFolder/config_mp.cfg" in err and "profiles/GameFolder" not in err,
                        "the home folder's active profile is the one used", err[-3000:])
        checker.require({entry: (profiles / "base" / entry).read_bytes() for entry in tree_files(profiles / "base")}
                        == before, "the game folder's players/ is left as it was")

        # Without fs_homepath the client keeps out of the game folder: it writes
        # to $XDG_DATA_HOME/KisakCOD, or ~/.local/share/KisakCOD without one.
        for index, (name, extra, home_dir) in enumerate((
                ("XDG_DATA_HOME", {"XDG_DATA_HOME": "xdg", "HOME": "user"}, "xdg/KisakCOD"),
                ("HOME", {"XDG_DATA_HOME": None, "HOME": "user"}, "user/.local/share/KisakCOD"),
                ("HOME when XDG_DATA_HOME is relative", {"XDG_DATA_HOME": "relative/xdg", "HOME": "user"},
                 "user/.local/share/KisakCOD"))):
            tree = work / f"default-home-{index}"
            make_stub_tree(tree / "base")
            run_env = dict(env)
            for key, value in extra.items():
                if value is None:
                    run_env.pop(key, None)
                elif value.startswith("relative/"):
                    run_env[key] = value
                else:
                    run_env[key] = str(tree / value)
            completed = run(binary, ["+set", "fs_basepath", str(tree / "base")], cwd, run_env)
            log = completed.stdout + completed.stderr
            checker.require(completed.returncode == 1 and "========= autoconfigure" in completed.stderr,
                            f"without fs_homepath ({name}) the stub tree still reaches autoconfigure", log[-2000:])
            written = [entry for entry in tree_files(tree) if entry != "base/main/fileSysCheck.cfg"]
            checker.require(written and all(entry.startswith(home_dir + "/") for entry in written),
                            f"without fs_homepath ({name}) the run writes only under {home_dir}", str(written))

        # No usable default and no fs_homepath: refused before the engine starts.
        refused = work / "no-home"
        (refused / "base" / "main").mkdir(parents=True)
        run_env = {key: value for key, value in env.items() if key not in ("XDG_DATA_HOME", "HOME")}
        completed = run(binary, ["+set", "fs_basepath", str(refused / "base")], cwd, run_env)
        log = completed.stdout + completed.stderr
        checker.require(completed.returncode == 1 and "pass +set fs_homepath <folder>" in completed.stderr
                        and "FS_Startup" not in log,
                        "with no home folder the client refuses before the engine starts", log[-2000:])
        checker.require(tree_files(refused) == [] and not any(cwd.iterdir()), "the refusal writes nothing",
                        str(tree_files(refused)))


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
