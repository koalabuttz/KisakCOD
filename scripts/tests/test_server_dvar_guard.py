#!/usr/bin/env python3
"""Check which client settings a game server may set.

A server a player joins sets dvars on that client by name: through systeminfo
(``CL_SystemInfoChanged``), through codinfo (``CG_ParseCodInfo``) and through
the script call setClientDvar (``CG_SetClientDvarFromServer``).  All three end
in an internal set that ignores the read-only, start-up-only and cheat flags.
Retail trusted the server with that and mods depend on it, so every retail
setting stays as it was in retail.  The one exception is the list
``kServerDvarsOfThisPort`` in ``src/qcommon/server_dvar_guard.h``: every
setting this port added, which no retail server and no retail mod knows.

Three things are checked, with no game data and no network:

1. The real header is compiled into a small native program, 64-bit and - when
   the toolchain can - 32-bit, with the address and undefined-behaviour
   sanitizers when available.  Every name is tested from a heap block of
   exactly its size, so a read past its end fails the run.
2. The three places a server's name reaches a dvar all go through
   ``CL_SetDvarFromServer``, which asks the header's rule before it sets
   anything.
3. The list fits the sources.  A dvar registered in ``src`` that the retail
   sources did not have (``retail_dvar_names.txt``) must be on the list or
   exempted below with a reason.  No listed name is a retail name, and none is
   one the engine itself replicates to clients (``DVAR_SYSTEMINFO``,
   ``DVAR_CODINFO``), so no retail server and no retail mod is refused.

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
HEADER = SRC / "qcommon" / "server_dvar_guard.h"
CL_PARSE = SRC / "client_mp" / "cl_parse_mp.cpp"
CG_SERVERCMDS = SRC / "cgame_mp" / "cg_servercmds_mp.cpp"
Q_SHARED = SRC / "universal" / "q_shared.h"
RETAIL_NAMES = Path(__file__).resolve().parent / "retail_dvar_names.txt"

SOURCE_SUFFIXES = (".cpp", ".c", ".h", ".hpp", ".inl", ".mm")

# Dvars this port added that a game server may set all the same.  Each entry
# needs the reason a server has to set it; the default answer is the port's
# list in server_dvar_guard.h.
PORT_DVARS_A_SERVER_MAY_SET = {}

# What a stock server sends through systeminfo: the settings the engine flags
# DVAR_SYSTEMINFO.  The compiled program checks that each one may be set, and
# the source check below that this is still the engine's whole set.
STOCK_SYSTEMINFO = [
    "bg_fallDamageMaxHeight", "bg_fallDamageMinHeight", "com_timescale", "fs_game",
    "g_synchronousClients", "sv_FFCheckSums", "sv_FFNames", "sv_cheats", "sv_clientSideBullets",
    "sv_iwdNames", "sv_iwds", "sv_pure", "sv_referencedFFCheckSums", "sv_referencedFFNames",
    "sv_referencedIwdNames", "sv_referencedIwds", "sv_serverid", "sv_voice", "sv_voiceQuality",
    "timescale",
]

PROGRAM = r'''
#include "server_dvar_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;

static void Expect(bool condition, const char* what, const std::string& detail = "")
{
    if (!condition)
    {
        std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
        ++failures;
    }
}

// The name is tested from a heap block of exactly its length plus the
// terminator, so the sanitizer sees any read past it.
static bool MaySet(const std::string& text)
{
    char* exact = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(exact, text.c_str(), text.size() + 1);
    const bool allowed = Com_ServerMaySetDvar(exact);
    std::free(exact);
    return allowed;
}

static std::string Upper(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
    }
    return text;
}

static std::string Lower(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    }
    return text;
}

template <size_t Count>
static void CheckListed(const char* const (&list)[Count], const char* listName)
{
    Expect(Count > 0, "a list is empty", listName);
    for (const char* name : list)
    {
        const std::string text(name);
        Expect(!MaySet(text), "a listed name may be set", text);
        Expect(!MaySet(Upper(text)), "a listed name may be set in upper case", text);
        Expect(!MaySet(Lower(text)), "a listed name may be set in lower case", text);
        // Only the exact name is refused: a longer or a shorter one is another dvar.
        Expect(MaySet(text + "2"), "a longer name is refused", text + "2");
        Expect(MaySet(text + " "), "a name with a trailing space is refused", text);
        Expect(MaySet(" " + text), "a name with a leading space is refused", text);
        Expect(MaySet(text.substr(0, text.size() - 1)), "a shorter name is refused", text);
    }
}

static const char* const stockSysteminfo[] = {
@STOCK_SYSTEMINFO@
};

// Every dvar name of the retail sources (retail_dvar_names.txt).  Retail
// settings stay as they were in retail, also the ones that matter to the
// player's machine such as activeAction and cl_allowDownload: the rule lets a
// server set each of them, in any letter case.
static const char* const retailNames[] = {
@RETAIL_NAMES@
};

// Names mods and stock scripts set on players, and names like the listed ones.
static const char* const modNames[] = {
    "cg_fov", "cg_fovScale", "com_maxfps", "cl_maxpackets", "rate", "snaps", "r_fullbright",
    "ui_hud_hardcore", "ui_uav_client", "ui_xpText", "clientcmd", "hud_enable", "g_compassShowEnemies",
    "scr_game_spectatetype", "cg_drawCrosshair", "compassSize", "ammoCounterHide", "ui_mapname",
    "sv_hostname", "g_gametype", "mapname", "nextmap", "name", "password", "fs_game", "sv_serverid",
    "cg_objectiveText", "hud_drawHud", "g_scriptMainMenu", "r_backendish", "vita", "vita_", "net",
    "r_backends", "vita_layouts", "fs_gamedir", "x",
};

static void CheckRule()
{
    CheckListed(kServerDvarsOfThisPort, "kServerDvarsOfThisPort");

    Expect(sizeof(retailNames) / sizeof(retailNames[0]) > 1000, "the retail names are missing");
    for (const char* name : retailNames)
    {
        Expect(MaySet(name), "a retail setting is refused", name);
        Expect(MaySet(Upper(name)), "a retail setting is refused in upper case", name);
        Expect(MaySet(Lower(name)), "a retail setting is refused in lower case", name);
    }

    for (const char* name : stockSysteminfo)
    {
        Expect(MaySet(name), "a stock systeminfo setting is refused", name);
        Expect(MaySet(Upper(name)), "a stock systeminfo setting is refused in upper case", name);
    }
    for (const char* name : modNames)
        Expect(MaySet(name), "a name a mod may set is refused", name);

    Expect(!Com_ServerMaySetDvar(nullptr), "no name at all may be set");
    Expect(MaySet(""), "the empty name is refused");

    // Names of this port, spelled the way a server might.
    Expect(!MaySet("Vita_KeyboardAnswer"), "Vita_KeyboardAnswer may be set");
    Expect(!MaySet("VITA_ATMAP"), "VITA_ATMAP may be set");
    Expect(!MaySet("R_BACKEND"), "R_BACKEND may be set");
    Expect(!MaySet("r_vkdevice"), "r_vkdevice may be set");
    Expect(!MaySet("SV_ALLOWUNVERIFIEDGUIDS"), "SV_ALLOWUNVERIFIEDGUIDS may be set");
    Expect(!MaySet("con_Restricted"), "con_Restricted may be set");

    // Bytes around the ASCII letters and 8-bit bytes are not letters.
    Expect(MaySet("r_b`ckend"), "'`' matched 'a'");
    Expect(MaySet("r_b@ckend"), "'@' matched a letter");
    Expect(MaySet("in_gamepadDead{one"), "'{' matched a letter");
    Expect(MaySet("in_gamepadDead[one"), "'[' matched a letter");
    Expect(MaySet(std::string("r_backen") + static_cast<char>(0xE4)), "an 8-bit byte matched 'd'");
    Expect(MaySet(std::string("r_backen") + static_cast<char>(0xC4)), "an 8-bit byte matched 'D'");
}

static void CheckNamesMatch()
{
    Expect(Com_ServerDvarNamesMatch("", ""), "two empty names differ");
    Expect(Com_ServerDvarNamesMatch("abc", "ABC"), "letter case matters");
    Expect(Com_ServerDvarNamesMatch("a_1Z", "A_1z"), "letter case matters with digits");
    Expect(!Com_ServerDvarNamesMatch("abc", "abcd"), "a prefix matched");
    Expect(!Com_ServerDvarNamesMatch("abcd", "abc"), "a longer name matched");
    Expect(!Com_ServerDvarNamesMatch("abc", "abd"), "different names matched");
    Expect(!Com_ServerDvarNamesMatch("a", ""), "a name matched the empty name");
}

static void CheckSysteminfoRule()
{
    Expect(SERVER_DVAR_SYSTEMINFO_FLAG == 0x8, "the systeminfo flag is not 0x8");
    Expect(Com_SysteminfoSettingExpected(true, 0x8), "a systeminfo setting is not expected");
    Expect(Com_SysteminfoSettingExpected(true, 0x8 | 0x40 | 0x100), "a systeminfo setting with other flags is not expected");
    Expect(!Com_SysteminfoSettingExpected(true, 0), "a setting with no flags is expected");
    Expect(!Com_SysteminfoSettingExpected(true, 0xFFFF & ~0x8u), "a setting with every other flag is expected");
    Expect(!Com_SysteminfoSettingExpected(false, 0x8), "a setting the client does not have is expected");
    Expect(!Com_SysteminfoSettingExpected(false, 0), "an unknown setting is expected");
}

static std::string Shown(const std::string& name, size_t outSize)
{
    // A buffer of exactly outSize bytes, so a write past it is seen.
    char* out = static_cast<char*>(std::malloc(outSize ? outSize : 1));
    std::memset(out, 'X', outSize ? outSize : 1);
    Com_CopyServerDvarName(name.c_str(), out, outSize);
    const std::string result = outSize ? std::string(out) : std::string();
    std::free(out);
    return result;
}

static void CheckCopy()
{
    Expect(Shown("r_backend", 64) == "r_backend", "a plain name is changed");
    Expect(Shown("Az09_", 64) == "Az09_", "letters, digits and '_' are changed");
    Expect(Shown("a b", 64) == "a?b", "a space is kept");
    Expect(Shown("a^1b", 64) == "a?1b", "a colour code is kept");
    Expect(Shown("a%sb", 64) == "a?sb", "a percent sign is kept");
    Expect(Shown("a\nb\rc\x1b" "d", 64) == "a?b?c?d", "a control byte is kept");
    Expect(Shown(std::string("a") + static_cast<char>(0xFF) + "b", 64) == "a?b", "an 8-bit byte is kept");
    Expect(Shown("abcdef", 4) == "abc", "the copy is not cut to fit");
    Expect(Shown("abcdef", 1).empty(), "a one-byte buffer does not hold the empty text");
    Expect(Shown("", 8).empty(), "the empty name is changed");
    Shown("abcdef", 0);
    char out[4] = { 'k', 'e', 'p', 0 };
    Com_CopyServerDvarName(nullptr, out, sizeof(out));
    Expect(out[0] == 0, "no name does not give the empty text");
    Com_CopyServerDvarName("abc", nullptr, 4);
}

static void CheckNames()
{
    ServerDvarNames<3, 8>* names = new ServerDvarNames<3, 8>();

    Expect(!names->FirstTime(nullptr), "no name counts as new");
    Expect(names->FirstTime("alpha"), "a new name is not new");
    Expect(!names->FirstTime("alpha"), "a name is new twice");
    Expect(!names->FirstTime("ALPHA"), "a name is new again in another letter case");
    Expect(names->FirstTime("alphab"), "a longer name is not new");
    // Names are kept to seven bytes here: two that agree that far are one name.
    Expect(names->FirstTime("abcdefgh"), "a long name is not new");
    Expect(!names->FirstTime("abcdefgZZZZ"), "a long name with the same start is new");
    Expect(names->Count() == 3, "the count is wrong");
    // Full: nothing further is new, and what is held still is not.
    Expect(!names->FirstTime("beta"), "a name is new when every slot is taken");
    Expect(!names->FirstTime("alpha"), "a held name is new when every slot is taken");
    Expect(names->Count() == 3, "the count grew past the slots");
    delete names;

    ServerDvarNames<64, 64>* many = new ServerDvarNames<64, 64>();
    int fresh = 0;
    for (int round = 0; round < 3; ++round)
    {
        for (int i = 0; i < 200; ++i)
        {
            const std::string name = "name_" + std::to_string(i) + std::string(static_cast<size_t>(i), 'x');
            char* exact = static_cast<char*>(std::malloc(name.size() + 1));
            std::memcpy(exact, name.c_str(), name.size() + 1);
            fresh += many->FirstTime(exact) ? 1 : 0;
            std::free(exact);
        }
    }
    // 200 different names, shown three times: the first 64 are new once.
    Expect(fresh == 64, "the new names are not exactly the slots", std::to_string(fresh));
    Expect(many->Count() == 64, "the count does not match the slots");
    delete many;
}

int main()
{
    CheckRule();
    CheckNamesMatch();
    CheckSysteminfoRule();
    CheckCopy();
    CheckNames();
    if (failures)
    {
        std::printf("%d server dvar rule check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
'''


def toolchain_supports(cxx: str, flags, workdir: Path) -> bool:
    probe = workdir / "probe.cpp"
    probe.write_text("#include <string>\nint main() { std::string s(\"x\"); return s.size() == 1 ? 0 : 1; }\n")
    out = workdir / "probe"
    built = subprocess.run([cxx, "-std=c++20", *flags, str(probe), "-o", str(out)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if built.returncode != 0:
        return False
    return subprocess.run([str(out)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def strip_comments(text: str) -> str:
    """The text without comments; string and character literals are kept whole."""
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


def read_source(path: Path) -> str:
    return strip_comments(path.read_text(encoding="utf-8", errors="replace"))


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


def header_list(text: str, name: str):
    """The names in one of the header's lists, in the order they are written."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not match:
        return None
    return re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))


