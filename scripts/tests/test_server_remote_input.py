#!/usr/bin/env python3
"""Check the production validators for rcon, download names and query rate limits.

``src/server_mp/sv_remote_input.h`` holds two header-only rules that stand
between a UDP packet and the server console or file system:

  ``SV_RconPasswordAccepted``  decides whether an out-of-band ``rcon`` packet may
                               run a console command.  The replaced check read
                               the string dvar's pointer as a boolean, so an
                               unset ``rcon_password`` accepted the empty
                               password that ``rcon "" <command>`` supplies.
  ``SV_IsSafeDownloadName``    decides whether a ``download <name>`` client
                               command may reach ``FS_SV_FOpenFileRead``, which
                               prepends ``fs_homepath`` and nothing else.

``src/server_mp/sv_query_limit.h`` holds the per-address and global leaky
buckets for getstatus, getinfo, getchallenge and rcon replies, which stop the
server from being used as a UDP reflector and stop bad-password floods from
locking the admin out of rcon.

The real header is compiled into a small native program, 64-bit and - when the
toolchain can - 32-bit like the shipped server, and every rule is exercised with
the exact inputs an attacker or a retail client would send.  No game data, no
server process and no Python reimplementation of the rules are involved.
``--m32`` makes the 32-bit build a requirement.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "server_mp" / "sv_remote_input.h"
LIMIT_HEADER = ROOT / "src" / "server_mp" / "sv_query_limit.h"

PROGRAM = r'''
#include "sv_remote_input.h"
#include "sv_query_limit.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expect(bool got, bool want, const char* what)
{
    if (got != want)
    {
        ++failures;
        std::printf("FAIL: %s: got %s, want %s\n", what, got ? "true" : "false", want ? "true" : "false");
    }
}

static void expect_rcon(const char* configured, const char* supplied, bool want, const char* what)
{
    expect(SV_RconPasswordAccepted(configured, supplied), want, what);
}

static void expect_download(const char* name, bool want, const char* what)
{
    expect(SV_IsSafeDownloadName(name), want, what);
}

static int count_allowed(SvQueryLimits* limits, SvQueryKind kind, uint32_t address, uint32_t now, int tries)
{
    int allowed = 0;
    for (int i = 0; i < tries; ++i)
        allowed += SV_QueryLimitAllow(limits, kind, address, now) ? 1 : 0;
    return allowed;
}

static void expect_count(int got, int want, const char* what)
{
    if (got != want)
    {
        ++failures;
        std::printf("FAIL: %s: got %d, want %d\n", what, got, want);
    }
}

static void check_query_limits()
{
    // Static storage: the tables start zeroed like the server's.
    static SvQueryLimits limits;
    limits = SvQueryLimits{};
    limits.salt = 0x5eed1234u;
    const uint32_t a = 0x0100000au, b = 0x0200000bu;

    // getstatus: 2 at once per address, then one per 2 s.
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 1000, 5), 2, "getstatus burst per address");
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 1999, 1), 0, "getstatus before the period drains");
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 3000, 3), 1, "getstatus one more after 2 s");
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, b, 3000, 5), 2, "another address has its own bucket");
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 3000 + 60000, 5), 2, "idle address gets its full burst back");

    // Global getstatus bucket: 20 at once, then 10/s, however many addresses.
    limits = SvQueryLimits{};
    int allowed = 0;
    for (uint32_t i = 0; i < 200; ++i)
        allowed += SV_QueryLimitAllow(&limits, SV_QUERY_STATUS, 0x01000000u + i * 7919u, 50000) ? 1 : 0;
    expect_count(allowed, 20, "global getstatus burst over 200 spoofed addresses");
    allowed = 0;
    for (uint32_t i = 0; i < 200; ++i)
        allowed += SV_QueryLimitAllow(&limits, SV_QUERY_STATUS, 0x05000000u + i * 7919u, 51000) ? 1 : 0;
    expect_count(allowed, 10, "global getstatus refill is 10 per second");

    // getinfo and getchallenge: 4 at once per address.
    limits = SvQueryLimits{};
    expect_count(count_allowed(&limits, SV_QUERY_INFO, a, 7, 10), 4, "getinfo burst per address");
    expect_count(count_allowed(&limits, SV_QUERY_CHALLENGE, a, 7, 10), 4, "getchallenge burst per address");
    expect_count(count_allowed(&limits, SV_QUERY_CHALLENGE, a, 1007, 10), 1, "getchallenge one per second after");

    // rcon: 10 at once per address, then 5/s; failure replies 10 at once then 10/s globally.
    limits = SvQueryLimits{};
    expect_count(count_allowed(&limits, SV_QUERY_RCON, a, 100, 30), 10, "rcon burst per address");
    expect_count(count_allowed(&limits, SV_QUERY_RCON, b, 100, 3), 3, "rcon flood from one address leaves another alone");
    int replies = 0;
    for (int i = 0; i < 30; ++i)
        replies += SV_RconFailureReplyAllow(&limits, 100) ? 1 : 0;
    expect_count(replies, 10, "bad rcon replies share a global burst");
    expect_count(count_allowed(&limits, SV_QUERY_RCON, b, 100, 1), 1, "bad-reply limit does not block a password check");

    // Clock wrap: no lockout when Sys_Milliseconds wraps.
    limits = SvQueryLimits{};
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 0xfffff000u, 2), 2, "fill just before the wrap");
    expect_count(count_allowed(&limits, SV_QUERY_STATUS, a, 0x00001000u, 2), 2, "drained across the wrap");

    // Table pressure: more addresses than slots never refuses a fresh address
    // outright beyond its own burst.
    limits = SvQueryLimits{};
    for (uint32_t i = 0; i < 5000; ++i)
        (void)SV_QueryLimitAllow(&limits, SV_QUERY_INFO, 0x09000000u + i, 100000 + i * 30);
    expect_count(count_allowed(&limits, SV_QUERY_INFO, a, 100000 + 5000 * 30 + 5000, 1), 1, "new address after the table filled");

    // Bad kinds fail closed.
    expect(SV_QueryLimitAllow(&limits, SV_QUERY_KIND_COUNT, a, 0), false, "unknown kind refuses");
    expect(SV_QueryLimitAllow(nullptr, SV_QUERY_STATUS, a, 0), false, "null limits refuse");
}

int main()
{
    check_query_limits();

    // --- rcon -------------------------------------------------------------
    // The default: rcon_password is registered as "".  Nothing may pass.
    expect_rcon("", "", false, "unset password refuses the empty password (rcon \"\" <cmd>)");
    expect_rcon("", "anything", false, "unset password refuses a guess");
    expect_rcon(nullptr, "", false, "null configured password refuses");
    expect_rcon("secret", nullptr, false, "null supplied password refuses");
    expect_rcon("secret", "", false, "set password refuses the empty password");
    expect_rcon("secret", "secret", true, "exact match is accepted");
    expect_rcon("secret", "Secret", false, "case differs");
    expect_rcon("secret", "secre", false, "prefix of the password");
    expect_rcon("secret", "secrets", false, "password plus a suffix");
    expect_rcon("secret", "zecret", false, "first byte differs");
    expect_rcon("secret", "secreu", false, "last byte differs");
    expect_rcon("p\x7f ss", "p\x7f ss", true, "non-alphanumeric bytes compare byte for byte");

    // --- download ---------------------------------------------------------
    // What a retail client actually asks for.
    expect_download("main/zzz_mymod.iwd", true, "iwd in main");
    expect_download("mods/mymod/mod.ff", true, "mod fastfile");
    expect_download("mods/mymod/mymod.iwd", true, "mod iwd");
    expect_download("mods/My Mod-2 (final)/mod.ff", true, "space, dash, digits and parentheses in a component");
    expect_download("mods\\mymod\\mod.ff", true, "backslash separators are accepted as separators");
    expect_download("MODS/MYMOD/MOD.FF", true, "extension check is case-insensitive");
    expect_download("main/pack.IWD", true, "upper-case iwd");
    expect_download("usermaps/mp_custom/mp_custom.ff", true, "usermap fastfile");
    expect_download("usermaps/mp_custom/mp_custom_load.ff", true, "usermap load fastfile");
    // Mod folder names retail's fs_game accepts (it refuses only ".." and "::").
    expect_download("mods/[NL]promod/mod.ff", true, "brackets in a mod folder");
    expect_download("mods/rock&roll!/z_svr.iwd", true, "ampersand and bang in a mod folder");
    expect_download("mods/clan#1 ~mod~/mod.ff", true, "hash and tilde in a mod folder");
    expect_download("mods/it's@home/mod.ff", true, "apostrophe and at sign in a mod folder");
    expect_download("mods/.hidden/mod.ff", true, "leading dot that is not . or ..");

    // What the fix exists for.
    expect_download("main/server.cfg", false, "server config");
    expect_download("../../../../etc/passwd", false, "parent traversal");
    expect_download("main/../server.cfg", false, "traversal in the middle");
    expect_download("mods/..\\..\\server.cfg", false, "backslash traversal");
    expect_download("main/../../main/zzz.iwd", false, "traversal to a valid extension");
    expect_download("..", false, "bare parent");
    expect_download(".", false, "bare dot");
    expect_download("./main/zzz.iwd", false, "leading dot component");
    expect_download("/main/zzz.iwd", false, "leading slash");
    expect_download("\\main\\zzz.iwd", false, "leading backslash");
    expect_download("main//zzz.iwd", false, "empty component");
    expect_download("main/zzz.iwd/", false, "trailing separator");
    expect_download("", false, "empty name");
    expect_download(nullptr, false, "null name");
    expect_download(".iwd", false, "extension alone");
    expect_download("main/.ff", false, "extension alone as last component");
    expect_download("main/zzz.iwd.bak", false, "wrong final extension");
    expect_download("main/zzz.iwdx", false, "extension with a suffix");
    expect_download("main/zzz.ff.txt", false, "ff not final");
    expect_download("main/zzzff", false, "no dot before ff");
    expect_download("C:/cod4/main/zzz.iwd", false, "drive letter");
    expect_download("main:zzz.iwd", false, "colon");
    expect_download("main/zzz;rm.iwd", false, "semicolon");
    expect_download("main/zz\"z.iwd", false, "quote");
    expect_download("main/zzz.iwd\x01", false, "control byte");
    expect_download("main/zz\xc3\xa9.iwd", false, "non-ASCII byte");
    expect_download("main/zzz.iwd\n", false, "newline");
    expect_download("main/zzz.iwd ", false, "trailing space is not part of the extension");
    // Windows drops trailing dots and spaces from a path component, so these
    // would resolve to ".." there.
    expect_download("mods/.../zzz.iwd", false, "three dots");
    expect_download("mods/.. /zzz.iwd", false, "dot dot space");
    expect_download("mods/x./zzz.iwd", false, "component ending in a dot");
    expect_download("mods/x /zzz.iwd", false, "component ending in a space");
    expect_download("main/zzz.iwd.", false, "file name ending in a dot");
    expect_download("main/zzz\x7f.iwd", false, "DEL byte");

    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all remote-input checks passed\n");
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--m32", action="store_true",
                        help="require a 32-bit build in addition to the 64-bit one")
    parser.add_argument("--no-sanitize", action="store_true",
                        help="compile without -fsanitize=address,undefined")
    args = parser.parse_args()

    for header in (HEADER, LIMIT_HEADER):
        if not header.is_file():
            print(f"FAIL: {header} is missing", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory(prefix="kisak-remote-input-") as temp:
        workdir = Path(temp)
        source = workdir / "remote_input_check.cpp"
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
            executable = workdir / f"remote_input_check{width}"
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags,
                       f"-I{HEADER.parent}", str(source), "-o", str(executable)]
            print("+", " ".join(shlex.quote(part) for part in command))
            subprocess.run(command, check=True)
            print("+", shlex.quote(str(executable)))
            result = subprocess.run([str(executable)])
            if result.returncode != 0:
                print(f"FAIL: {width}-bit remote-input checks failed", file=sys.stderr)
                return 1
            print(f"PASS: {width}-bit remote-input checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
