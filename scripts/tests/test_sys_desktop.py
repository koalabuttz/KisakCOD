#!/usr/bin/env python3
"""Check the desktop rules of the Linux, macOS and Vita builds.

``src/linux/sys_desktop.h`` holds what stands in for Windows' ``MessageBoxA``,
``ShellExecuteA`` and ``CreateProcessA`` (win_main.cpp, win_syscon.cpp):

  ``Sys_DialogRefusal``        when a message box may be shown: never with
                               ``KISAK_NO_DIALOGS`` set, on SDL's dummy or
                               offscreen video driver, or off the main thread,
                               so a scripted run never waits for a click.
  ``Sys_IsSiblingProgramName`` ``Sys_QuitAndStartProcess`` starts only a program
                               named without a folder, beside the running one
                               (``startSingleplayer``, ``startMultiplayer``),
                               never the auto-update file a server sent.
  ``Sys_SplitParameters``      its parameters become arguments as the engine
                               reads a command line: spaces split, double
                               quotes group.
  ``Sys_StartProgram``         the program starts with ``/dev/null`` for input
                               and none of this process's other open files.

Checked with no game data, no display and no network:

1. The real header is compiled into a small native program, 64-bit and - when
   the toolchain can - 32-bit, with the address and undefined-behaviour
   sanitizers when available. It starts a stand-in program and reads back
   what that program was given.
2. The production code uses the rules: ``Sys_QuitAndStartProcess`` refuses a
   name with a folder before anything else, ``Sys_ShowDialog`` asks
   ``Sys_DialogRefusal`` before ``SDL_ShowMessageBox``, and the scripts that
   start the game for a run set ``KISAK_NO_DIALOGS``.

``--m32`` makes the 32-bit build a requirement.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
HEADER = SRC / "linux" / "sys_desktop.h"
POSIX_MAIN = SRC / "linux" / "main.cpp"
CLIENT_INPUT = SRC / "linux" / "client" / "mp_client_input.cpp"
RUN_SCRIPTS = [
    ROOT / "scripts" / "runs" / "perf_run.py",
    ROOT / "scripts" / "runs" / "menu_check.py",
    ROOT / "scripts" / "tests" / "run_linux_sp_level_sweep.py",
    ROOT / "scripts" / "tests" / "test_linux_mp_client.py",
    ROOT / "scripts" / "tests" / "test_linux_sp_game.py",
]

PROGRAM = r'''
#include "sys_desktop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>

static int failures = 0;

static void Expect(bool good, const char *what)
{
    if (!good)
    {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

static std::string Read(const char *path)
{
    std::string text;
    if (FILE *file = std::fopen(path, "rb"))
    {
        char buffer[512];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            text.append(buffer, got);
        std::fclose(file);
    }
    return text;
}

// Splits `parameters`; the arguments joined by '|', or "<-1>" when refused.
static std::string Split(const char *parameters, size_t storageSize = 256, int maxArguments = 16)
{
    char *storage = static_cast<char *>(std::malloc(storageSize));
    const char *arguments[16];
    const int count = Sys_SplitParameters(parameters, storage, storageSize, arguments, maxArguments);
    std::string joined;
    if (count < 0)
        joined = "<-1>";
    for (int i = 0; i < count; ++i)
    {
        if (i)
            joined += '|';
        joined += arguments[i];
    }
    std::free(storage);
    return joined;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::printf("usage: %s <stand-in program> <work folder>\n", argv[0]);
        return 2;
    }
    const char *const standIn = argv[1];
    const std::string work = argv[2];

    // When a box may be shown.
    Expect(Sys_DialogRefusal(nullptr, nullptr, true) == nullptr, "a box is refused with nothing set");
    Expect(Sys_DialogRefusal("", "x11", true) == nullptr, "an empty KISAK_NO_DIALOGS refuses a box");
    Expect(Sys_DialogRefusal("0", "wayland", true) == nullptr, "KISAK_NO_DIALOGS=0 refuses a box");
    Expect(Sys_DialogRefusal(nullptr, "cocoa", true) == nullptr, "the macOS driver refuses a box");
    Expect(Sys_DialogRefusal("1", "x11", true) != nullptr, "KISAK_NO_DIALOGS=1 lets a box through");
    Expect(Sys_DialogRefusal("yes", nullptr, true) != nullptr, "KISAK_NO_DIALOGS=yes lets a box through");
    Expect(Sys_DialogRefusal(nullptr, "dummy", true) != nullptr, "the dummy driver lets a box through");
    Expect(Sys_DialogRefusal(nullptr, "offscreen", true) != nullptr, "the offscreen driver lets a box through");
    Expect(Sys_DialogRefusal(nullptr, "x11", false) != nullptr, "another thread may show a box");

    // Which programs Sys_QuitAndStartProcess starts.
    Expect(Sys_IsSiblingProgramName("KisakCOD-sp"), "KisakCOD-sp is refused");
    Expect(Sys_IsSiblingProgramName("KisakCOD-mp"), "KisakCOD-mp is refused");
    Expect(!Sys_IsSiblingProgramName("updates/iw3mp_update.exe"), "the auto-update file is started");
    Expect(!Sys_IsSiblingProgramName("/bin/sh"), "an absolute path is started");
    Expect(!Sys_IsSiblingProgramName("../KisakCOD-sp"), "a parent folder's program is started");
    Expect(!Sys_IsSiblingProgramName("..\\x.exe"), "a backslash path is started");
    Expect(!Sys_IsSiblingProgramName("C:x.exe"), "a drive path is started");
    Expect(!Sys_IsSiblingProgramName(".."), "\"..\" is started");
    Expect(!Sys_IsSiblingProgramName("."), "\".\" is started");
    Expect(!Sys_IsSiblingProgramName(""), "an empty name is started");
    Expect(!Sys_IsSiblingProgramName(nullptr), "no name is started");
    Expect(!Sys_IsSiblingProgramName("a\nb"), "a name with a line break is started");

    // Parameters into arguments.
    Expect(Split(nullptr) == "", "no parameters are not no arguments");
    Expect(Split("  \t ") == "", "blank parameters are not no arguments");
    Expect(Split("+set fs_game \"mods/x\"") == "+set|fs_game|mods/x", "quotes are not removed");
    Expect(Split("+set fs_basepath \"/a b/c\"  +set  x 1") == "+set|fs_basepath|/a b/c|+set|x|1",
           "a quoted space does not stay in one argument");
    Expect(Split("a\"b c\"d") == "ab cd", "quotes inside an argument do not group");
    Expect(Split("\"\"") == "", "an empty quoted argument is not dropped as in the engine");
    Expect(Split("\"open") == "open", "an unclosed quote does not run to the end");
    Expect(Split("abc def", 8) == "abc|def", "parameters that fit exactly are refused");
    Expect(Split("abc defg", 8) == "<-1>", "storage one byte short is not refused");
    Expect(Split("a b c", 256, 2) == "<-1>", "too many arguments are not refused");
    Expect(Split("a b", 256, 2) == "a|b", "the most arguments are refused");

    // Starting a program: its arguments, its input, the files it inherits.
    const std::string out = work + "/started.txt";
    const int kept = open((work + "/open-file.txt").c_str(), O_CREAT | O_WRONLY, 0600);
    Expect(kept >= 0 && dup2(kept, 77) == 77, "the work folder's file could not be opened as fd 77");
    const char *arguments[] = { standIn, out.c_str(), "+set", "fs_basepath", "/a b/c", nullptr };
    const pid_t child = Sys_StartProgram(standIn, arguments);
    Expect(child > 0, "the stand-in program could not be started");
    int status = 0;
    if (child > 0)
    {
        Expect(waitpid(child, &status, 0) == child, "the stand-in program could not be waited for");
        Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the stand-in program failed");
        const std::string got = Read(out.c_str());
        Expect(got.find("arg=+set\narg=fs_basepath\narg=/a b/c\n") != std::string::npos,
               "the stand-in program did not get its arguments one by one");
        Expect(got.find("stdin=/dev/null\n") != std::string::npos, "the stand-in program's input is not /dev/null");
        Expect(got.find("fd77=closed\n") != std::string::npos, "the stand-in program inherited fd 77");
        if (failures)
            std::printf("the stand-in program wrote:\n%s", got.c_str());
    }
    close(77);
    if (kept >= 0)
        close(kept);

    const std::string missing = work + "/no-such-program";
    const char *missingArguments[] = { missing.c_str(), nullptr };
    const pid_t gone = Sys_StartProgram(missing.c_str(), missingArguments);
    Expect(gone > 0, "a missing program's process could not be made");
    if (gone > 0)
    {
        Expect(waitpid(gone, &status, 0) == gone && WIFEXITED(status) && WEXITSTATUS(status) == 127,
               "a missing program does not end with 127");
    }

    if (failures)
    {
        std::printf("%d desktop rule check(s) failed\n", failures);
        return 1;
    }
    std::printf("desktop rule checks passed\n");
    return 0;
}
'''

# What the stand-in program writes: each argument after the output file, what
# its input is and whether the parent's fd 77 reached it.
STAND_IN = r'''#!/bin/sh
out="$1"
shift
: > "$out"
for argument in "$@"; do
    printf 'arg=%s\n' "$argument" >> "$out"
done
printf 'stdin=%s\n' "$(readlink /proc/$$/fd/0)" >> "$out"
if [ -e /proc/$$/fd/77 ]; then
    printf 'fd77=open\n' >> "$out"
else
    printf 'fd77=closed\n' >> "$out"
fi
'''


def toolchain_supports(compiler: str, flags: list, workdir: Path) -> bool:
    probe = workdir / "probe.cpp"
    probe.write_text("int main() { return 0; }\n")
    result = subprocess.run(
        [compiler, *flags, "-std=c++20", str(probe), "-o", str(workdir / "probe")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def strip_comments(text: str) -> str:
    """`text` without its comments; string and character literals kept whole."""
    out = []
    pos = 0
    size = len(text)
    while pos < size:
        char = text[pos]
        if char in "\"'":
            end = pos + 1
            while end < size and text[end] != char and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            out.append(text[pos:end + 1])
            pos = end + 1
        elif text.startswith("//", pos):
            end = text.find("\n", pos)
            pos = size if end < 0 else end
        elif text.startswith("/*", pos):
            end = text.find("*/", pos + 2)
            out.append(" ")
            pos = size if end < 0 else end + 2
        else:
            out.append(char)
            pos += 1
    return "".join(out)


def function_body(text: str, signature: str):
    """The text between the braces of the function whose definition starts with `signature`."""
    start = text.find(signature)
    if start < 0:
        return None
    brace = text.find("{", start)
    if brace < 0 or ";" in text[start:brace]:
        return None
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:pos]
    return None


def check_order(path: Path, signature: str, first: str, then: str, what: str) -> bool:
    body = function_body(strip_comments(path.read_text(encoding="utf-8", errors="replace")), signature)
    if body is None:
        print(f"FAIL: {path.name} no longer defines {signature.split('(')[0].split()[-1]} the way this check reads it")
        return False
    first_at, then_at = body.find(first), body.find(then)
    if first_at < 0 or then_at < 0 or first_at > then_at:
        print(f"FAIL: {what}")
        return False
    name = signature.split("(")[0].split()[-1]
    print(f"PASS: {name} calls {first.split('(')[0]} before {then.split('(')[0]}")
    return True


def check_sources() -> bool:
    ok = check_order(POSIX_MAIN, "void Sys_QuitAndStartProcess(const char *exeName, const char *parameters)",
                     "Sys_IsSiblingProgramName(exeName)", "access(",
                     "Sys_QuitAndStartProcess does not refuse a name with a folder before it looks for the program")
    ok = check_order(POSIX_MAIN, "void Sys_SpawnQuitProcess()", "Sys_SplitParameters(", "Sys_StartProgram(",
                     "Sys_SpawnQuitProcess does not start the program through Sys_StartProgram") and ok
    ok = check_order(CLIENT_INPUT, "SysDialogAnswer Sys_ShowDialog(SysDialogKind kind, const char *title, const char *text)",
                     "Sys_DialogRefusal(", "SDL_ShowMessageBox(",
                     "Sys_ShowDialog does not ask Sys_DialogRefusal before SDL_ShowMessageBox") and ok
    missing = [str(path.relative_to(ROOT)) for path in RUN_SCRIPTS
               if 'setdefault("KISAK_NO_DIALOGS", "1")' not in path.read_text(encoding="utf-8")]
    if missing:
        print(f"FAIL: these scripts start the game without KISAK_NO_DIALOGS: {', '.join(missing)}")
        ok = False
    else:
        print("PASS: the scripts that start the game for a run set KISAK_NO_DIALOGS")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--m32", action="store_true",
                        help="require a 32-bit build in addition to the 64-bit one")
    parser.add_argument("--no-sanitize", action="store_true",
                        help="compile without -fsanitize=address,undefined")
    args = parser.parse_args()

    for needed in (HEADER, POSIX_MAIN, CLIENT_INPUT, *RUN_SCRIPTS):
        if not needed.is_file():
            print(f"FAIL: {needed} is missing", file=sys.stderr)
            return 1

    ok = check_sources()
    if not Path("/proc/self/fd").is_dir():
        print("FAIL: this check reads /proc to see what a started program inherits; run it on Linux")
        return 1

    with tempfile.TemporaryDirectory(prefix="kisak-sys-desktop-") as temp:
        workdir = Path(temp)
        source = workdir / "sys_desktop_check.cpp"
        source.write_text(PROGRAM)
        stand_in = workdir / "stand-in.sh"
        stand_in.write_text(STAND_IN)
        stand_in.chmod(0o755)
        sanitize = [] if args.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if sanitize and not toolchain_supports(args.cxx, sanitize, workdir):
            print("NOTE: the sanitizers are not available; building without them")
            sanitize = []
        builds = [("64-bit", ["-m64"] if toolchain_supports(args.cxx, ["-m64"], workdir) else [])]
        if toolchain_supports(args.cxx, ["-m32", *sanitize], workdir):
            builds.append(("32-bit", ["-m32"]))
        elif args.m32:
            print("FAIL: --m32 was given but this toolchain cannot build 32-bit programs")
            return 1
        else:
            print("NOTE: no 32-bit toolchain; the 32-bit build is skipped")
        for label, arch in builds:
            binary = workdir / f"sys_desktop_check_{label}"
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", *sanitize, *arch,
                       f"-I{HEADER.parent}", str(source), "-o", str(binary)]
            print("+ " + " ".join(shlex.quote(part) for part in command))
            if subprocess.run(command).returncode != 0:
                print(f"FAIL: the {label} desktop rule check did not compile")
                ok = False
                continue
            run_dir = workdir / label
            run_dir.mkdir()
            result = subprocess.run([str(binary), str(stand_in), str(run_dir)], stdin=subprocess.DEVNULL)
            if result.returncode != 0:
                print(f"FAIL: {label} desktop rule checks")
                ok = False
            else:
                print(f"PASS: {label} desktop rule checks")

    print("PASS: desktop rules" if ok else "FAIL: desktop rules")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