# A registration with its name written out: Dvar_RegisterBool("name", ...) and
# the helpers built on it, R_RegisterWorkerThreadDvar("name", ...).
REGISTRATION = re.compile(
    r"\b(Dvar_Register\w+|\w*Register\w*Dvar\w*)\s*\(\s*(?:\(\s*(?:const\s+)?char\s*\*\s*\)\s*)?\"([A-Za-z0-9_]+)\"")


def call_arguments(text: str, open_paren: int):
    """The top-level arguments of the call whose '(' is at `open_paren`, or None."""
    arguments = []
    depth = 0
    start = open_paren + 1
    pos = open_paren
    size = len(text)
    while pos < size:
        char = text[pos]
        if char in "\"'":
            pos += 1
            while pos < size and text[pos] != char:
                pos += 2 if text[pos] == "\\" else 1
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                arguments.append(text[start:pos].strip())
                return arguments
        elif char == "," and depth == 1:
            arguments.append(text[start:pos].strip())
            start = pos + 1
        pos += 1
    return None


def registered_dvars(src: Path = SRC):
    """Every dvar name registered in the sources: name -> [(file, function, arguments)]."""
    found = {}
    for path in sorted(src.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = read_source(path)
        for match in REGISTRATION.finditer(text):
            arguments = call_arguments(text, text.index("(", match.start(1) + len(match.group(1))))
            found.setdefault(match.group(2), []).append(
                (path.relative_to(src).as_posix(), match.group(1), arguments))
    return found


def flag_values(q_shared: Path = Q_SHARED):
    """The DVAR_ flag names and their bits, from the engine's own enum."""
    body = re.search(r"enum\s+DvarFlags\b[^{]*\{(.*?)\}\s*;", read_source(q_shared), re.S)
    values = {}
    if body:
        for name, value in re.findall(r"\b(DVAR_\w+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)", body.group(1)):
            values[name] = int(value, 0)
    return values


def flags_of(arguments, values):
    """The flags a Dvar_Register* call passes (the argument before the description), or None."""
    if not arguments or len(arguments) < 3:
        return None
    text = re.sub(r"\(\s*(?:unsigned\s+)?(?:u?int(?:16|32)(?:_t)?|__int16|DvarFlags|unsigned|int)\s*\)", "", arguments[-2])
    total = 0
    for part in text.split("|"):
        part = part.strip().strip("()").strip()
        if part in values:
            total |= values[part]
        elif re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]*", part):
            total |= int(part.rstrip("uUlL"), 0)
        else:
            return None
    return total


