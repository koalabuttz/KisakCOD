#!/usr/bin/env python3
"""Check which addresses the client hands to the desktop.

On Windows ``Sys_OpenURL`` is ``ShellExecute`` "open", which runs whatever it is
given.  A game server chooses the address when it redirects a download
(``CL_ParseWWWDownload``) and can set the dvar ``CL_GetAutoUpdate`` opens, so
retail let a server start a program on the player's machine.
``src/qcommon/url_check.h`` holds the header-only rule that now stands in
between:

  ``Com_IsWebUrl``       only ``http://`` or ``https://``, a host, and bytes a
                         URL can hold are opened.
  ``Com_CopyPrintable``  the refused address is printed with every byte that is
                         not printable ASCII replaced.

Two things are checked, with no game data and no network:

1. The real header is compiled into a small native program, 64-bit and - when
   the toolchain can - 32-bit, with the address and undefined-behaviour
   sanitizers when available.  Every address is tested from a heap block of
   exactly its size, so a read past its end fails the run.
2. The production ``Sys_OpenURL`` in ``src/win32/win_main.cpp`` applies the rule
   before it calls ``ShellExecuteA``, and no other ``ShellExecuteA`` call in the
   sources opens anything but a fixed string.

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
HEADER = SRC / "qcommon" / "url_check.h"
WIN_MAIN = SRC / "win32" / "win_main.cpp"

PROGRAM = r'''
#include "url_check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;

// The address is tested from a heap block of exactly its length plus the
// terminator, so the sanitizer sees any read past it.
static bool IsWeb(const std::string& text)
{
    char* exact = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(exact, text.c_str(), text.size() + 1);
    const bool web = Com_IsWebUrl(exact);
    std::free(exact);
    return web;
}

static void Expect(const std::string& text, bool want, const char* what)
{
    if (IsWeb(text) != want)
    {
        ++failures;
        std::printf("FAIL: %s: %s\n", what, want ? "was refused" : "was accepted");
    }
}

int main()
{
    // What a server that redirects a download really sends.
    Expect("http://example.com/mods/pack.zip", true, "a plain http address");
    Expect("https://example.com/", true, "a plain https address");
    Expect("https://example.com", true, "an address with no path");
    Expect("HTTP://EXAMPLE.COM/A", true, "an upper-case scheme");
    Expect("hTtPs://example.com/a?b=c&d=e#f", true, "a query and a fragment");
    Expect("http://192.168.1.10:8080/cod4/file.iwd", true, "a numeric host with a port");
    Expect("http://[2001:db8::1]/x", true, "an IPv6 host");
    Expect("https://example.com/a%20b/(c)!$'*+,;=~_-.@", true, "every byte a URL may hold");

    // What ShellExecute would run, open from a share or pass to another program.
    Expect("", false, "an empty address");
    Expect("h", false, "one letter");
    Expect("http", false, "a scheme alone");
    Expect("http:", false, "a scheme and a colon");
    Expect("http:/", false, "a scheme and one slash");
    Expect("http://", false, "a scheme with no host");
    Expect("https://", false, "an https scheme with no host");
    Expect("http:///etc/passwd", false, "an empty host");
    Expect("http://@example.com", false, "a host that starts with @");
    Expect("http://:80/", false, "a host that starts with a port");
    Expect("http://-example.com", false, "a host that starts with a dash");
    Expect("httpx://example.com", false, "a scheme that only starts with http");
    Expect("httpss://example.com", false, "https with one s too many");
    Expect(" http://example.com", false, "a leading space");
    Expect("calc.exe", false, "a program name");
    Expect("C:\\Windows\\System32\\calc.exe", false, "a program path");
    Expect("\\\\198.51.100.7\\share\\run.exe", false, "a file on a network share");
    Expect("file:///C:/Windows/System32/calc.exe", false, "a file address");
    Expect("ftp://example.com/x", false, "an ftp address");
    Expect("ms-msdt:/id PCWDiagnostic", false, "another program's protocol");
    Expect("search-ms:query=x&crumb=location:\\\\198.51.100.7\\share", false, "the search protocol");
    Expect("javascript:alert(1)", false, "a script address");
    Expect("mailto:someone@example.com", false, "a mail address");
    Expect("steam://run/7940", false, "a Steam address");
    Expect("shell:startup", false, "a shell folder");
    Expect("..\\..\\x.bat", false, "a relative path");
    Expect("http://example.com/a b", false, "a space");
    Expect("http://example.com/\" --renderer-cmd-prefix=\"calc", false, "a double quote");
    Expect("http://example.com\\@198.51.100.7", false, "a backslash");
    Expect("http://example.com/<x>", false, "angle brackets");
    Expect("http://example.com/a|b", false, "a pipe");
    Expect("http://example.com/^1red", false, "a caret");
    Expect("http://example.com/`x`", false, "a backquote");
    Expect("http://example.com/{x}", false, "braces");
    Expect("http://example.com/\tx", false, "a tab");
    Expect("http://example.com/\r\nx", false, "a line break");
    Expect("http://example.com/\x14x", false, "a control byte");
    Expect("http://example.com/\x7fx", false, "the delete byte");
    Expect("http://example.com/\xc3\xa9", false, "an 8-bit byte");
    Expect("http://\xe2\x80\xaemoc.elpmaxe/", false, "an 8-bit byte in the host");

    if (Com_IsWebUrl(nullptr))
    {
        ++failures;
        std::printf("FAIL: no address at all was accepted\n");
    }

    // Length: the longest address accepted is OPEN_URL_MAX_LENGTH bytes.
    {
        const std::string head = "http://example.com/";
        Expect(head + std::string(OPEN_URL_MAX_LENGTH - head.size(), 'a'), true, "the longest address");
        Expect(head + std::string(OPEN_URL_MAX_LENGTH - head.size() + 1, 'a'), false, "one byte longer than that");
        Expect(head + std::string(65536, 'a'), false, "a very long address");
    }

    // Every single byte: put after a good start, only URL bytes keep it a web address.
    for (int byte = 1; byte < 256; ++byte)
    {
        const char c = static_cast<char>(byte);
        const bool isUrlByte = std::strchr(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~:/?#[]@!$&'()*+,;=%", c) != nullptr;
        if (Com_IsUrlByte(c) != isUrlByte)
        {
            ++failures;
            std::printf("FAIL: byte %d is %s a URL byte\n", byte, isUrlByte ? "not counted as" : "counted as");
        }
        if (IsWeb(std::string("http://example.com/") + c) != isUrlByte)
        {
            ++failures;
            std::printf("FAIL: an address ending in byte %d was %s\n", byte, isUrlByte ? "refused" : "accepted");
        }
    }
    if (Com_IsUrlByte('\0'))
    {
        ++failures;
        std::printf("FAIL: the terminator is counted as a URL byte\n");
    }

    // The copy that is printed: cut to fit, always terminated, nothing but printable ASCII.
    {
        char* out = static_cast<char*>(std::malloc(8));
        std::memset(out, 'x', 8);
        Com_CopyPrintable("ab\x14\x15\r\n\x7f\xe9yz", out, 8);
        if (std::strcmp(out, "ab?????") != 0)
        {
            ++failures;
            std::printf("FAIL: the printable copy is \"%s\"\n", out);
        }
        std::memset(out, 'x', 8);
        Com_CopyPrintable("calc.exe", out, 8);
        if (std::strcmp(out, "calc.ex") != 0)
        {
            ++failures;
            std::printf("FAIL: the printable copy is not cut to fit: \"%s\"\n", out);
        }
        std::memset(out, 'x', 8);
        Com_CopyPrintable(nullptr, out, 8);
        if (out[0] != 0)
        {
            ++failures;
            std::printf("FAIL: the printable copy of no address is not empty\n");
        }
        std::memset(out, 'x', 8);
        Com_CopyPrintable("abc", out, 1);
        if (out[0] != 0 || out[1] != 'x')
        {
            ++failures;
            std::printf("FAIL: a one-byte copy wrote past its byte\n");
        }
        Com_CopyPrintable("abc", out, 0);
        Com_CopyPrintable("abc", nullptr, 8);
        std::free(out);
    }

    if (failures)
    {
        std::printf("%d open-URL rule check(s) failed\n", failures);
        return 1;
    }
    std::printf("open-URL rule checks passed\n");
    return 0;
}
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
    """`text` without its comments.  String and character literals are kept whole,
    so the "//" of an address in one is not taken for a comment."""
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


def check_sys_open_url(win_main: Path = WIN_MAIN) -> bool:
    """The Windows Sys_OpenURL refuses anything but a web address before it opens it."""
    body = function_body(strip_comments(win_main.read_text(encoding="utf-8", errors="replace")),
                         "void Sys_OpenURL(const char *url, int doexit)")
    if body is None:
        print(f"FAIL: {win_main.name} no longer defines Sys_OpenURL the way this check reads it")
        return False
    guard = re.search(r"if\s*\(\s*!\s*Com_IsWebUrl\s*\(\s*url\s*\)\s*\)", body)
    opened = body.find("ShellExecuteA")
    if opened < 0:
        print("FAIL: Sys_OpenURL no longer calls ShellExecuteA; update this check for what it calls now")
        return False
    if not guard or guard.start() > opened:
        print("FAIL: Sys_OpenURL calls ShellExecuteA without first refusing what Com_IsWebUrl refuses")
        return False
    refused = body[guard.end():opened]
    if not re.search(r"\breturn\s*;", refused):
        print("FAIL: Sys_OpenURL does not return after refusing an address")
        return False
    print("PASS: Sys_OpenURL refuses anything but a web address before ShellExecuteA")
    return True


def check_other_shell_calls(src: Path = SRC) -> bool:
    """Every other ShellExecute call opens a fixed string."""
    ok = True
    seen = 0
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".cpp", ".c", ".h", ".hpp", ".inl", ".mm"):
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in re.finditer(r"\bShellExecute(?:Ex)?[AW]?\s*\(([^;{]*)\)\s*([;{]?)", text):
            arguments, tail = match.group(1), match.group(2)
            # A declaration or a stand-in's definition names its parameter types.
            if re.search(r"\b(HWND|LPCSTR|LPCWSTR)\b", arguments):
                continue
            seen += 1
            if path.resolve() == WIN_MAIN.resolve():
                continue  # checked above
            parts = [part.strip() for part in arguments.split(",")]
            if tail != ";" or len(parts) < 3 or not re.fullmatch(r'"(?:[^"\\]|\\.)*"', parts[2]):
                print(f"FAIL: src/{path.relative_to(src).as_posix()} calls ShellExecute with something other than a fixed string: "
                      f"{' '.join(match.group(0).split())}")
                ok = False
    if seen == 0:
        print("FAIL: found no ShellExecute call at all; this check no longer reads the sources")
        return False
    if ok:
        print("PASS: every other ShellExecute call opens a fixed string")
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

    for needed in (HEADER, WIN_MAIN):
        if not needed.is_file():
            print(f"FAIL: {needed} is missing", file=sys.stderr)
            return 1

    ok = check_sys_open_url()
    ok = check_other_shell_calls() and ok

    with tempfile.TemporaryDirectory(prefix="kisak-open-url-") as temp:
        workdir = Path(temp)
        source = workdir / "open_url_check.cpp"
        source.write_text(PROGRAM)

        sanitize = [] if args.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if sanitize and not toolchain_supports(args.cxx, sanitize, workdir):
            print("NOTE: address/undefined sanitizers are unavailable for this toolchain; building without them")
            sanitize = []

        widths = [("64", list(sanitize))]
        if toolchain_supports(args.cxx, sanitize + ["-m32"], workdir):
            widths.append(("32", sanitize + ["-m32"]))
        elif toolchain_supports(args.cxx, ["-m32"], workdir):
            print("NOTE: no 32-bit sanitizer runtime; the 32-bit configuration is built without sanitizers")
            widths.append(("32", ["-m32"]))
        elif args.m32:
            print("FAIL: --m32 requires a 32-bit build and this toolchain cannot produce one", file=sys.stderr)
            return 1
        else:
            print("NOTE: this toolchain cannot build 32-bit; only the 64-bit configuration runs")

        for width, flags in widths:
            executable = workdir / f"open_url_check{width}"
            # Both char signs: the rule must not depend on which one the platform has.
            for sign in ("-fsigned-char", "-funsigned-char"):
                command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", sign, *flags,
                           f"-I{HEADER.parent}", str(source), "-o", str(executable)]
                print("+", " ".join(shlex.quote(part) for part in command))
                subprocess.run(command, check=True)
                print("+", shlex.quote(str(executable)))
                if subprocess.run([str(executable)]).returncode != 0:
                    print(f"FAIL: {width}-bit open-URL rule checks failed ({sign})", file=sys.stderr)
                    ok = False
                else:
                    print(f"PASS: {width}-bit open-URL rule checks ({sign})")

    print("PASS: open URL" if ok else "FAIL: open URL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