def read_retail_names(path: Path = RETAIL_NAMES):
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            names.append(line)
    return names


def check_header_lists(header: str) -> bool:
    """The header holds one list, the one this check reads, and the rule asks only it."""
    ok = True
    lists = sorted(set(re.findall(r"\b(\w+)\s*\[\s*\]\s*=\s*\{", header)))
    if lists != ["kServerDvarsOfThisPort"]:
        print(f"FAIL: server_dvar_guard.h holds the lists {lists}; this check reads kServerDvarsOfThisPort only, "
              "and every retail setting stays as it was in retail (docs/server-set-dvars.md)")
        ok = False
    rule = function_body(header, "inline bool Com_ServerMaySetDvar(")
    asked = [] if rule is None else re.findall(r"\bCom_ServerDvarListed\s*\(\s*(\w+)", rule)
    if asked != ["kServerDvarsOfThisPort"]:
        print(f"FAIL: Com_ServerMaySetDvar asks {asked}; it must ask kServerDvarsOfThisPort and nothing else")
        ok = False
    elif not re.fullmatch(r"\s*if\s*\(\s*!\s*name\s*\)\s*return\s+false\s*;\s*"
                          r"return\s*!\s*Com_ServerDvarListed\s*\(\s*kServerDvarsOfThisPort\s*,\s*name\s*\)\s*;\s*",
                          rule):
        print("FAIL: Com_ServerMaySetDvar is no longer 'no name: false; otherwise: not on the port's list'; "
              "a retail setting must not be refused by anything else")
        ok = False
    if ok:
        print("PASS: the rule refuses the names on kServerDvarsOfThisPort and nothing else")
    return ok


def check_lists(port, registered, retail, values, exempt=None) -> bool:
    """The list fits the sources."""
    exempt = PORT_DVARS_A_SERVER_MAY_SET if exempt is None else exempt
    ok = True
    retail_lower = {name.lower() for name in retail}
    listed = set()
    for name in port:
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            print(f"FAIL: kServerDvarsOfThisPort holds {name!r}, which is not a dvar name")
            ok = False
        if name.lower() in listed:
            print(f"FAIL: {name} is listed twice in kServerDvarsOfThisPort")
            ok = False
        listed.add(name.lower())

    for name in port:
        if name.lower() in retail_lower:
            print(f"FAIL: {name} is in kServerDvarsOfThisPort but the retail sources have it; every retail "
                  "setting stays as it was in retail, where a server may set it (docs/server-set-dvars.md)")
            ok = False
    for name in exempt:
        if name.lower() in listed:
            print(f"FAIL: {name} is both listed in the header and exempted in this check")
            ok = False

    exempt_lower = {name.lower() for name in exempt}
    missing = sorted((name for name in registered
                      if name.lower() not in retail_lower and name.lower() not in listed
                      and name.lower() not in exempt_lower), key=str.lower)
    for name in missing:
        where = registered[name][0][0]
        print(f"FAIL: src/{where} registers the dvar {name}, which this port adds and a game server could set. "
              "Add it to kServerDvarsOfThisPort in src/qcommon/server_dvar_guard.h, or, if servers must be able "
              "to set it, to PORT_DVARS_A_SERVER_MAY_SET in scripts/tests/test_server_dvar_guard.py with the reason")
        ok = False

    replicated = values.get("DVAR_SYSTEMINFO", 0) | values.get("DVAR_CODINFO", 0)
    if not values.get("DVAR_SYSTEMINFO") or not values.get("DVAR_CODINFO"):
        print("FAIL: could not read DVAR_SYSTEMINFO and DVAR_CODINFO from src/universal/q_shared.h")
        return False
    systeminfo = set()
    for name, places in registered.items():
        for where, function, arguments in places:
            if not function.startswith("Dvar_Register"):
                continue
            flags = flags_of(arguments, values)
            if flags is not None and flags & values["DVAR_SYSTEMINFO"]:
                systeminfo.add(name)
            if name.lower() not in listed:
                continue
            if flags is None:
                print(f"FAIL: could not read the flags src/{where} registers {name} with; "
                      "this check must know that a listed name is not replicated to clients")
                ok = False
            elif flags & replicated:
                print(f"FAIL: {name} is listed in server_dvar_guard.h, but src/{where} registers it as a setting "
                      "the server replicates to its clients (DVAR_SYSTEMINFO or DVAR_CODINFO); every stock server "
                      "would be refused")
                ok = False

    if {name.lower() for name in systeminfo} != {name.lower() for name in STOCK_SYSTEMINFO}:
        print("FAIL: the engine's DVAR_SYSTEMINFO settings are no longer the ones this check knows: "
              f"sources {sorted(systeminfo, key=str.lower)}, check {sorted(STOCK_SYSTEMINFO, key=str.lower)}. "
              "Update STOCK_SYSTEMINFO in scripts/tests/test_server_dvar_guard.py")
        ok = False

    if len(registered) < 1000:
        print(f"FAIL: found only {len(registered)} registered dvars; this check no longer reads the sources")
        ok = False
    if ok:
        added = sum(1 for name in registered if name.lower() not in retail_lower)
        print(f"PASS: all {added} dvars this port adds are on its list, and no listed name is replicated to clients "
              f"({len(registered)} registered, {len(port)} listed, no retail name among them)")
    return ok


def check_call_sites(cl_parse: Path = CL_PARSE, cg_servercmds: Path = CG_SERVERCMDS) -> bool:
    """Every dvar name a server chooses goes through CL_SetDvarFromServer, which asks the rule first."""
    ok = True
    parse_text = read_source(cl_parse)
    cgame_text = read_source(cg_servercmds)

    setter = function_body(parse_text, "void CL_SetDvarFromServer(")
    if setter is None:
        print(f"FAIL: {cl_parse.name} no longer defines CL_SetDvarFromServer the way this check reads it")
        return False
    guard = re.search(r"if\s*\(\s*!\s*Com_ServerMaySetDvar\s*\(\s*name\s*\)\s*\)", setter)
    if not guard:
        print("FAIL: CL_SetDvarFromServer does not ask Com_ServerMaySetDvar about the name")
        return False
    refused = function_body(setter[guard.end():], "")
    if refused is None or not re.search(r"\breturn\s*;\s*$", refused.strip()):
        print("FAIL: CL_SetDvarFromServer does not return after refusing a name")
        ok = False
    elif re.search(r"\bDvar_Set\w*\s*\(", refused):
        print("FAIL: CL_SetDvarFromServer sets a dvar it has just refused")
        ok = False
    # Before the rule only the player's own server may set: it runs here, this
    # client is joined to it, and no demo is playing.
    before = setter[:guard.start()]
    own = re.search(r"if\s*\(\s*com_sv_running->current\.enabled\s*&&\s*NET_IsLocalAddress\s*\(\s*clc->serverAddress"
                    r"\s*\)\s*&&\s*!\s*clc->demoplaying\s*\)", before)
    if own:
        own_block = function_body(before[own.end():], "")
        if own_block is not None:
            before = before[:own.end()] + before[own.end():].replace(own_block, "", 1)
    if re.search(r"\bDvar_Set\w*\s*\(", before):
        print("FAIL: CL_SetDvarFromServer sets a dvar before asking Com_ServerMaySetDvar, and not only for "
              "the player's own server (running here, joined over the local address, no demo playing)")
        ok = False

    def sets_by_name(body):
        return [match.group(0) for match in re.finditer(r"\bDvar_Set\w*ByName\w*\s*\(\s*([^\s\"(][^,)]*)", body)]

    sites = (
        (parse_text, "void CL_SystemInfoChanged(", cl_parse.name, "SERVER_DVAR_ROUTE_SYSTEMINFO"),
        (cgame_text, "void CG_ParseCodInfo(", cg_servercmds.name, "SERVER_DVAR_ROUTE_CODINFO"),
        (cgame_text, "void CG_SetClientDvarFromServer(", cg_servercmds.name, "SERVER_DVAR_ROUTE_CLIENT_DVAR"),
    )
    for text, signature, file_name, route in sites:
        function = signature[len("void "):-1]
        body = function_body(text, signature)
        if body is None:
            print(f"FAIL: {file_name} no longer defines {function} the way this check reads it")
            ok = False
            continue
        if not re.search(r"\bCL_SetDvarFromServer\s*\([^;]*\b" + route + r"\s*\)\s*;", body):
            print(f"FAIL: {function} does not set the server's dvars through CL_SetDvarFromServer(..., {route})")
            ok = False
        for call in sets_by_name(body):
            print(f"FAIL: {function} sets a dvar by a name that is not written out, past CL_SetDvarFromServer: {call}")
            ok = False

    # Nothing else in the two files sets a dvar by a name that is not written out.
    for text, path in ((parse_text, cl_parse), (cgame_text, cg_servercmds)):
        outside = text
        if path == cl_parse:
            start = text.find("void CL_SetDvarFromServer(")
            outside = text[:start] + text[start:].replace(setter, "", 1)
        for call in sets_by_name(outside):
            print(f"FAIL: {path.name} sets a dvar by a name that is not written out, past CL_SetDvarFromServer: {call}")
            ok = False

    if ok:
        print("PASS: systeminfo, codinfo and setClientDvar set dvars only through CL_SetDvarFromServer, "
              "which asks the rule first")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--m32", action="store_true",
                        help="require a 32-bit build in addition to the 64-bit one")
    parser.add_argument("--no-sanitize", action="store_true",
                        help="compile without -fsanitize=address,undefined")
    parser.add_argument("--list-registered", action="store_true",
                        help="print every dvar name registered in the sources, one per line, and exit")
    args = parser.parse_args()

    if args.list_registered:
        for name in sorted(registered_dvars(), key=lambda text: (text.lower(), text)):
            print(name)
        return 0

    for needed in (HEADER, CL_PARSE, CG_SERVERCMDS, Q_SHARED, RETAIL_NAMES):
        if not needed.is_file():
            print(f"FAIL: {needed} is missing", file=sys.stderr)
            return 1

    header = read_source(HEADER)
    port = header_list(header, "kServerDvarsOfThisPort")
    if not port:
        print("FAIL: could not read kServerDvarsOfThisPort from server_dvar_guard.h", file=sys.stderr)
        return 1

    ok = check_call_sites()
    ok = check_header_lists(header) and ok
    retail = read_retail_names()
    for name in retail:
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            print(f"FAIL: retail_dvar_names.txt holds {name!r}, which is not a dvar name", file=sys.stderr)
            return 1
    ok = check_lists(port, registered_dvars(), retail, flag_values()) and ok

    with tempfile.TemporaryDirectory(prefix="kisak-server-dvar-guard-") as temp:
        workdir = Path(temp)
        source = workdir / "server_dvar_guard_check.cpp"
        source.write_text(
            PROGRAM.replace("@STOCK_SYSTEMINFO@", "\n".join(f'    "{name}",' for name in STOCK_SYSTEMINFO))
            .replace("@RETAIL_NAMES@", "\n".join(f'    "{name}",' for name in retail)))

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
            executable = workdir / f"server_dvar_guard_check{width}"
            # Both char signs: the rule must not depend on which one the platform has.
            for sign in ("-fsigned-char", "-funsigned-char"):
                command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", sign, *flags,
                           f"-I{HEADER.parent}", str(source), "-o", str(executable)]
                print("+", " ".join(shlex.quote(part) for part in command))
                subprocess.run(command, check=True)
                print("+", shlex.quote(str(executable)))
                if subprocess.run([str(executable)]).returncode != 0:
                    print(f"FAIL: {width}-bit server dvar rule checks failed ({sign})", file=sys.stderr)
                    ok = False
                else:
                    print(f"PASS: {width}-bit server dvar rule checks ({sign})")

    print("PASS: server dvar guard" if ok else "FAIL: server dvar guard")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
