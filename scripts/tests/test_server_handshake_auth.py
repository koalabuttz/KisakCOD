#!/usr/bin/env python3
"""Exercise the production server handshake authentication code, asset-free.

The multi-player handshake is the only place an unauthenticated UDP packet can
decide what the server believes about a client's identity:

  ``SV_GetChallenge``   (``src/server_mp/sv_client_mp.cpp``) classifies the
                        handshake by field count: a retail CoD4 client's
                        ``getchallenge <n> "<32 hex GUID>"``, KisakCOD's
                        ``getchallenge <n> <ticket> <SteamID64>``, or nothing
                        else. It validates the claimed identity and ticket,
                        issues a challenge, and - in the native Linux dedicated
                        build - issues a synthetic ``local-test-...`` identity
                        for permitted loopback test clients.
  ``SV_DirectConnect``  consumes a challenge token and copies the stored
                        identity onto a ``client_t``, after deciding which kinds
                        of identity this process accepts at all.
  ``SV_DropClient``     tells the Steam backend about a departing client.

Everything here runs the real code. The three functions above are extracted from
production together with ``SV_IsTempBannedGuid``, ``SV_BanGuidBriefly``,
``SV_FindFreeTempBanSlot``, the ``sv_allowUnverifiedGuids`` capture and veto
(``SV_CaptureUnverifiedGuids``/``SV_UnverifiedGuidsEnabled``) and the static
``SV_RejectUnsafeTestHandshake`` guard, and the helpers are the real
``sv_handshake_auth.h`` header and the real ``b64_decode``/``b64_int`` from
``src/universal/base64.cpp``. The extracted client functions keep their text
unchanged except for the ``__cdecl`` calling-convention spelling, which is
dropped because it has no meaning on the x86-64/32 GNU ABI this harness builds
for; every signature is checked against the expected production signature before
it is used, so a source change fails the run instead of testing other code.
Only the surrounding engine services are doubled:
command argument text, out-of-band printing, ``svs``, dvars (including the
default-off ``sv_allowProtocol6`` switch and the
``sv_allowUnverifiedGuids`` dvar the capture reads), the Steam backend,
script allocation, netchan setup and the game's ``ClientConnect``. Those doubles
record every call so the tests can assert both the decision and its ordering.

Two builds are produced:

  * ``native-linux-dedicated`` compiles with ``-DKISAK_DEDICATED`` and so
    includes every ``#if defined(__linux__) && defined(KISAK_DEDICATED)``
    branch: the loopback test bypass, the synthetic identity, the explicit
    "Steam authentication is unavailable" reply and the ``SV_DropClient``
    short circuit.
  * ``nonnative`` omits ``-DKISAK_DEDICATED``. It is the same source text with
    those same guards compiled out, which is what a non-Linux or non-dedicated
    build sees of this code. It proves the bypass does not exist there and that
    the ordinary Steam rejection wording is unchanged.

Both are built 64-bit and, when the toolchain can produce them, 32-bit, since
the shipped Linux server target is 32-bit. ``--m32`` turns the 32-bit build into
a requirement: the run fails if no 32-bit binary can be produced. A toolchain
without a 32-bit sanitizer runtime still gets its 32-bit build, unsanitized, and
says so.

No Python reimplementation of the algorithm is tested, and no assertion here
reads the production source text as behaviour. If the source no longer has the
expected function signatures the extraction fails loudly instead of testing
something else.

Usage:
    python3 scripts/tests/test_server_handshake_auth.py
    python3 scripts/tests/test_server_handshake_auth.py --cxx clang++-19
    python3 scripts/tests/test_server_handshake_auth.py --cxx clang++ --m32
    python3 scripts/tests/test_server_handshake_auth.py --no-sanitize --m32

Every configuration that the toolchain can produce is built and run: the native
Linux dedicated guards and the non-native guards, at 64-bit and (when the
toolchain has 32-bit support) at 32-bit, with AddressSanitizer and
UndefinedBehaviorSanitizer unless the toolchain lacks them. What ran is printed
at the end.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLIENT_SOURCE = ROOT / "src" / "server_mp" / "sv_client_mp.cpp"
AUTH_HEADER = ROOT / "src" / "server_mp" / "sv_handshake_auth.h"
BASE64_SOURCE = ROOT / "src" / "universal" / "base64.cpp"

# (name, exact start of the definition text). The second field is verified so a
# signature change cannot silently turn this into a different test.
CLIENT_FUNCTIONS = (
    ("SV_CaptureUnverifiedGuids", "void __cdecl SV_CaptureUnverifiedGuids()"),
    ("SV_UnverifiedGuidsEnabled", "bool SV_UnverifiedGuidsEnabled()"),
    ("SV_CaptureAdvertisedProtocol", "void __cdecl SV_CaptureAdvertisedProtocol()"),
    ("SV_GetAdvertisedProtocol", "int SV_GetAdvertisedProtocol()"),
    ("SV_SetAdvertisedProtocolInfo", "void __cdecl SV_SetAdvertisedProtocolInfo(char *info)"),
    ("SV_FindFreeTempBanSlot", "uint32_t __cdecl SV_FindFreeTempBanSlot()"),
    ("SV_BanGuidBriefly", "void __cdecl SV_BanGuidBriefly("),
    ("SV_RejectUnsafeTestHandshake", "static bool SV_RejectUnsafeTestHandshake("),
    ("SV_IsTempBannedGuid", "int __cdecl SV_IsTempBannedGuid("),
    ("SV_GetChallenge", "void __cdecl SV_GetChallenge("),
    ("SV_DirectConnect", "void __cdecl SV_DirectConnect("),
    ("SV_DropClient", "void __cdecl SV_DropClient("),
)

BASE64_FUNCTIONS = (
    ("b64_int", "uint32_t b64_int("),
    ("b64_decode", "uint32_t b64_decode("),
)

TOKEN_RE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)


def extract_definition(source: str, name: str, expected_start: str, path: Path) -> str:
    """Return the full definition of ``name``: return type, body, closing brace.

    Only definitions are accepted (the next significant character after the
    parameter list must be ``{``), so a call site or a prototype in the same file
    cannot be picked up by mistake.
    """
    for match in re.finditer(r"(?<![\w:.>])" + re.escape(name) + r"\s*\(", source):
        opening = match.end() - 1
        depth = 0
        index = opening
        while index < len(source):
            if source[index] == "(":
                depth += 1
            elif source[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        else:
            continue

        after = index + 1
        while after < len(source) and source[after] in " \t\r\n":
            after += 1
        if after >= len(source) or source[after] != "{":
            continue                      # prototype or call, not the definition

        start = source.rfind("\n", 0, match.start()) + 1
        text = source[start:]
        if not text.startswith(expected_start):
            raise SystemExit(
                f"{path}: {name} does not start with the expected signature "
                f"{expected_start!r}; found {text[:len(expected_start) + 20]!r}"
            )

        brace = start + (after - start)
        depth = 0
        index = brace
        while index < len(source):
            token = TOKEN_RE.match(source, index)
            if token:
                value = token.group()
                if value == "{":
                    depth += 1
                elif value == "}":
                    depth -= 1
                    if depth == 0:
                        return source[start:index + 1] + "\n"
                index = token.end()
                continue
            index += 1

        raise SystemExit(f"{path}: unbalanced braces while extracting {name}")

    raise SystemExit(f"{path}: could not find the definition of {name}")


PREAMBLE = r'''
// Generated by scripts/tests/test_server_handshake_auth.py -- not a build target.
//
// The production functions below are compiled unchanged. Everything else in
// this file is a checked test double for an engine service the handshake calls;
// each double records its calls so the tests can assert the decision made by
// production code and the order in which side effects happened.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef NDEBUG
#error Assertions must remain enabled
#endif

typedef unsigned char byte;
typedef unsigned short word;
typedef uint32_t uint32;

// src/universal/base64.h
uint32_t b64_decode(const unsigned char *in, uint32_t in_len, unsigned char *out);

// ---- assertion channel -----------------------------------------------------
// Production calls MyAssertHandler() directly on the paths this file pins. Any
// call is a test failure: a malformed handshake field must be rejected, never
// asserted on.

static int g_checks;
static int g_failures;
static int g_asserts;

struct AssertFired
{
};

void MyAssertHandler(const char *file, int line, int type, const char *fmt, ...)
{
    va_list ap;
    (void)type;
    ++g_asserts;
    fprintf(stderr, "ASSERT FIRED %s:%d: ", file ? file : "(null)", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    throw AssertFired();
}

#define alwaysfails 0

// ---- engine data model (field names and types as in production) ------------

enum netadrtype_t
{
    NA_BOT,
    NA_BAD,
    NA_LOOPBACK,
    NA_BROADCAST,
    NA_IP,
    NA_IPX,
    NA_BROADCAST_IPX
};

enum netsrc_t
{
    NS_CLIENT1 = 0,
    NS_SERVER = 1,
    NS_PACKET = 2
};

enum
{
    CS_FREE = 0,
    CS_ZOMBIE = 1,
    CS_CONNECTED = 2,
    CS_CLIENTLOADING = 3,
    CS_ACTIVE = 4
};

enum svscmd_type
{
    SV_CMD_CAN_IGNORE = 0,
    SV_CMD_RELIABLE = 1
};

enum
{
    CON_CHANNEL_SERVER = 0xF,
    CON_CHANNEL_SYSTEM = 0x10
};

struct netadr_t
{
    netadrtype_t type;
    unsigned char ip[4];
    unsigned short port;
    unsigned char ipx[10];
};

struct netchan_t
{
    int outgoingSequence;
    netsrc_t sock;
    int dropped;
    int incomingSequence;
    netadr_t remoteAddress;
    int qport;
};

struct clientHeader_t
{
    int state;
    int sendAsActive;
    int deltaMessage;
    int rateDelayed;
    netchan_t netchan;
    float predictedOrigin[3];
    int predictedOriginServerTime;
};

struct gentity_s
{
    int clientNum;
};

// client_t is trimmed to the fields the handshake touches plus the two netchan
// buffers it hands to Netchan_Setup(). The snapshot/frame arrays are omitted:
// no extracted function reads them.
struct client_t
{
    clientHeader_t header;
    const char *dropReason;
    char userinfo[1024];
    int reliableSequence;
    int reliableAcknowledge;
    int reliableSent;
    int messageAcknowledge;
    int gamestateMessageNum;
    int challenge;
    int lastClientCommand;
    gentity_s *gentity;
    char name[16];
    int lastPacketTime;
    int lastConnectTime;
    int nextSnapshotTime;
    int timeoutCount;
    char netchanOutgoingBuffer[131072];
    char netchanIncomingBuffer[2048];
    char cdkeyHash[33];
    uint16_t scriptId;
    int voicePacketCount;
    bool sendVoice;
};

struct challenge_t
{
    netadr_t adr;
    int challenge;
    int time;
    int pingTime;
    int firstTime;
    int firstPing;
    int connected;
    char cdkeyHash[33];
};

struct tempBanSlot_t
{
    char cdkeyHash[32];
    int banTime;
};

union DvarValue
{
    bool enabled;
    int integer;
    float value;
    const char *string;
};

struct dvar_s
{
    const char *name;
    const char *description;
    word flags;
    byte type;
    bool modified;
    DvarValue current;
    DvarValue latched;
    DvarValue reset;
};

typedef dvar_s dvar_t;

// The snapshot arrays of the real serverStatic_t are not modelled; the handshake
// only uses these members.
struct serverStatic_t
{
    int initialized;
    int time;
    int snapFlagServerBit;
    client_t clients[64];
    challenge_t challenges[1024];
    netadr_t redirectAddress;
    netadr_t authorizeAddress;
    int sv_lastTimeMasterServerCommunicated;
    tempBanSlot_t tempBans[16];
};

static serverStatic_t svs;

static dvar_t dvar_maxclients;
static dvar_t dvar_minping;
static dvar_t dvar_maxping;
static dvar_t dvar_reconnectlimit;
static dvar_t dvar_privateclients;
static dvar_t dvar_privatepassword;
static dvar_t dvar_kickbantime;
static dvar_t dvar_dedicated;
static dvar_t dvar_fsgamedir;
// sv_allowProtocol6 is registered in SV_Init as a plain bool dvar defaulting to
// off (DVAR_NOFLAG); the handshake reads only its enabled state.
static dvar_t dvar_allowprotocol6;
// sv_allowUnverifiedGuids is registered in SV_Init as a DVAR_INIT bool
// defaulting to off. The harness owns the same storage production keeps in
// sv_client_mp.cpp so the extracted capture and veto bodies compile and can be
// driven through every captured/current combination.
static dvar_t dvar_allowunverifiedguids;
static const dvar_t *sv_allowUnverifiedGuids = &dvar_allowunverifiedguids;

static const dvar_t *sv_maxclients = &dvar_maxclients;
static const dvar_t *sv_minPing = &dvar_minping;
static const dvar_t *sv_maxPing = &dvar_maxping;
static const dvar_t *sv_reconnectlimit = &dvar_reconnectlimit;
static const dvar_t *sv_privateClients = &dvar_privateclients;
static const dvar_t *sv_privatePassword = &dvar_privatepassword;
static const dvar_t *sv_kickBanTime = &dvar_kickbantime;
static const dvar_t *com_dedicated = &dvar_dedicated;
static const dvar_t *fs_gameDirVar = &dvar_fsgamedir;
static const dvar_t *sv_allowProtocol6 = &dvar_allowprotocol6;

// sv_advertisedProtocol is registered in SV_Init as a DVAR_INIT int defaulting
// to 1; its capture is production code in sv_client_mp.cpp.
static dvar_t dvar_advertisedprotocol;
static const dvar_t *sv_advertisedProtocol = &dvar_advertisedprotocol;
// The read-only "protocol" serverinfo dvar that SV_Init seeds from the capture.
static dvar_t dvar_protocolinfo;
static const dvar_t *protocolInfo = &dvar_protocolinfo;

// ---- recording doubles -----------------------------------------------------

struct Recorder
{
    std::vector<std::string> events;
    std::vector<std::string> oob;
    std::vector<netadr_t> oobFrom;
    std::vector<std::string> printed;
    std::vector<std::string> alwaysPrinted;
    std::vector<std::string> debugPrinted;
    std::vector<std::string> steamDropped;
    std::vector<std::string> serverCommands;
    // Identities handed to SV_IsBannedGuid(), in call order, so a test can prove
    // a ban check received the normalised GUID and not some raw field.
    std::vector<std::string> banCheckedIdentities;
    int steamChecks;
    uint64_t lastSteamId;
    uint32_t lastTicketLength;
    int netchanSetups;
    netadr_t netchanAdr;
    int netchanQport;
    int userinfoChanged;
    int heartbeats;
    int freeClient;
    int freeClientScriptId;
    int clientConnectCalls;
    int lastClientNum;
    uint16_t lastScriptPersId;
    int scrAllocCalls;
    int sendDisconnects;
    std::string lastDisconnectReason;
    // Internal-style dvar writes (Dvar_SetInt), in call order.  DVAR_INIT and
    // DVAR_ROM refuse external and script input but let these through, which is
    // exactly the hole the advertised-protocol capture closes.
    std::vector<int> dvarSetIntValues;

    Recorder()
        : steamChecks(0)
        , lastSteamId(0)
        , lastTicketLength(0)
        , netchanSetups(0)
        , netchanQport(0)
        , userinfoChanged(0)
        , heartbeats(0)
        , freeClient(0)
        , freeClientScriptId(0)
        , clientConnectCalls(0)
        , lastClientNum(-1)
        , lastScriptPersId(0)
        , scrAllocCalls(0)
        , sendDisconnects(0)
    {
        netchanAdr.type = NA_BAD;
        netchanAdr.ip[0] = netchanAdr.ip[1] = netchanAdr.ip[2] = netchanAdr.ip[3] = 0;
        netchanAdr.port = 0;
    }
};

static Recorder R;

// Behaviour of the network module and the Steam backend, set per test.
static bool g_loopbackAuthTestFrozenEnabled = false;
static bool g_loopbackAuthTestCanBypass = false;
static bool g_steamAcceptsTicket = true;
static bool g_bannedGuid = false;
static const char *g_clientConnectDenial = NULL;
static uint32_t g_scrNextId = 1;
static gentity_s g_gentities[64];

static Recorder NewRecorder()
{
    return Recorder();
}

static void Record(const std::string &event)
{
    R.events.push_back(event);
}

static std::string Format(const char *fmt, va_list ap)
{
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    return std::string(buffer);
}

static std::string FormatVa(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string text = Format(fmt, ap);
    va_end(ap);
    return text;
}

// ---- commands --------------------------------------------------------------
// SV_Cmd_Argv() hands out pointers into the engine's command text buffer, and
// production code compares a fixed 32 bytes from those pointers
// (SV_IsTempBannedGuid). Reproduce that layout: one contiguous text buffer for
// the arguments, pointers into it, so short arguments see their neighbours
// exactly as they do in the engine.

static char g_cmdText[8192];
static const char *g_cmdArgv[4];
static int g_cmdArgc = 0;

static const char *SV_Cmd_Argv(int argIndex)
{
    if (argIndex < 0 || argIndex >= g_cmdArgc || !g_cmdArgv[argIndex])
        return "";
    return g_cmdArgv[argIndex];
}

// SV_Cmd_Argc() counts the tokenised fields of the current server command,
// including the command name. SV_GetChallenge() classifies the handshake with
// it, so the tests set it explicitly instead of inferring it from fields.
static int SV_Cmd_Argc()
{
    return g_cmdArgc;
}

// ---- printing --------------------------------------------------------------

// Com_Printf reaches the operator console always; Com_DPrintf only in a
// developer build. Both channels are recorded separately so the tests can tell
// which one a message used.
static int Com_Printf(int channel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string text = Format(fmt, ap);
    va_end(ap);
    R.printed.push_back(text);
    R.alwaysPrinted.push_back(text);
    (void)channel;
    return 0;
}

static int Com_DPrintf(int channel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string text = Format(fmt, ap);
    va_end(ap);
    R.printed.push_back(text);
    R.debugPrinted.push_back(text);
    (void)channel;
    return 0;
}

static int Com_PrintWarning(int channel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string text = Format(fmt, ap);
    va_end(ap);
    R.printed.push_back(text);
    R.alwaysPrinted.push_back(text);
    (void)channel;
    return 0;
}

// Dvar_SetInt is an internal write, so DVAR_INIT and DVAR_ROM accept it.
static void Dvar_SetInt(dvar_s *dvar, int value)
{
    R.dvarSetIntValues.push_back(value);
    if (dvar)
        dvar->current.integer = value;
}

// Info_SetValueForKey replaces an existing pair for the key and appends the new
// one, as src/universal/q_shared.cpp does. Only that behaviour is modelled, so
// the extracted publishing helper can be pointed at a stale info string.
static void Info_SetValueForKey(char *s, const char *key, const char *value)
{
    std::string text = s ? s : "";
    const std::string pair = std::string("\\") + key + "\\";
    const size_t at = text.find(pair);
    if (at != std::string::npos)
    {
        const size_t end = text.find('\\', at + pair.size());
        text.erase(at, end == std::string::npos ? std::string::npos : end - at);
    }
    if (value && *value)
        text += pair + value;
    std::strncpy(s, text.c_str(), 1023);
    s[1023] = 0;
}

static char g_vaBuffers[4][2048];
static int g_vaIndex = 0;

char *va(const char *fmt, ...)
{
    char *buffer = g_vaBuffers[g_vaIndex & 3];
    va_list ap;
    g_vaIndex += 1;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(g_vaBuffers[0]), fmt, ap);
    va_end(ap);
    return buffer;
}

int Com_sprintf(char *dest, uint32_t size, const char *fmt, ...)
{
    va_list ap;
    int written;
    va_start(ap, fmt);
    written = vsnprintf(dest, size, fmt, ap);
    va_end(ap);
    return written;
}

static void I_strncpyz(char *dest, const char *src, int destsize)
{
    int i;
    if (!dest || destsize <= 0)
        return;
    if (!src)
        src = "";
    for (i = 0; i < destsize - 1 && src[i]; ++i)
        dest[i] = src[i];
    dest[i] = 0;
}

static int I_stricmp(const char *s0, const char *s1)
{
    int c0;
    int c1;
    if (s0 == s1)
        return 0;
    if (!s0)
        return -1;
    if (!s1)
        return 1;
    do
    {
        c0 = *s0++;
        c1 = *s1++;
        if (c0 >= 'A' && c0 <= 'Z')
            c0 += 'a' - 'A';
        if (c1 >= 'A' && c1 <= 'Z')
            c1 += 'a' - 'A';
        if (c0 != c1)
            return c0 - c1;
    } while (c0 && c1);
    return 0;
}

// Info_ValueForKey keeps a small ring of buffers, like the engine's two-slot
// version: SV_DirectConnect holds the "password" pointer across later parsing.
static const char *Info_ValueForKey(const char *s, const char *key)
{
    static char values[4][1024];
    static int next = 0;
    const size_t capacity = sizeof(values[0]);
    char *value = values[next & 3];
    char part[1024];

    next += 1;
    value[0] = 0;
    if (!s)
        return value;
    if (*s == '\\')
        ++s;

    while (*s)
    {
        size_t length = 0;
        while (*s && *s != '\\')
        {
            if (length + 1 < sizeof(part))
                part[length++] = *s;
            ++s;
        }
        part[length] = 0;
        if (*s == '\\')
            ++s;

        length = 0;
        while (*s && *s != '\\')
        {
            if (length + 1 < capacity)
                value[length++] = *s;
            ++s;
        }
        value[length] = 0;

        if (!I_stricmp(part, key))
            return value;
        if (*s == '\\')
            ++s;
    }
    value[0] = 0; // Production returns an empty string for a missing key.
    return value;
}

// ---- addresses -------------------------------------------------------------
// NET_CompareAdrSigned / NET_CompareBaseAdrSigned from
// src/qcommon/net_chan_mp.cpp, reproduced so slot lookup behaves as it does in
// the engine.

static int CompareBaseAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return static_cast<int>(a.type) - static_cast<int>(b.type);
    switch (a.type)
    {
    case NA_LOOPBACK:
        return a.port - b.port;
    case NA_BOT:
        return a.port - b.port;
    case NA_IP:
        return memcmp(a.ip, b.ip, 4);
    case NA_IPX:
        return memcmp(a.ipx, b.ipx, 10);
    default:
        return 0;
    }
}

static int CompareAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return static_cast<int>(a.type) - static_cast<int>(b.type);
    switch (a.type)
    {
    case NA_LOOPBACK:
        return 0;
    case NA_IP:
        if (a.port == b.port)
            return memcmp(a.ip, b.ip, 4);
        return a.port - b.port;
    case NA_IPX:
        return memcmp(a.ipx, b.ipx, 10);
    default:
        return 0;
    }
}

static bool NET_CompareAdr(netadr_t a, netadr_t b)
{
    return CompareAdr(a, b) == 0;
}

static bool NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    return CompareBaseAdr(a, b) == 0;
}

static bool NET_IsLocalAddress(netadr_t adr)
{
    return adr.type == NA_LOOPBACK || adr.type == NA_BOT;
}

static char g_adrString[64];

static const char *NET_AdrToString(netadr_t a)
{
    snprintf(g_adrString, sizeof(g_adrString), "%u.%u.%u.%u:%u",
             a.ip[0], a.ip[1], a.ip[2], a.ip[3], static_cast<unsigned>(a.port));
    return g_adrString;
}

static bool NET_OutOfBandPrint(netsrc_t sock, netadr_t adr, const char *data)
{
    (void)sock;
    R.oob.push_back(std::string(data ? data : ""));
    R.oobFrom.push_back(adr);
    return true;
}

// LAN classification only gates the sv_minPing/sv_maxPing comparisons, which
// the tests leave disabled. Mirrors Sys_IsLANAddress* for the private and
// loopback ranges.
static bool Sys_IsLANAddress(netadr_t adr)
{
    if (NET_IsLocalAddress(adr))
        return true;
    if (adr.type != NA_IP)
        return false;
    if (adr.ip[0] == 127 || adr.ip[0] == 10)
        return true;
    if (adr.ip[0] == 192 && adr.ip[1] == 168)
        return true;
    if (adr.ip[0] == 172 && adr.ip[1] >= 16 && adr.ip[1] < 32)
        return true;
    return false;
}

// ---- Linux network module (owned by the net tests, doubled here) -----------
// NET_LoopbackAuthTestEnabled() is the captured start-up answer and
// NET_CanBypassLoopbackAuth() the per-packet authorisation answer, exactly the
// two questions SV_GetChallenge/SV_DirectConnect ask of src/linux/net.cpp.

#if defined(__linux__) && defined(KISAK_DEDICATED)
static bool NET_LoopbackAuthTestEnabled()
{
    return g_loopbackAuthTestFrozenEnabled;
}

static bool NET_CanBypassLoopbackAuth(netadr_t from)
{
    if (!g_loopbackAuthTestFrozenEnabled || !g_loopbackAuthTestCanBypass)
        return false;
    return from.type == NA_IP && from.ip[0] == 127;
}
#endif

// ---- Steam backend ---------------------------------------------------------

static bool Steam_CheckClientTicket(const void *pAuthTicket, uint32_t authTicketLen, uint64_t steamID64)
{
    R.steamChecks += 1;
    R.lastSteamId = steamID64;
    R.lastTicketLength = authTicketLen;
    Record(FormatVa("steam_check:%llu:%u", static_cast<unsigned long long>(steamID64),
                    static_cast<unsigned>(authTicketLen)));
    (void)pAuthTicket;
    return g_steamAcceptsTicket;
}

static void Steam_OnClientDropped(uint64_t steamID64)
{
    R.steamDropped.push_back(FormatVa("%llu", static_cast<unsigned long long>(steamID64)));
    Record(FormatVa("steam_dropped:%llu", static_cast<unsigned long long>(steamID64)));
}

// ---- remaining engine services --------------------------------------------

static int SV_IsBannedGuid(const char *cdkeyHash)
{
    R.banCheckedIdentities.push_back(std::string(cdkeyHash ? cdkeyHash : ""));
    return g_bannedGuid ? 1 : 0;
}

static gentity_s *SV_GentityNum(int num)
{
    if (num < 0 || num >= 64)
        return NULL;
    return &g_gentities[num];
}

static uint32_t Scr_AllocArray()
{
    R.scrAllocCalls += 1;
    return g_scrNextId++;
}

static void Netchan_Setup(netsrc_t sock, netchan_t *chan, netadr_t adr, int qport,
                          char *outgoingBuffer, int outgoingBufferSize,
                          char *incomingBuffer, int incomingBufferSize)
{
    (void)sock;
    (void)outgoingBuffer;
    (void)incomingBuffer;
    (void)outgoingBufferSize;
    (void)incomingBufferSize;
    R.netchanSetups += 1;
    R.netchanAdr = adr;
    R.netchanQport = qport;
    if (chan)
    {
        // Netchan_Setup() in src/qcommon/net_chan_mp.cpp stores both on the
        // channel; the address and qport a rejected connect leaves behind are
        // what the reconnect lookup matches later.
        chan->remoteAddress = adr;
        chan->qport = qport;
    }
    Record(FormatVa("netchan:%s:%d", NET_AdrToString(adr), qport));
}

static char *ClientConnect(uint32_t clientNum, uint16_t scriptPersId)
{
    R.clientConnectCalls += 1;
    R.lastClientNum = static_cast<int>(clientNum);
    R.lastScriptPersId = scriptPersId;
    Record(FormatVa("client_connect:%u:%u", clientNum, static_cast<unsigned>(scriptPersId)));
    return const_cast<char *>(g_clientConnectDenial);
}

static void SV_UserinfoChanged(client_t *cl)
{
    (void)cl;
    R.userinfoChanged += 1;
    Record("userinfo_changed");
}

static void SV_Heartbeat_f()
{
    R.heartbeats += 1;
    Record("heartbeat");
}

static void SV_FreeClient(client_t *cl)
{
    (void)cl;
    R.freeClient += 1;
    Record("free_client");
}

static void SV_FreeClientScriptId(client_t *cl)
{
    (void)cl;
    R.freeClientScriptId += 1;
    Record("free_client_script_id");
}

static void SV_SendServerCommand(client_t *cl, svscmd_type type, const char *fmt, ...)
{
    va_list ap;
    (void)cl;
    va_start(ap, fmt);
    std::string message = Format(fmt, ap);
    va_end(ap);
    R.serverCommands.push_back(message);
    Record(FormatVa("svcmd:%d:%s", static_cast<int>(type), message.c_str()));
}

static void SV_SendDisconnect(client_t *client, int state, const char *reason,
                              bool translationForReason, const char *clientName)
{
    (void)client;
    (void)state;
    (void)translationForReason;
    (void)clientName;
    R.sendDisconnects += 1;
    R.lastDisconnectReason = std::string(reason ? reason : "");
    Record(std::string("send_disconnect:") + (reason ? reason : ""));
}

static const char *SEH_StringEd_GetString(const char *pszReference)
{
    (void)pszReference;
    return NULL;
}

// ---- sv_allowUnverifiedGuids capture state ---------------------------------
// Production freezes sv_allowUnverifiedGuids into these two file-scope statics
// when SV_Init() registers the dvar. The extracted SV_CaptureUnverifiedGuids()
// and SV_UnverifiedGuidsEnabled() bodies read and write them, so the harness
// declares the same storage and the tests drive the captured/current matrix
// exactly as a real process would experience it.
static bool unverifiedGuidsCaptured = false;
static bool unverifiedGuidsEnabled = false;

// ---- sv_advertisedProtocol capture state -----------------------------------
// Production freezes the validated value into these two statics when SV_Init()
// registers the dvar.
static bool advertisedProtocolCaptured = false;
static int advertisedProtocolValue = 1;
'''

DRIVER_HEAD = r'''
// ---- test support ----------------------------------------------------------

static std::string Escape(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c >= 32 && c < 127)
            out += static_cast<char>(c);
        else
        {
            char buffer[8];
            snprintf(buffer, sizeof(buffer), "\\x%02x", c);
            out += buffer;
        }
    }
    return out;
}

static void Check(bool ok, const std::string &what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        printf("  FAIL  %s\n", what.c_str());
    }
    else
    {
        printf("  ok    %s\n", what.c_str());
    }
}

static void CheckText(const std::string &got, const std::string &want, const std::string &what)
{
    std::string label = what + " -> \"" + Escape(got) + "\"";
    if (got != want)
        label += " (expected \"" + Escape(want) + "\")";
    Check(got == want, label);
}

static void CheckEq(long long got, long long want, const std::string &what)
{
    std::string label = what + " -> " + FormatVa("%lld", got);
    if (got != want)
        label += FormatVa(" (expected %lld)", want);
    Check(got == want, label);
}

static netadr_t MakeAdr(int a, int b, int c, int d, unsigned short port)
{
    netadr_t adr;
    memset(&adr, 0, sizeof(adr));
    adr.type = NA_IP;
    adr.ip[0] = static_cast<unsigned char>(a);
    adr.ip[1] = static_cast<unsigned char>(b);
    adr.ip[2] = static_cast<unsigned char>(c);
    adr.ip[3] = static_cast<unsigned char>(d);
    adr.port = port;
    return adr;
}

static netadr_t MakeLoopbackAdr()
{
    netadr_t adr;
    memset(&adr, 0, sizeof(adr));
    adr.type = NA_LOOPBACK;
    return adr;
}

static bool SetArgv(const std::string &a0, const std::string &a1 = std::string(),
                    const std::string &a2 = std::string(), const std::string &a3 = std::string(),
                    int argc = -1)
{
    const std::string *args[4] = { &a0, &a1, &a2, &a3 };
    size_t used = 0;
    g_cmdArgc = 0;
    for (int i = 0; i < 4; ++i)
    {
        size_t length = args[i]->size();
        if (used + length + 1 > sizeof(g_cmdText))
        {
            Check(false, "the command text double is too small for this test input");
            return false;
        }
        memcpy(g_cmdText + used, args[i]->data(), length);
        g_cmdText[used + length] = 0;
        g_cmdArgv[i] = g_cmdText + used;
        used += length + 1;
        g_cmdArgc = i + 1;
    }
    // Production classifies the handshake with SV_Cmd_Argc(), not by inspecting
    // fields. Let a test declare a count the four stored slots do not imply:
    // argc 2 (no identity field at all) or 5 (fields nobody reads).
    if (argc >= 0)
        g_cmdArgc = argc;
    return true;
}

static bool AnyLineContains(const std::vector<std::string> &lines, const char *needle)
{
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(needle) != std::string::npos)
            return true;
    return false;
}

static std::string LastOob()
{
    if (R.oob.empty())
        return std::string();
    return R.oob.back();
}

static int OobChallengeToken()
{
    std::string text = LastOob();
    const std::string prefix = "challengeResponse ";
    if (text.compare(0, prefix.size(), prefix) != 0)
        return -1;
    return atoi(text.c_str() + prefix.size());
}

static std::string SlotIdentity(int slot)
{
    // A missing slot is a test failure the caller reports; returning "" keeps a
    // failed lookup from reading outside svs.challenges.
    if (slot < 0 || slot >= 1024)
        return std::string();
    return std::string(svs.challenges[slot].cdkeyHash);
}

static bool SlotIsZeroed(int slot)
{
    const challenge_t &c = svs.challenges[slot];
    if (c.adr.type != NA_BOT || c.adr.port != 0 || c.challenge != 0 || c.time != 0 ||
        c.pingTime != 0 || c.firstTime != 0 || c.firstPing != 0 || c.connected != 0)
        return false;
    for (size_t i = 0; i < sizeof(c.cdkeyHash); ++i)
        if (c.cdkeyHash[i] != 0)
            return false;
    return true;
}

static bool AllChallengesZeroed()
{
    for (int i = 0; i < 1024; ++i)
        if (!SlotIsZeroed(i))
            return false;
    return true;
}

// Challenge slot lookup exactly as SV_DirectConnect/SV_DropClient use it.
static int FindSlot(netadr_t adr)
{
    for (int i = 0; i < 1024; ++i)
        if (NET_CompareAdr(adr, svs.challenges[i].adr))
            return i;
    return -1;
}

static void ResetWorld()
{
    memset(&svs, 0, sizeof(svs));
    memset(g_gentities, 0, sizeof(g_gentities));
    R = NewRecorder();
    g_asserts = 0;
    g_loopbackAuthTestFrozenEnabled = false;
    g_loopbackAuthTestCanBypass = false;
    g_steamAcceptsTicket = true;
    g_bannedGuid = false;
    g_clientConnectDenial = NULL;
    g_scrNextId = 1;
    g_cmdArgc = 0;
    memset(g_cmdText, 0, sizeof(g_cmdText));
    unverifiedGuidsCaptured = false;
    unverifiedGuidsEnabled = false;
    dvar_allowunverifiedguids.current.enabled = false;
    advertisedProtocolCaptured = false;
    advertisedProtocolValue = 1;
    dvar_advertisedprotocol.current.integer = 1;
    dvar_protocolinfo.current.integer = 1;

    svs.time = 1000;

    dvar_maxclients.current.integer = 4;
    dvar_minping.current.integer = 0;
    dvar_maxping.current.integer = 0;
    dvar_reconnectlimit.current.integer = 0;
    dvar_privateclients.current.integer = 0;
    dvar_privatepassword.current.string = "";
    dvar_kickbantime.current.value = 600.0f;
    dvar_dedicated.current.integer = 1;
    dvar_fsgamedir.current.string = "mods/kisak";
    dvar_allowprotocol6.current.enabled = false;

    srand(0x5EED);
}

static void Run(const char *name, void (*test)())
{
    printf("\n== %s ==\n", name);
    ResetWorld();
    int assertsBefore = g_asserts;
    try
    {
        test();
    }
    catch (const AssertFired &)
    {
        ++g_failures;
        printf("  FAIL  %s: MyAssertHandler fired on a handshake input\n", name);
    }
    if (g_asserts != assertsBefore)
        Check(false, std::string(name) + ": no MyAssertHandler call");
}

// A ticket that passes SV_ValidateSteamTicketBase64.
static const char *const kGoodTicket = "QUJDREVGR0hJSktMTU5PUA==";
static const char *const kGoodIdentity = "76561197960287930";

// Issues a challenge with valid fields and returns its token, leaving the slot
// populated. Fails the check (and returns -1) if the server did not issue one.
static int IssueChallenge(netadr_t from, const char *ticket = kGoodTicket,
                          const char *identity = kGoodIdentity)
{
    SetArgv("getchallenge", "", ticket, identity);
    SV_GetChallenge(from);
    return OobChallengeToken();
}

static void ConnectAttempt(netadr_t from, const char *userinfo)
{
    SetArgv("directconnect", userinfo);
    SV_DirectConnect(from);
}

// ---- SV_ParseSteamIdentity -------------------------------------------------

struct IdentityCase
{
    const char *text;
    bool ok;
    unsigned long long value;
};

static const IdentityCase kIdentityCases[] = {
    { "1", true, 1ULL },
    // A uint64 decimal identity fits in 20 digits; overlong zero-padded
    // spellings are refused rather than truncated into the 33-byte GUID field.
    { "00000000000000000001", true, 1ULL },
    { "000000000000000000001", false, 0ULL },
    { "0000000000000000000001", false, 0ULL },
    { "0000000000000000000000000000000000000001", false, 0ULL },
    { "76561197960287930", true, 76561197960287930ULL },
    { "1844674407370955161", true, 1844674407370955161ULL },
    { "18446744073709551614", true, 18446744073709551614ULL },
    { "18446744073709551615", true, 18446744073709551615ULL },
    { "18446744073709551616", false, 0ULL },
    { "99999999999999999999", false, 0ULL },
    { "", false, 0ULL },
    { "0", false, 0ULL },
    { "000", false, 0ULL },
    { "-1", false, 0ULL },
    { "+1", false, 0ULL },
    { " 1", false, 0ULL },
    { "1 ", false, 0ULL },
    { "1\n", false, 0ULL },
    { "\t1", false, 0ULL },
    { "1a", false, 0ULL },
    { "0x10", false, 0ULL },
    { "1.0", false, 0ULL },
    { "76561197960287930x", false, 0ULL },
    // The server's own local-test namespace is not a claimable Steam ID.
    { "local-test-000012340000", false, 0ULL },
    { "local-test-", false, 0ULL },
    // ARABIC-INDIC DIGIT ONE: the loop compares bytes, not characters.
    { "\xD9\xA1", false, 0ULL },
    { "\xFF", false, 0ULL },
};

static void TestParseSteamIdentity()
{
    const unsigned long long sentinel = 0xDEADBEEFCAFEF00DULL;

    for (size_t i = 0; i < sizeof(kIdentityCases) / sizeof(kIdentityCases[0]); ++i)
    {
        const IdentityCase &c = kIdentityCases[i];
        uint64_t identity = sentinel;
        bool ok = SV_ParseSteamIdentity(c.text, &identity);
        std::string label = std::string("SV_ParseSteamIdentity(\"") + Escape(c.text) + "\")";
        Check(ok == c.ok, label + (c.ok ? " accepted" : " rejected"));
        if (c.ok)
            CheckEq(static_cast<long long>(identity), static_cast<long long>(c.value), label + " value");
        else
            Check(identity == sentinel, label + " leaves the output untouched");
    }

    uint64_t identity = sentinel;
    Check(!SV_ParseSteamIdentity(NULL, &identity), "SV_ParseSteamIdentity(NULL, out) rejected");
    Check(identity == sentinel, "SV_ParseSteamIdentity(NULL, out) leaves the output untouched");
    Check(!SV_ParseSteamIdentity("1", NULL), "SV_ParseSteamIdentity(text, NULL) rejected");
}

// ---- SV_ValidateSteamTicketBase64 -----------------------------------------

struct TicketCase
{
    const char *text;
    size_t capacity;
    bool ok;
    size_t encodedLength;
    uint32_t decodedLength;
};

static const TicketCase kTicketCases[] = {
    { "AAAA", 1152, true, 4, 3 },
    { "AAAA", 3, true, 4, 3 },
    { "AAAA", 0, false, 0, 0 },
    { "AA==", 2, false, 0, 0 },
    { "AA==", 1152, true, 4, 1 },
    { "AAA=", 1152, true, 4, 2 },
    { "A===", 1152, false, 0, 0 },
    { "====", 1152, false, 0, 0 },
    { "AA=A", 1152, false, 0, 0 },
    { "AA*A", 1152, false, 0, 0 },
    { "AA-A", 1152, false, 0, 0 },
    { "AA_A", 1152, false, 0, 0 },
    { "AA.A", 1152, false, 0, 0 },
    { "AA A", 1152, false, 0, 0 },
    { "AA\nA", 1152, false, 0, 0 },
    { "AA\tA", 1152, false, 0, 0 },
    { "AA+/", 1152, true, 4, 3 },
    { "aaaa", 1152, true, 4, 3 },
    { "////", 1152, true, 4, 3 },
    { "AAAA====", 1152, false, 0, 0 },
    { "A", 1152, false, 0, 0 },
    { "AA", 1152, false, 0, 0 },
    { "AAA", 1152, false, 0, 0 },
    { "aaaa0", 1152, false, 0, 0 },
    { "", 1152, false, 0, 0 },
    { "AAAAAAAA", 6, true, 8, 6 },
    { "AAAAAAAAAAAA", 6, false, 0, 0 },
};

static void TestValidateSteamTicketBase64()
{
    for (size_t i = 0; i < sizeof(kTicketCases) / sizeof(kTicketCases[0]); ++i)
    {
        const TicketCase &c = kTicketCases[i];
        size_t encodedLength = 999;
        bool ok = SV_ValidateSteamTicketBase64(c.text, c.capacity, &encodedLength);
        std::string label = FormatVa("SV_ValidateSteamTicketBase64(\"%s\", capacity %u)",
                                     Escape(c.text).c_str(), static_cast<unsigned>(c.capacity));
        Check(ok == c.ok, label + (c.ok ? " accepted" : " rejected"));
        if (!c.ok)
        {
            Check(encodedLength == 999, label + " leaves encodedLength untouched");
            continue;
        }
        CheckEq(static_cast<long long>(encodedLength), static_cast<long long>(c.encodedLength),
                label + " encodedLength");

        // The validated length is exactly what SV_GetChallenge hands to the
        // production decoder; the decoder must agree on the decoded size and
        // must not write past it.
        std::vector<unsigned char> decoded(c.decodedLength ? c.decodedLength : 1);
        uint32_t produced = b64_decode(reinterpret_cast<const unsigned char *>(c.text),
                                       static_cast<uint32_t>(encodedLength), decoded.data());
        CheckEq(static_cast<long long>(produced), static_cast<long long>(c.decodedLength),
                label + " b64_decode length");
    }

    size_t encodedLength = 999;
    Check(!SV_ValidateSteamTicketBase64(NULL, 1152, &encodedLength), "ticket NULL text rejected");
    Check(!SV_ValidateSteamTicketBase64("AAAA", 1152, NULL), "ticket NULL encodedLength rejected");

    // A high byte at a legal position must be refused by the alphabet check,
    // not by the length check: build 4 characters with a 0x80 in the middle.
    {
        std::string hostile("AA");
        hostile += static_cast<char>(0x80);
        hostile += 'A';
        encodedLength = 999;
        Check(!SV_ValidateSteamTicketBase64(hostile.c_str(), 1152, &encodedLength),
              "ticket with a non-ASCII byte rejected");
    }

    // Ticket size boundary for the real capacity used by SV_GetChallenge:
    // (1152 / 3) * 4 == 1536 encoded characters, which decode to exactly 1152.
    struct SizeCase
    {
        size_t length;
        bool ok;
        size_t decodedLength;
    };
    static const SizeCase kSizes[] = {
        { 4, true, 3 },
        { 1532, true, 1149 },
        { 1536, true, 1152 },
        { 1540, false, 0 },
        { 4096, false, 0 },
        { 8192, false, 0 },
    };
    for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i)
    {
        std::string text(kSizes[i].length, 'A');
        encodedLength = 999;
        bool ok = SV_ValidateSteamTicketBase64(text.c_str(), 1152, &encodedLength);
        std::string label = FormatVa("ticket of %u base64 characters", static_cast<unsigned>(text.size()));
        Check(ok == kSizes[i].ok, label + (kSizes[i].ok ? " accepted" : " rejected"));
        if (!kSizes[i].ok)
            continue;
        CheckEq(static_cast<long long>(encodedLength), static_cast<long long>(text.size()), label + " encodedLength");
        std::vector<unsigned char> decoded(kSizes[i].decodedLength);
        uint32_t produced = b64_decode(reinterpret_cast<const unsigned char *>(text.data()),
                                       static_cast<uint32_t>(encodedLength), decoded.data());
        CheckEq(static_cast<long long>(produced), static_cast<long long>(kSizes[i].decodedLength),
                label + " b64_decode length");
    }
}

#if defined(__linux__) && defined(KISAK_DEDICATED)
// ---- SV_IsLoopbackTestIdentity ---------------------------------------------

static void TestLoopbackTestIdentity()
{
    Check(SV_IsLoopbackTestIdentity("local-test-00001234ffff"),
          "server-issued identity recognised");
    Check(SV_IsLoopbackTestIdentity("local-test-"), "prefix alone is recognised");
    Check(!SV_IsLoopbackTestIdentity("local-test"), "truncated prefix is not recognised");
    Check(!SV_IsLoopbackTestIdentity("local-tesX-00001234ffff"), "mutated prefix is not recognised");
    Check(!SV_IsLoopbackTestIdentity("76561197960287930"), "numeric Steam ID is not a test identity");
    Check(!SV_IsLoopbackTestIdentity(""), "empty identity is not a test identity");
    Check(!SV_IsLoopbackTestIdentity(NULL), "NULL identity is not a test identity");
}
#endif

// ---- SV_GetChallenge, ordinary (authenticated) path ------------------------

static void TestGetChallengeMissingFields()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    SetArgv("getchallenge");
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.",
              "no fields at all");
    CheckEq(R.steamChecks, 0, "no Steam backend call");
    Check(R.oob.size() == 1, "exactly one reply");
    Check(AllChallengesZeroed(), "no challenge slot was touched");

    SetArgv("getchallenge", "", "", kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.",
              "ticket missing");

    SetArgv("getchallenge", "", kGoodTicket, "");
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.",
              "identity missing");
    CheckEq(R.steamChecks, 0, "still no Steam backend call");
    Check(AllChallengesZeroed(), "still no challenge slot touched");
}

static void TestGetChallengeMalformedFields()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    struct Case
    {
        const char *ticket;
        const char *identity;
        const char *what;
    };
    const Case cases[] = {
        { kGoodTicket, "abc", "non-numeric identity" },
        { kGoodTicket, "-1", "negative identity" },
        { kGoodTicket, "0", "zero identity" },
        { kGoodTicket, "18446744073709551616", "identity overflowing uint64" },
        { kGoodTicket, " 76561197960287930", "identity with leading space" },
        { kGoodTicket, "local-test-000012340000", "synthetic test identity claimed as a Steam ID" },
        { "AAA", kGoodIdentity, "ticket length not a multiple of four" },
        { "AA*A", kGoodIdentity, "ticket with an illegal character" },
        { "A===", kGoodIdentity, "ticket with excess padding" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        SetArgv("getchallenge", "", cases[i].ticket, cases[i].identity);
        SV_GetChallenge(from);
        CheckText(LastOob(), "error\nMalformed Steam authentication fields.", cases[i].what);
    }
    CheckEq(R.steamChecks, 0, "malformed fields never reach the Steam backend");
    Check(AllChallengesZeroed(), "no challenge slot was touched");

    // An empty ticket is reported as a missing field, not as malformed input.
    SetArgv("getchallenge", "", "", kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.",
              "empty ticket counted as a missing field");

    // The removed code path asserted on an oversized ticket; it must simply be
    // rejected now.
    std::string oversized(4096, 'A');
    SetArgv("getchallenge", "", oversized.c_str(), kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(), "error\nMalformed Steam authentication fields.",
              "4096-character ticket rejected without asserting");
    CheckEq(R.steamChecks, 0, "oversized ticket never reaches the Steam backend");
    Check(AllChallengesZeroed(), "oversized ticket leaves no challenge slot");

    std::string justOver(1540, 'A');
    SetArgv("getchallenge", "", justOver.c_str(), kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(), "error\nMalformed Steam authentication fields.",
              "ticket one group past the buffer bound rejected");
}

static void TestGetChallengeBackendRejection()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    g_steamAcceptsTicket = false;

    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);

#if defined(__linux__) && defined(KISAK_DEDICATED)
    CheckText(LastOob(), "error\nSteam authentication is unavailable in this native Linux build.",
              "native build reports the stand-in backend explicitly");
#else
    CheckText(LastOob(), "error\x0a\x15Your Steam Client Ticket was Invalid",
              "non-native build keeps the invalid-ticket wording");
    Check(R.printed.size() == 1 &&
              R.printed[0].find("rejected connection from invalid Steam GUID") != std::string::npos,
          "non-native build still logs the rejected GUID");
#endif
    CheckEq(R.steamChecks, 1, "the backend was asked exactly once");
    CheckEq(static_cast<long long>(R.lastSteamId), 76561197960287930LL, "backend saw the parsed identity");
    Check(R.lastTicketLength > 0, "backend saw a non-empty decoded ticket");
    Check(AllChallengesZeroed(), "a rejected authentication leaves no challenge");
}

static void TestGetChallengeAccepted()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);

    Check(token > 0, "challengeResponse carries a positive token");
    CheckEq(R.steamChecks, 1, "the backend was asked exactly once");
    CheckEq(static_cast<long long>(R.lastSteamId), 76561197960287930LL, "backend saw the parsed identity");
    int slot = FindSlot(from);
    Check(slot >= 0, "the challenge is recorded for the source address");
    if (slot >= 0)
    {
        CheckEq(svs.challenges[slot].challenge, token, "recorded token matches the reply");
        CheckText(SlotIdentity(slot), kGoodIdentity, "recorded identity is the claimed Steam ID");
        CheckEq(svs.challenges[slot].pingTime, svs.time, "recorded pingTime");
        CheckEq(svs.challenges[slot].connected, 0, "the slot is not marked connected yet");
    }

    // A second call from the same address reuses the slot and the token.
    int again = IssueChallenge(from);
    CheckEq(again, token, "a retry from the same address reuses the token");
    CheckEq(R.steamChecks, 2, "the retry authenticated again");
    CheckEq(R.oob.size(), 2, "two challenge replies");
}

static void TestGetChallengeFailureClearsIssuedChallenge()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);
    Check(token > 0, "a challenge was issued first");
    int slot = FindSlot(from);
    Check(slot >= 0, "the issued challenge is recorded");

    // A malformed retry must not leave the accepted token in place.
    SetArgv("getchallenge", "", "AA*A", kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(), "error\nMalformed Steam authentication fields.", "malformed retry rejected");
    Check(AllChallengesZeroed(), "no challenge survives a failed authentication");

    // The next attempt must start over: a fresh token for a fresh slot.
    int fresh = IssueChallenge(from);
    Check(fresh > 0, "a later attempt issues a new challenge");
    Check(AllChallengesZeroed() == false, "the new challenge is recorded");
    CheckEq(R.steamChecks, 2, "backend calls: one accepted, one malformed never reached it");

    // The same holds when the backend itself rejects a retry.
    ResetWorld();
    g_steamAcceptsTicket = true;
    token = IssueChallenge(from);
    Check(token > 0, "a challenge was issued again");
    g_steamAcceptsTicket = false;
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    Check(AllChallengesZeroed(), "backend rejection clears the earlier challenge too");
}

// The temp-ban table holds a fixed 32-byte key: pad the identity the way the
// engine stores it. Copying 32 bytes straight from the literal would read past
// the end of the string constant.
static void SeedTempBan(const char *identity, int banTime)
{
    size_t length = strlen(identity);
    memset(svs.tempBans[0].cdkeyHash, 0, sizeof(svs.tempBans[0].cdkeyHash));
    if (length > sizeof(svs.tempBans[0].cdkeyHash))
        length = sizeof(svs.tempBans[0].cdkeyHash);
    memcpy(svs.tempBans[0].cdkeyHash, identity, length);
    svs.tempBans[0].banTime = banTime;
}

static void TestGetChallengeIdentityLength()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // Exactly 20 digits is the longest accepted identity and must be stored in
    // full: the GUID field is 33 bytes and I_strncpyz() would otherwise cut an
    // overlong identity short.
    const char *twentyDigits = "00000000000000000001";
    int token = IssueChallenge(from, kGoodTicket, twentyDigits);
    Check(token > 0, "a 20-digit identity is accepted");
    int slot = FindSlot(from);
    Check(slot >= 0, "the 20-digit identity produced a challenge");
    if (slot >= 0)
        CheckText(SlotIdentity(slot), twentyDigits,
                  "the accepted 20-digit identity is stored without truncation");

    // 21 digits, and a long zero-padded spelling: refused as malformed input,
    // never truncated into a challenge record.
    const char *cases[] = {
        "000000000000000000001",
        "0000000000000000000000000000000000000001",
        "00000000000000000000000000000000000000000000000001",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        ResetWorld();
        SetArgv("getchallenge", "", kGoodTicket, cases[i]);
        SV_GetChallenge(from);
        std::string label = FormatVa("%u-digit zero-padded identity",
                                     static_cast<unsigned>(strlen(cases[i])));
        CheckText(LastOob(), "error\nMalformed Steam authentication fields.", label + " rejected");
        Check(AllChallengesZeroed(), label + " leaves no challenge record");
        CheckEq(R.steamChecks, 0, label + " never reaches the Steam backend");
    }
}

static void TestGetChallengeBans()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    g_bannedGuid = true;
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(), "error\x0a\x15You are permanently banned from this server",
              "permanently banned GUID rejected");
    CheckEq(R.steamChecks, 0, "banned GUID never reaches the backend");
    Check(AllChallengesZeroed(), "banned GUID leaves no challenge");

    // Temporary ban uses the real SV_IsTempBannedGuid against svs.tempBans.
    ResetWorld();
    SeedTempBan(kGoodIdentity, svs.time - 1000);
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(), "error\x0a\x15You are temporarily banned from this server",
              "temporarily banned GUID rejected");
    CheckEq(R.steamChecks, 0, "temporarily banned GUID never reaches the backend");
    Check(AllChallengesZeroed(), "temporarily banned GUID leaves no challenge");

    // Once the ban window has passed the same GUID is allowed through again.
    ResetWorld();
    SeedTempBan(kGoodIdentity, svs.time - 700000);
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    Check(OobChallengeToken() > 0, "an expired temporary ban no longer rejects the GUID");
    CheckEq(R.steamChecks, 1, "expired ban reaches the backend");
}

#if defined(__linux__) && defined(KISAK_DEDICATED)
// ---- SV_GetChallenge, loopback test mode -----------------------------------

static void TestLocalTestChallengeIdentity()
{
    netadr_t from = MakeAdr(127, 0, 0, 1, 28960);
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = true;

    // No authentication fields at all.
    SetArgv("getchallenge");
    SV_GetChallenge(from);
    int token = OobChallengeToken();
    Check(token > 0, "test mode issues a challenge without authentication fields");
    CheckEq(R.steamChecks, 0, "test mode never calls the Steam backend");

    int slot = FindSlot(from);
    Check(slot >= 0, "test mode records the challenge for the source address");
    if (slot < 0)
        return;

    std::string expected = FormatVa("local-test-%08x%04x", static_cast<unsigned int>(token),
                                    static_cast<unsigned int>(from.port));
    std::string stored = SlotIdentity(slot);
    CheckText(stored, expected, "identity is derived from the server token and source port");
    Check(stored.size() < 33, FormatVa("identity fits the 33-byte field (%u bytes)",
                                       static_cast<unsigned>(stored.size())));
    CheckEq(static_cast<long long>(stored.size()), 23, "identity length is fixed");
    Check(stored.compare(0, 11, "local-test-") == 0, "identity uses the local-test namespace");
    Check(SV_IsLoopbackTestIdentity(stored.c_str()), "generated identity is recognised by the guard");
    Check(!SV_ParseSteamIdentity(stored.c_str(), NULL),
          "generated identity is not a numeric Steam ID (NULL output still rejected)");
    {
        uint64_t parsed = 0;
        Check(!SV_ParseSteamIdentity(stored.c_str(), &parsed),
              "generated identity cannot be parsed as a claimed SteamID");
    }

    // A user-supplied identity is ignored: the server token wins.
    SetArgv("getchallenge", "", "", "76561197960287930");
    SV_GetChallenge(from);
    CheckText(SlotIdentity(slot), expected, "a client-supplied Steam ID is not adopted");
    CheckEq(OobChallengeToken(), token, "the retry keeps the issued token");

    SetArgv("getchallenge", "", "", "local-test-deadbeef0000");
    SV_GetChallenge(from);
    CheckText(SlotIdentity(slot), expected, "a client-supplied local-test string is not adopted");
    CheckEq(R.steamChecks, 0, "no Steam backend call in any test-mode attempt");
}

static void TestLocalTestChallengeIsStableAndUnique()
{
    netadr_t fromA = MakeAdr(127, 0, 0, 1, 1111);
    netadr_t fromB = MakeAdr(127, 0, 0, 1, 2222);
    netadr_t fromC = MakeAdr(127, 0, 0, 2, 1111);
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = true;

    int tokenA = IssueChallenge(fromA, "", "");
    std::string identityA = SlotIdentity(FindSlot(fromA));
    Check(tokenA > 0, "first client got a challenge");
    Check(identityA == FormatVa("local-test-%08x%04x", static_cast<unsigned int>(tokenA), 1111u),
          "first client identity matches the token and port");

    // Same client, retry: same token, same identity.
    int tokenA2 = IssueChallenge(fromA, "", "");
    CheckEq(tokenA2, tokenA, "retry keeps the token");
    CheckText(SlotIdentity(FindSlot(fromA)), identityA, "retry keeps the identity");

    // Different source port, different client.
    int tokenB = IssueChallenge(fromB, "", "");
    std::string identityB = SlotIdentity(FindSlot(fromB));
    Check(tokenB > 0, "second client got a challenge");
    Check(identityB != identityA, "a different client gets a different identity");

    // Same port, different address: isolate the token contribution by pinning
    // both slots to the same token.
    memset(&svs.challenges[0], 0, sizeof(svs.challenges[0]));
    memset(&svs.challenges[1], 0, sizeof(svs.challenges[1]));
    svs.challenges[0].adr = fromA;
    svs.challenges[0].challenge = 0x0BADF00D;
    svs.challenges[1].adr = fromC;
    svs.challenges[1].challenge = 0x0BADF00D;
    SetArgv("getchallenge");
    SV_GetChallenge(fromA);
    std::string pinnedA = SlotIdentity(0);
    SetArgv("getchallenge");
    SV_GetChallenge(fromC);
    std::string pinnedC = SlotIdentity(1);
    CheckText(pinnedA, "local-test-0badf00d0457", "identity embeds token and port (0.0.1:1111)");
    CheckText(pinnedC, "local-test-0badf00d0457", "identical token and port give the identical label");
    memset(&svs.challenges[1], 0, sizeof(svs.challenges[1]));
    svs.challenges[1].adr = fromB;
    svs.challenges[1].challenge = 0x0BADF00D;
    SetArgv("getchallenge");
    SV_GetChallenge(fromB);
    CheckText(SlotIdentity(1), "local-test-0badf00d08ae",
              "the source port alone changes the identity (0.0.1:2222)");
    CheckEq(R.steamChecks, 0, "no Steam backend call while issuing local test challenges");
}

static void TestLocalTestGuardRejectsUnpermittedSource()
{
    netadr_t from = MakeAdr(192, 168, 1, 20, 28960);
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = false;

    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nLoopback test authentication requires source and server bind 127.0.0.1.",
              "non-permitted source is refused by the early guard");
    Check(AllChallengesZeroed(), "the guard runs before any challenge bookkeeping");
    CheckEq(R.steamChecks, 0, "the guard runs before the Steam backend");

    // Even a fully valid authenticated client is refused while the test mode is
    // enabled but the socket/source is not permitted.
    g_steamAcceptsTicket = true;
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    CheckText(LastOob(),
              "error\nLoopback test authentication requires source and server bind 127.0.0.1.",
              "valid credentials do not bypass the guard");

    // Loopback source with the guard satisfied: test mode admits, no backend.
    netadr_t loop = MakeAdr(127, 0, 0, 1, 28960);
    g_loopbackAuthTestCanBypass = true;
    SetArgv("getchallenge");
    SV_GetChallenge(loop);
    Check(OobChallengeToken() > 0, "permitted loopback source is admitted");
    CheckEq(R.steamChecks, 0, "admitted test client does not touch the backend");

    // With test mode off, a loopback source is treated as ordinary.
    ResetWorld();
    SetArgv("getchallenge");
    SV_GetChallenge(loop);
    CheckText(LastOob(),
              "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.",
              "test mode off: loopback still needs authentication fields");
}
#endif

// ---- SV_DirectConnect ------------------------------------------------------

struct ConnectArgs
{
    const char *protocol;
    int challenge;
    int qport;
    const char *password;
};

static std::string MakeUserinfo(const ConnectArgs &args)
{
    char buffer[1024];
    if (args.password && *args.password)
        snprintf(buffer, sizeof(buffer), "protocol\\%s\\challenge\\%d\\qport\\%d\\password\\%s",
                 args.protocol, args.challenge, args.qport, args.password);
    else
        snprintf(buffer, sizeof(buffer), "protocol\\%s\\challenge\\%d\\qport\\%d",
                 args.protocol, args.challenge, args.qport);
    return std::string(buffer);
}

static bool ClientIsConnected(int index)
{
    return svs.clients[index].header.state == CS_CONNECTED;
}

static void TestDirectConnectVersionRejection()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);
    Check(token > 0, "challenge issued");

    ConnectArgs args = { "2", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "error\nThis server requires protocol version 1 (CoD4 1.0); your client uses protocol 2.\n",
              "wrong protocol version gets an error reply naming both protocols");
    CheckEq(R.netchanSetups, 0, "no netchan setup");
    Check(!ClientIsConnected(0), "no client became connected");
    Check(FindSlot(from) >= 0, "the challenge record is still present");

#if defined(__linux__) && defined(KISAK_DEDICATED)
    // The native build reports the mismatch on the always-on console channel so
    // an operator sees why a test-mode client was refused.
    Check(AnyLineContains(R.alwaysPrinted, "client protocol 2") &&
              AnyLineContains(R.alwaysPrinted, "required protocol 1"),
          "native build reports the received and required protocol on the console");
    Check(!AnyLineContains(R.debugPrinted, "rejected connect from protocol version"),
          "native build no longer reports the mismatch only on the debug channel");
#else
    Check(AnyLineContains(R.debugPrinted, "rejected connect from protocol version"),
          "non-native build keeps the developer-only protocol message");
    Check(!AnyLineContains(R.alwaysPrinted, "required protocol 1"),
          "non-native build does not add the native console line");
#endif

    // A wrong challenge token with a bad protocol still reports the version.
    ConnectArgs wrong = { "0", 12345, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(wrong).c_str());
    CheckText(LastOob(), "error\nThis server requires protocol version 1 (CoD4 1.0); your client uses protocol 0.\n",
              "version check comes before the challenge lookup");
}

// Retail CoD4 1.7 clients advertise protocol 6. sv_allowProtocol6 (registered in
// SV_Init, default off) only lets that one number through, rewriting it to 1 so
// the rest of the handshake is the protocol-1 path unchanged. It is a
// compatibility switch for the version check alone: the challenge token and the
// Steam identity attached to it are still mandatory.
static void TestDirectConnectProtocol6Admission()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // --- shipped default: protocol 6 is just another rejected version --------
    ResetWorld();
    Check(!sv_allowProtocol6->current.enabled, "sv_allowProtocol6 defaults to off");
    int token = IssueChallenge(from);
    Check(token > 0, "challenge issued");

    ConnectArgs six = { "6", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(six).c_str());
    CheckText(LastOob(), "error\nThis server requires protocol version 1 (CoD4 1.0); your client uses protocol 6.\n",
              "protocol 6 is rejected by default with the plain version message");
    Check(!AnyLineContains(R.oob, "CoD4 1.7"),
          "the disabled dvar does not advertise the 1.7 alternative");
    CheckEq(R.netchanSetups, 0, "rejected protocol 6 does not set up a netchan");
    Check(!ClientIsConnected(0), "rejected protocol 6 does not connect a client");
    Check(FindSlot(from) >= 0, "the challenge record survives the version rejection");

    // --- enabled: protocol 6 takes the protocol-1 path and connects ---------
    ResetWorld();
    dvar_allowprotocol6.current.enabled = true;
    token = IssueChallenge(from);
    Check(token > 0, "challenge issued with the dvar on");

    ConnectArgs sixOk = { "6", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(sixOk).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "protocol 6 connect is acknowledged");
    Check(ClientIsConnected(0), "protocol 6 client reached CS_CONNECTED");
    CheckText(std::string(svs.clients[0].cdkeyHash), kGoodIdentity,
              "the authenticated identity is copied onto the protocol 6 client");
    CheckEq(svs.clients[0].challenge, token, "the protocol 6 client records the accepted token");
    CheckEq(R.netchanSetups, 1, "protocol 6 sets up exactly one netchan");
    Check(R.netchanQport == 49001, "protocol 6 netchan got the client qport");
    Check(NET_CompareAdr(R.netchanAdr, from), "protocol 6 netchan got the client address");
    CheckEq(R.userinfoChanged, 1, "SV_UserinfoChanged ran for protocol 6");
    CheckEq(R.clientConnectCalls, 1, "the game was asked to accept the protocol 6 client");
    CheckEq(R.lastClientNum, 0, "the game was told client number 0");
    Check(R.lastScriptPersId != 0, "the protocol 6 client was given an allocated script id");
    CheckEq(R.heartbeats, 1, "the first protocol 6 client triggers a heartbeat");
    CheckText(Info_ValueForKey(svs.clients[0].userinfo, "protocol"), "6",
              "the client's own protocol string stays in its userinfo");

    // --- enabled: no other version is admitted, and the reply says so -------
    {
        const char *const rejected[] = { "21", "7", "2", "0" };
        for (size_t v = 0; v < sizeof(rejected) / sizeof(rejected[0]); ++v)
        {
            netadr_t peer = MakeAdr(203, 0, 113, 8, static_cast<unsigned short>(28100 + v));
            ResetWorld();
            dvar_allowprotocol6.current.enabled = true;
            int fresh = IssueChallenge(peer);
            Check(fresh > 0, std::string("challenge issued for protocol ") + rejected[v]);

            ConnectArgs other = { rejected[v], fresh, 49001, "" };
            ConnectAttempt(peer, MakeUserinfo(other).c_str());
            CheckText(LastOob(),
                      std::string("error\nThis server requires protocol version 1 (CoD4 1.0)"
                                  " or 6 (CoD4 1.7); your client uses protocol ") +
                          rejected[v] + ".\n",
                      std::string("protocol ") + rejected[v] + " is still rejected");
            CheckEq(R.netchanSetups, 0, std::string("protocol ") + rejected[v] + " sets up no netchan");
            Check(!ClientIsConnected(0), std::string("protocol ") + rejected[v] + " connects no client");
        }
    }

    // --- enabled: the challenge token is still required ---------------------
    ResetWorld();
    dvar_allowprotocol6.current.enabled = true;
    token = IssueChallenge(from);
    Check(token > 0, "challenge issued for the token test");
    ConnectArgs staleToken = { "6", token + 1, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(staleToken).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "protocol 6 with a wrong challenge token is rejected");
    CheckEq(R.netchanSetups, 0, "a wrong token sets up no netchan even for protocol 6");
    Check(!ClientIsConnected(0), "a wrong token connects no protocol 6 client");

    ConnectArgs noToken = { "6", 0, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(noToken).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "protocol 6 without ever asking for a challenge is rejected");

    // --- enabled: authentication is not bypassed ----------------------------
    // The backend rejects the ticket, so no challenge (and therefore no
    // identity) is ever issued; the protocol-6 client cannot connect anyway.
    ResetWorld();
    dvar_allowprotocol6.current.enabled = true;
    g_steamAcceptsTicket = false;
    SetArgv("getchallenge", "", kGoodTicket, kGoodIdentity);
    SV_GetChallenge(from);
    Check(OobChallengeToken() < 0, "a rejected ticket issues no challenge");
    ConnectArgs unauth = { "6", 12345, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(unauth).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "protocol 6 cannot connect without an authenticated challenge");
    CheckEq(R.netchanSetups, 0, "an unauthenticated protocol 6 client gets no netchan");
    Check(!ClientIsConnected(0), "an unauthenticated protocol 6 client is not connected");
}

static void TestDirectConnectBadChallenge()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);
    Check(token > 0, "challenge issued");

    ConnectArgs wrongToken = { "1", token + 1, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(wrongToken).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "wrong token rejected");
    CheckEq(R.netchanSetups, 0, "wrong token does not set up a netchan");

    // Right token, different source port: the challenge is address-scoped.
    netadr_t other = MakeAdr(203, 0, 113, 7, 28001);
    ConnectArgs borrowed = { "1", token, 49002, "" };
    ConnectAttempt(other, MakeUserinfo(borrowed).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "challenge from another source port rejected");
    CheckEq(R.netchanSetups, 0, "wrong source does not set up a netchan");

    // Right token, different address: rejected too.
    netadr_t elsewhere = MakeAdr(198, 51, 100, 9, 28000);
    ConnectAttempt(elsewhere, MakeUserinfo(borrowed).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "challenge from another address rejected");
    Check(!ClientIsConnected(0), "no client became connected");
}

static void TestDirectConnectSuccess()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);
    Check(token > 0, "challenge issued");

    ConnectArgs args = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "successful connect is acknowledged");
    Check(ClientIsConnected(0), "client 0 reached CS_CONNECTED");
    CheckText(std::string(svs.clients[0].cdkeyHash), kGoodIdentity,
              "the authenticated identity is copied onto the client");
    CheckEq(svs.clients[0].challenge, token, "the client records the accepted token");
    CheckEq(R.netchanSetups, 1, "netchan was set up once");
    Check(R.netchanQport == 49001, "netchan got the client qport");
    Check(NET_CompareAdr(R.netchanAdr, from), "netchan got the client address");
    CheckEq(R.userinfoChanged, 1, "SV_UserinfoChanged ran");
    CheckEq(R.heartbeats, 1, "the first client triggers a heartbeat");
    CheckEq(R.clientConnectCalls, 1, "the game was asked to accept the client");
    CheckEq(R.lastClientNum, 0, "the game was told client number 0");
    Check(R.lastScriptPersId != 0, "the game was given an allocated script id");
    CheckEq(R.freeClientScriptId, 0, "nothing was freed");
}

#if defined(__linux__) && defined(KISAK_DEDICATED)
static void TestDirectConnectTestModeIdentityRules()
{
    netadr_t loop = MakeAdr(127, 0, 0, 1, 28960);
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = true;

    int token = IssueChallenge(loop);
    Check(token > 0, "test mode issued a challenge");
    std::string synthetic = SlotIdentity(FindSlot(loop));
    Check(synthetic.compare(0, 11, "local-test-") == 0, "the stored identity is synthetic");

    ConnectArgs args = { "1", token, 28961, "" };
    ConnectAttempt(loop, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak",
              "a server-issued synthetic identity is accepted in test mode");
    Check(ClientIsConnected(0), "the test client reached CS_CONNECTED");
    CheckText(std::string(svs.clients[0].cdkeyHash), synthetic,
              "the synthetic identity is copied onto the client");
    CheckEq(R.steamChecks, 0, "no Steam backend call for the test client");

    // Test mode on, but the challenge record holds a claimed Steam ID: that
    // record cannot have come from the bypass, so it is refused.
    ResetWorld();
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = true;
    svs.challenges[0].adr = loop;
    svs.challenges[0].challenge = 0x1234;
    I_strncpyz(svs.challenges[0].cdkeyHash, kGoodIdentity, 33);
    ConnectArgs forced = { "1", 0x1234, 28961, "" };
    ConnectAttempt(loop, MakeUserinfo(forced).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "test mode refuses a numerically claimed Steam ID");
    Check(!ClientIsConnected(0), "the refused client never connects");
    CheckEq(R.netchanSetups, 0, "refused before any connection setup");

    // Test mode off, synthetic identity in the record: refused.
    ResetWorld();
    svs.challenges[0].adr = loop;
    svs.challenges[0].challenge = 0x1234;
    I_strncpyz(svs.challenges[0].cdkeyHash, "local-test-00001234abcd", 33);
    ConnectAttempt(loop, MakeUserinfo(forced).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "default build refuses a synthetic identity");
    Check(!ClientIsConnected(0), "the synthetic identity never connects by default");
    CheckEq(R.netchanSetups, 0, "refused before any connection setup");
}

static void TestDirectConnectGuardOrdering()
{
    netadr_t from = MakeAdr(192, 168, 1, 20, 28960);
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = false;

    // An invalid protocol number must not be reported: the guard runs first and
    // the handler returns before touching any client state.
    ConnectArgs args = { "2", 1234, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(args).c_str());
    CheckText(LastOob(),
              "error\nLoopback test authentication requires source and server bind 127.0.0.1.",
              "the direct-connect guard precedes the protocol check");
    CheckEq(R.netchanSetups, 0, "guard rejects before connection setup");
    CheckEq(R.clientConnectCalls, 0, "guard rejects before the game sees the client");
    Check(svs.clients[0].header.state == CS_FREE, "no client slot changed state");

    netadr_t loop = MakeAdr(127, 0, 0, 1, 28960);
    g_loopbackAuthTestCanBypass = true;
    int token = IssueChallenge(loop);
    Check(token > 0, "permitted source can still obtain a challenge");
    ConnectArgs good = { "1", token, 49001, "" };
    ConnectAttempt(loop, MakeUserinfo(good).c_str());
    Check(OobChallengeToken() == -1 && LastOob() == "connectResponse mods/kisak",
          "permitted source still connects");
}
#endif

static void TestDirectConnectCapacityAndPassword()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    dvar_maxclients.current.integer = 1;
    svs.clients[0].header.state = CS_CONNECTED;
    svs.clients[0].header.netchan.remoteAddress = MakeAdr(198, 51, 100, 9, 27960);
    svs.clients[0].header.netchan.qport = 27961;

    int token = IssueChallenge(from);
    Check(token > 0, "challenge issued");
    ConnectArgs args = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "error\nEXE_SERVERISFULL", "a full server rejects the connection");
    CheckEq(R.netchanSetups, 0, "a full server does not set up a netchan");
    Check(!ClientIsConnected(1), "no second client slot exists");

    // Missing keys must not borrow the final unrelated field's value.
    const char* incomplete = "\\qport\\49001";
    CheckText(Info_ValueForKey(incomplete, "protocol"), "", "absent protocol is empty");
    CheckText(Info_ValueForKey(incomplete, "challenge"), "", "absent challenge is empty");
    CheckText(Info_ValueForKey("\\protocol\\1", "qport"), "", "absent qport is empty");
    CheckText(Info_ValueForKey(incomplete, "password"), "", "absent password is empty");

    // Private slots: a wrong password skips the private range, the right one
    // allows it. Deliberately match the private password to the last unrelated
    // field so a missing-key double returning that field cannot hide the bug.
    ResetWorld();
    dvar_maxclients.current.integer = 2;
    dvar_privateclients.current.integer = 1;
    dvar_privatepassword.current.string = "49001";

    token = IssueChallenge(from);
    ConnectArgs noPassword = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(noPassword).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "a normal client still connects");
    Check(!ClientIsConnected(0), "a normal client does not take the private slot");
    Check(ClientIsConnected(1), "a normal client takes the first public slot");

    ResetWorld();
    dvar_maxclients.current.integer = 2;
    dvar_privateclients.current.integer = 1;
    dvar_privatepassword.current.string = "secret";
    token = IssueChallenge(from);
    ConnectArgs withPassword = { "1", token, 49001, "secret" };
    ConnectAttempt(from, MakeUserinfo(withPassword).c_str());
    Check(ClientIsConnected(0), "the private password still selects the private range");
}

static void TestDirectConnectGameRejection()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    int token = IssueChallenge(from);
    g_clientConnectDenial = "EXE_SCRIPTNOTFOUND";

    ConnectArgs args = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "error\nEXE_SCRIPTNOTFOUND", "the game's denial is forwarded to the client");
    Check(!ClientIsConnected(0), "a denied client stays free");
    CheckEq(R.freeClientScriptId, 1, "the allocated script id is released");
}

// The reconnect throttle is keyed on a client slot, so it must only apply to a
// slot that actually completed a connection. A connect the game rejects leaves
// the slot CS_FREE, but Netchan_Setup() has already stored this client's address
// and qport on it, and lastConnectTime is only written on success. Matching that
// slot would throttle a free slot, and because the rejection is a developer-only
// print the client waits forever for a reply it never gets instead of retrying.
// A rejected password followed by a corrected retry is the common case, and on a
// server younger than the limit every retry lands inside it.
//
// ResetWorld() leaves sv_reconnectlimit at 0, which disables the throttle, so
// unrelated tests never depend on connect timing. This test raises it to the
// shipped default of 3 seconds, which is also the timing the original bug needed.
static void TestDirectConnectReconnectLimit()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    const int limitMs = 3 * 1000;
    int token;

    // --- the slot a rejected connect leaves behind does not throttle ---------
    ResetWorld();
    dvar_reconnectlimit.current.integer = 3;
    Check(svs.time < limitMs, "the server is younger than the reconnect limit");
    token = IssueChallenge(from);
    Check(token > 0, "challenge issued");

    g_clientConnectDenial = "GAME_INVALIDPASSWORD";
    ConnectArgs badPassword = { "1", token, 49001, "wrong" };
    ConnectAttempt(from, MakeUserinfo(badPassword).c_str());
    CheckText(LastOob(), "error\nGAME_INVALIDPASSWORD", "the rejected password is reported to the client");
    Check(!ClientIsConnected(0), "a game-rejected client leaves the slot free");
    Check(svs.clients[0].header.state == CS_FREE, "the slot is CS_FREE after the rejection");
    CheckEq(svs.clients[0].header.netchan.qport, 49001,
            "the rejected slot still holds the client's qport");
    Check(NET_CompareAdr(svs.clients[0].header.netchan.remoteAddress, from),
          "the rejected slot still holds the client's address");
    CheckEq(svs.clients[0].lastConnectTime, 0, "a rejected connect never sets lastConnectTime");

    // The client corrects its password and retries, well inside the limit.
    int connectsBefore = R.clientConnectCalls;
    g_clientConnectDenial = NULL;
    token = IssueChallenge(from);
    Check(token > 0, "the retry obtains a challenge");
    R.oob.clear();
    R.debugPrinted.clear();
    ConnectArgs rightPassword = { "1", token, 49001, "right" };
    ConnectAttempt(from, MakeUserinfo(rightPassword).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "the corrected retry is admitted");
    Check(ClientIsConnected(0), "the corrected retry reached CS_CONNECTED");
    Check(!AnyLineContains(R.debugPrinted, "reconnect rejected"),
          "a free slot never reports a reconnect rejection");
    CheckEq(R.clientConnectCalls, connectsBefore + 1, "the game was asked about the retry");

    // --- a client that completed a connection is still throttled ------------
    ResetWorld();
    dvar_reconnectlimit.current.integer = 3;
    token = IssueChallenge(from);
    Check(token > 0, "challenge issued for the first connect");
    ConnectArgs firstConnect = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(firstConnect).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "the first connect is admitted");
    Check(ClientIsConnected(0), "the first client is connected");
    CheckEq(svs.clients[0].lastConnectTime, svs.time, "a completed connection records its time");
    Check(svs.time - svs.clients[0].lastConnectTime < limitMs,
          "the reconnect below happens inside the limit");

    token = IssueChallenge(from);
    Check(token > 0, "the reconnect obtains a challenge");
    connectsBefore = R.clientConnectCalls;
    int netchansBefore = R.netchanSetups;
    R.oob.clear();
    R.debugPrinted.clear();
    ConnectArgs tooSoon = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(tooSoon).c_str());
    Check(R.oob.empty(), "the too-soon rejection sends the client no reply");
    Check(!AnyLineContains(R.oob, "error"),
          "the too-soon rejection never aborts the client's connection attempt");
    Check(AnyLineContains(R.debugPrinted, "reconnect rejected : too soon"),
          "the developer channel still reports the rejection");
    CheckEq(R.netchanSetups, netchansBefore, "the throttled reconnect set up no netchan");
    CheckEq(R.clientConnectCalls, connectsBefore, "the game was not asked about the throttled reconnect");
    Check(ClientIsConnected(0), "the connected client is left untouched");
    CheckEq(R.freeClient, 0, "the connected client was not freed");

    // --- after the limit the existing reconnect path takes over -------------
    svs.time += limitMs;
    Check(svs.time - svs.clients[0].lastConnectTime >= limitMs,
          "the reconnect below happens at the edge of the limit");
    token = IssueChallenge(from);
    Check(token > 0, "the late reconnect obtains a challenge");
    R.oob.clear();
    R.debugPrinted.clear();
    ConnectAttempt(from, MakeUserinfo(tooSoon).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "the reconnect after the limit is admitted");
    Check(AnyLineContains(R.alwaysPrinted, "reconnect. same qport: 1, same port: 1"),
          "the existing reconnect path matched the slot on both qport and port");
    Check(!AnyLineContains(R.debugPrinted, "reconnect rejected"),
          "no rejection is reported once the limit has passed");
    CheckEq(R.freeClient, 1, "the reconnect path freed the previous client");
    CheckEq(R.lastClientNum, 0, "the reconnecting client kept its slot number");
    Check(ClientIsConnected(0), "the reconnecting client occupies the same slot");
    CheckEq(R.netchanSetups, netchansBefore + 1, "the reconnect set up one new netchan");

    // --- a slot the server moved back to CS_FREE does not throttle ----------
    // SV_CheckTimeouts() expires a zombie into CS_FREE and keeps its
    // lastConnectTime, so a free slot can carry a recent connect time. It is
    // reusable again: the throttle must key on the state, not on that timestamp.
    ResetWorld();
    dvar_reconnectlimit.current.integer = 3;
    svs.clients[0].header.state = CS_FREE;
    svs.clients[0].header.netchan.remoteAddress = from;
    svs.clients[0].header.netchan.qport = 49001;
    svs.clients[0].lastConnectTime = svs.time;
    token = IssueChallenge(from);
    Check(token > 0, "challenge issued for the freed slot");
    R.oob.clear();
    R.debugPrinted.clear();
    ConnectArgs freedSlot = { "1", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(freedSlot).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "a freed slot does not throttle the next connect");
    Check(ClientIsConnected(0), "the client took the freed slot");
    Check(!AnyLineContains(R.debugPrinted, "reconnect rejected"),
          "a stale connect time on a free slot throttles nothing");
}

static void TestDirectConnectLoopbackSource()
{
    ConnectArgs args = { "1", 4242, 49001, "" };

    // NET_IsLocalAddress() is NA_LOOPBACK/NA_BOT only. A 127.0.0.1 UDP source is
    // an ordinary IP client and must present a challenge token.
    netadr_t ipLoopback = MakeAdr(127, 0, 0, 1, 28960);
    ConnectAttempt(ipLoopback, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE",
              "a 127.0.0.1 UDP source still needs a challenge token");
    Check(!ClientIsConnected(0), "the unauthenticated loopback client is refused");
    CheckEq(R.steamChecks, 0, "no Steam backend call for the refused client");

    // An NA_LOOPBACK source skips the challenge lookup entirely, so a
    // listen-server client connects with no identity at all.
    ResetWorld();
    netadr_t loop = MakeLoopbackAdr();
    ConnectAttempt(loop, MakeUserinfo(args).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "NA_LOOPBACK source connects without a challenge");
    Check(ClientIsConnected(0), "the loopback client is connected");
    Check(svs.clients[0].cdkeyHash[0] == 0, "the loopback client has no identity");
    CheckEq(R.steamChecks, 0, "no Steam backend call for a loopback client");
}

// ---- SV_DropClient ---------------------------------------------------------

static client_t *PrepareDropClient(const char *identity, int state)
{
    memset(&svs.clients[0], 0, sizeof(client_t));
    svs.clients[0].header.state = state;
    svs.clients[0].header.netchan.remoteAddress = MakeAdr(203, 0, 113, 7, 28000);
    svs.clients[0].header.netchan.qport = 49001;
    I_strncpyz(svs.clients[0].name, "player", sizeof(svs.clients[0].name));
    svs.clients[0].gentity = NULL;
    if (identity)
        I_strncpyz(svs.clients[0].cdkeyHash, identity, sizeof(svs.clients[0].cdkeyHash));
    return &svs.clients[0];
}

static void TestDropClientSteamNotification()
{
    client_t *cl = PrepareDropClient(kGoodIdentity, CS_CONNECTED);
    dvar_dedicated.current.integer = 1;

    SV_DropClient(cl, "EXE_DISCONNECTED", false);
    CheckEq(static_cast<long long>(R.steamDropped.size()), 1, "one Steam backend notification");
    Check(R.steamDropped.size() == 1 && R.steamDropped[0] == "76561197960287930",
          "the backend was told the original numeric Steam ID");
    Check(svs.clients[0].header.state == CS_ZOMBIE, "the client went to CS_ZOMBIE");
    CheckEq(R.freeClient, 1, "the client slot was freed");
    CheckEq(R.sendDisconnects, 0, "tellThem false sends no disconnect message");

    // Ordering: the Steam notification happens before the client is torn down.
    bool steamFirst = false;
    if (R.events.size() >= 2)
        steamFirst = R.events[0].compare(0, 13, "steam_dropped") == 0 &&
                     R.events[1] == "free_client";
    Check(steamFirst, "the Steam notification precedes the slot teardown");

    // Reason forwarding: a kick reason reaches the other clients.
    ResetWorld();
    cl = PrepareDropClient(kGoodIdentity, CS_ACTIVE);
    SV_DropClient(cl, "EXE_PLAYERKICKED", true);
    CheckEq(static_cast<long long>(R.serverCommands.size()), 2, "two server commands for a kicked client");
    Check(R.serverCommands.size() >= 1 &&
              R.serverCommands[0].find("EXE_PLAYERKICKED") != std::string::npos,
          "the kick reason is forwarded to the other clients");
    CheckEq(R.sendDisconnects, 1, "tellThem true sends a disconnect message");
    CheckText(R.lastDisconnectReason, "EXE_PLAYERKICKED", "the disconnect carries the reason");

    // Dedicated off: the backend is never contacted.
    ResetWorld();
    cl = PrepareDropClient(kGoodIdentity, CS_CONNECTED);
    dvar_dedicated.current.integer = 0;
    SV_DropClient(cl, "EXE_DISCONNECTED", false);
    CheckEq(static_cast<long long>(R.steamDropped.size()), 0,
            "a listen server has no backend to notify");
    Check(svs.clients[0].header.state == CS_ZOMBIE, "the client is dropped anyway");
}

static void TestDropClientIdentityClassification()
{
    struct Case
    {
        const char *identity;
        bool backendCalled;
        const char *what;
    };
    const Case cases[] = {
        { "76561197960287930", true, "ordinary numeric identity" },
        { "1", true, "smallest valid identity" },
        { "18446744073709551615", true, "largest valid identity" },
        { "local-test-00001234abcd", false, "synthetic local test identity" },
        { "local-test-", false, "bare local test prefix" },
        { "", false, "empty identity" },
        { "0", false, "zero identity" },
        { "-12", false, "negative identity" },
        { "12 3", false, "identity with a space" },
        { "18446744073709551616", false, "identity overflowing uint64" },
        { "steam_76561197960287930", false, "prefixed identity" },
        // Retail GUIDs are never Steam identities, including the all-digit
        // spelling that looks numeric, so the backend must not hear about them.
        { "9fa4b618b026886e7fe966bb184ae770", false, "retail GUID" },
        { "9FA4B618B026886E7FE966BB184AE770", false, "uppercase retail GUID" },
        { "12345678901234567890123456789012", false, "all-digit retail GUID" },
        { "00000000000000000000000000000000", false, "all-zero retail GUID" },
        { "ffffffffffffffffffffffffffffffff", false, "all-f retail GUID" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        ResetWorld();
        client_t *cl = PrepareDropClient(cases[i].identity, CS_CONNECTED);
        dvar_dedicated.current.integer = 1;
        SV_DropClient(cl, "EXE_DISCONNECTED", false);
        std::string label = std::string("drop with identity \"") + Escape(cases[i].identity) + "\" (" +
                            cases[i].what + ")";
        CheckEq(static_cast<long long>(R.steamDropped.size()),
                cases[i].backendCalled ? 1 : 0, label + ": backend notifications");
        Check(svs.clients[0].header.state == CS_ZOMBIE, label + ": client still dropped");
        Check(g_asserts == 0, label + ": no assert");
    }

    // Sanity: the values the backend receives are the identities themselves.
    ResetWorld();
    client_t *cl = PrepareDropClient("18446744073709551615", CS_CONNECTED);
    SV_DropClient(cl, "EXE_DISCONNECTED", false);
    Check(R.steamDropped.size() == 1 && R.steamDropped[0] == "18446744073709551615",
          "the backend receives the full uint64 range");
}

static void TestDropClientZombie()
{
    client_t *cl = PrepareDropClient(kGoodIdentity, CS_ZOMBIE);
    cl->dropReason = NULL;
    svs.clients[0].gentity = &g_gentities[0];
    SV_DropClient(cl, "EXE_DISCONNECTED", false);
    Check(svs.clients[0].header.state == CS_ZOMBIE, "a zombie client stays a zombie");
    CheckEq(R.freeClient, 0, "a zombie client is not freed twice");
    CheckEq(static_cast<long long>(R.steamDropped.size()), 1,
            "the backend notification still happens once");
}

// ---- unverified retail GUIDs ----------------------------------------------

// The canonical retail CoD4 1.7 GUID observed from a real client, plus the same
// value in uppercase and mixed-case spellings.
static const char *const kRetailGuid = "9fa4b618b026886e7fe966bb184ae770";
static const char *const kRetailGuidUpper = "9FA4B618B026886E7FE966BB184AE770";
static const char *const kRetailGuidMixed = "9Fa4B618b026886e7fE966Bb184ae770";

static const char *const kMissingFieldsMessage =
    "error\nThis server requires the custom Steam ticket/ID handshake; no authentication fields were supplied.";
static const char *const kMalformedGuidMessage = "error\nMalformed GUID authentication fields.";
static const char *const kGuidPolicyMessage =
    "error\nThis server requires Steam authentication; unverified retail GUIDs are not accepted.";
static const char *const kUnsupportedHandshakeMessage = "error\nUnsupported authentication handshake.";

// Turns the opt-in on exactly the way SV_Init() does: set the dvar, capture once.
static void EnableUnverifiedGuids()
{
    dvar_allowunverifiedguids.current.enabled = true;
    SV_CaptureUnverifiedGuids();
}

// A retail request: getchallenge <clientChallenge> "<32 hex GUID>" (argc 3).
static int IssueRetailChallenge(netadr_t from, const char *guid)
{
    SetArgv("getchallenge", "0", guid, std::string(), 3);
    SV_GetChallenge(from);
    return OobChallengeToken();
}

// Places a challenge record directly, the way SV_GetChallenge would have, so the
// SV_DirectConnect identity policy can be exercised on its own.
static int SeedChallenge(netadr_t from, const char *identity, int token)
{
    for (int i = 0; i < 1024; ++i)
    {
        challenge_t &c = svs.challenges[i];
        if (c.adr.type == NA_BOT && c.adr.port == 0 && c.challenge == 0)
        {
            c.adr = from;
            c.challenge = token;
            c.time = svs.time;
            c.pingTime = svs.time;
            I_strncpyz(c.cdkeyHash, identity, sizeof(c.cdkeyHash));
            return i;
        }
    }
    return -1;
}

// ---- SV_ParseRetailGuid ----------------------------------------------------

struct GuidCase
{
    const char *text;
    bool ok;
};

static const GuidCase kGuidCases[] = {
    { "9fa4b618b026886e7fe966bb184ae770", true },
    { "9FA4B618B026886E7FE966BB184AE770", true },  // accepted, normalised
    { "9Fa4B618b026886e7fE966Bb184ae770", true },
    { "12345678901234567890123456789012", true },  // all digits
    { "00000000000000000000000000000000", true },  // all zero
    { "ffffffffffffffffffffffffffffffff", true },
    { "abcdef0123456789abcdef0123456789", true },
    { "9fa4b618b026886e7fe966bb184ae77", false },            // 31 characters
    { "9fa4b618b026886e7fe966bb184ae7700", false },          // 33 characters
    { "", false },                                           // empty
    { "9fa4b618b026886e7fe966bb184ae770 ", false },          // trailing space
    { " 9fa4b618b026886e7fe966bb184ae770", false },          // leading space
    { "9fa4b618b026886e7fe966bb184ae77\n", false },          // newline
    { "9fa4b618b02688\n7fe966bb184ae770", false },           // embedded whitespace
    { "9fa4b618b026886e7fe966bb184ae77g", false },           // non-hex letter
    { "9fa4b618b026-86e7fe966bb184ae770", false },           // separator
    { "+9fa4b618b026886e7fe966bb184ae77", false },           // sign
    { "-9fa4b618b026886e7fe966bb184ae77", false },           // sign
    { "0x9fa4b618b026886e7fe966bb184ae7", false },           // 0x prefix, 32 chars
    { "local-test-000012340000", false },                    // synthetic namespace
    { "9fa4b618b026886e7fe966bb184ae77\331\241", false },    // 32 hex + non-ASCII
    { "\331\2419fa4b618b026886e7fe966bb184", false },        // non-ASCII first bytes
};

static void TestParseRetailGuid()
{
    for (size_t i = 0; i < sizeof(kGuidCases) / sizeof(kGuidCases[0]); ++i)
    {
        const GuidCase &c = kGuidCases[i];
        char out[33];
        memset(out, 'X', sizeof(out));            // sentinel, deliberately not zero
        bool ok = SV_ParseRetailGuid(c.text, out);
        std::string label = std::string("SV_ParseRetailGuid(\"") + Escape(c.text) + "\")";
        Check(ok == c.ok, label + (c.ok ? " accepted" : " rejected"));
        if (c.ok)
        {
            // Exactly 32 lowercase hex digits followed by a NUL.
            std::string want;
            for (size_t j = 0; j < 32; ++j)
            {
                char ch = c.text[j];
                if (ch >= 'A' && ch <= 'F')
                    ch = static_cast<char>(ch - 'A' + 'a');
                want.push_back(ch);
            }
            CheckText(std::string(out, 32), want, label + " written to lowercase");
            CheckEq(static_cast<long long>(strlen(out)), 32, label + " output length");
            CheckEq(static_cast<int>(out[32]), 0, label + " output is NUL-terminated at byte 32");
        }
        else
        {
            Check(out[0] == 'X' && out[32] == 'X', label + " leaves the output buffer untouched");
        }
    }

    char out[33] = {};
    Check(!SV_ParseRetailGuid(NULL, out), "SV_ParseRetailGuid(NULL, out) rejected");
    Check(!SV_ParseRetailGuid(kRetailGuid, NULL), "SV_ParseRetailGuid(text, NULL) rejected");

    // A GUID is not a Steam identity, and the two parsers never overlap.
    uint64_t steamId = 0;
    Check(!SV_ParseSteamIdentity(kRetailGuid, &steamId), "a retail GUID is not a Steam identity");
    Check(!SV_ParseSteamIdentity("12345678901234567890123456789012", &steamId),
          "an all-digit retail GUID is too long to be a Steam identity");
}

// ---- sv_allowUnverifiedGuids capture --------------------------------------

static void TestUnverifiedGuidsCapture()
{
    // Shipped default: off, silent.
    Check(!SV_UnverifiedGuidsEnabled(), "the shipped default does not accept unverified GUIDs");
    SV_CaptureUnverifiedGuids();
    Check(!SV_UnverifiedGuidsEnabled(), "capturing the default leaves it off");
    Check(!AnyLineContains(R.alwaysPrinted, "sv_allowUnverifiedGuids is enabled"),
          "the default configuration prints no warning");

    // Enabled at start-up: the decision, the verbatim warning, printed once.
    ResetWorld();
    EnableUnverifiedGuids();
    Check(SV_UnverifiedGuidsEnabled(), "an enabled start-up captures permission");
    const std::string expectedWarning =
        "WARNING: sv_allowUnverifiedGuids is enabled. Retail GUIDs are accepted WITHOUT verification from any "
        "address. Players can impersonate GUIDs and evade GUID bans; do not trust GUID-based privileges or "
        "persistent identity. Steam-format clients still require Steam authentication. Protocol, challenge and "
        "password checks remain enabled. This setting requires a full process restart to change.\n";
    Check(AnyLineContains(R.alwaysPrinted, expectedWarning.c_str()), "the warning is printed verbatim");
    int warnings = 0;
    for (size_t i = 0; i < R.alwaysPrinted.size(); ++i)
        if (R.alwaysPrinted[i].find("sv_allowUnverifiedGuids is enabled") != std::string::npos)
            ++warnings;
    CheckEq(warnings, 1, "the warning is printed exactly once");

    // The capture is frozen. Turning the dvar off revokes acceptance at once...
    dvar_allowunverifiedguids.current.enabled = false;
    Check(!SV_UnverifiedGuidsEnabled(), "clearing the dvar revokes acceptance immediately");
    // ...and a later capture call (a second SV_Init) does not adopt the new value.
    SV_CaptureUnverifiedGuids();
    Check(!SV_UnverifiedGuidsEnabled(), "a later capture call changes nothing");
    dvar_allowunverifiedguids.current.enabled = true;
    Check(SV_UnverifiedGuidsEnabled(), "the frozen capture survives the dvar being rewritten");
    CheckEq(warnings, 1, "no second warning was printed");

    // A dvar turned on after start-up grants nothing: only the capture grants.
    ResetWorld();
    dvar_allowunverifiedguids.current.enabled = true;
    Check(!SV_UnverifiedGuidsEnabled(), "turning the dvar on after start-up grants nothing");
}

// ---- sv_advertisedProtocol capture ----------------------------------------

// The production setdvar script function reaches Dvar_SetFromStringByName(),
// which is DVAR_SOURCE_INTERNAL. DVAR_INIT and DVAR_ROM refuse external and
// script input but not this, so an internal write can change either dvar at any
// point after the capture has run.
static void InternalSetAdvertisedProtocol(int value)
{
    Dvar_SetInt((dvar_s *)sv_advertisedProtocol, value);
}

static void TestAdvertisedProtocolCapture()
{
    const char *valueWarning = "sv_advertisedProtocol must be 1 or 6";

    // Shipped default: protocol 1, silent, and no write-back to the dvar.
    CheckEq(SV_GetAdvertisedProtocol(), 1, "before the capture the accessor reports the built-in 1");
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 1, "the shipped default captures protocol 1");
    Check(!AnyLineContains(R.alwaysPrinted, valueWarning), "a default start prints no value warning");
    CheckEq(static_cast<long long>(R.dvarSetIntValues.size()), 0,
            "a valid default needs no write-back to the dvar");

    // 6 is the other supported value and is captured unchanged.
    ResetWorld();
    dvar_advertisedprotocol.current.integer = 6;
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 6, "6 is captured as 6");
    Check(!AnyLineContains(R.alwaysPrinted, valueWarning), "6 is not an unsupported value");
    CheckEq(static_cast<long long>(R.dvarSetIntValues.size()), 0, "6 needs no write-back either");

    // An unsupported value warns, advertises the safe 1, and corrects the dvar.
    ResetWorld();
    dvar_advertisedprotocol.current.integer = 9;
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 1, "an unsupported value advertises 1");
    CheckEq(static_cast<long long>(dvar_advertisedprotocol.current.integer), 1,
            "the fail-safe reset reaches the dvar through an internal write");
    CheckEq(static_cast<long long>(R.dvarSetIntValues.size()), 1,
            "exactly one internal write corrected the dvar");
    if (!R.dvarSetIntValues.empty())
        CheckEq(R.dvarSetIntValues[0], 1, "the corrective write stored 1");
    Check(AnyLineContains(R.alwaysPrinted, valueWarning), "the value warning is printed");

    // ...and only once, however often the engine re-runs the capture.
    SV_CaptureAdvertisedProtocol();
    SV_CaptureAdvertisedProtocol();
    int warnings = 0;
    for (size_t i = 0; i < R.alwaysPrinted.size(); ++i)
        if (R.alwaysPrinted[i].find(valueWarning) != std::string::npos)
            ++warnings;
    CheckEq(warnings, 1, "the value warning is printed exactly once");
    CheckEq(static_cast<long long>(R.dvarSetIntValues.size()), 1,
            "a repeated capture performs no further write");

    // ---- immutability under internal writes to BOTH dvars ------------------
    // A frozen capture must ignore internal writes both to the dvar it was taken
    // from and to the "protocol" serverinfo dvar that SV_Init seeded from it -
    // the pair the two browse replies used to read separately.
    ResetWorld();
    dvar_advertisedprotocol.current.integer = 6;
    SV_CaptureAdvertisedProtocol();
    dvar_protocolinfo.current.integer = 6;              // SV_Init's own seeding
    CheckEq(SV_GetAdvertisedProtocol(), 6, "the frozen 6 is reported");

    InternalSetAdvertisedProtocol(1);                   // setdvar("sv_advertisedProtocol","1")
    CheckEq(SV_GetAdvertisedProtocol(), 6, "an internal write to sv_advertisedProtocol changes nothing");
    InternalSetAdvertisedProtocol(9);                   // setdvar("sv_advertisedProtocol","9")
    CheckEq(SV_GetAdvertisedProtocol(), 6, "an internal write to an unsupported value changes nothing");
    Dvar_SetInt((dvar_s *)protocolInfo, 9);             // setdvar("protocol","9")
    CheckEq(SV_GetAdvertisedProtocol(), 6, "an internal write to the ROM protocol dvar changes nothing");
    Dvar_SetInt((dvar_s *)protocolInfo, 1);             // and back again
    CheckEq(SV_GetAdvertisedProtocol(), 6, "the frozen value survives it in both directions");

    // Nothing re-runs the capture today: SV_Init runs once, and a map change or a
    // net_restart never calls it. Call it again anyway to pin the hypothetical - a
    // second capture must not adopt any of the writes above.
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 6, "a repeated capture keeps the start-up answer");

    // The published key, not just the accessor: SV_SetAdvertisedProtocolInfo() is
    // the one helper both browse replies call, so a stale "protocol" pair is
    // replaced by the captured value while the dvars hold anything at all.
    CheckEq(static_cast<long long>(dvar_advertisedprotocol.current.integer), 9,
            "the dvar really holds the written 9 before publishing");
    CheckEq(static_cast<long long>(dvar_protocolinfo.current.integer), 1,
            "the protocol dvar really holds the written 1 before publishing");
    char info[1028];
    std::strcpy(info, "\\protocol\\9\\hostname\\probe\\mapname\\mp_crash");
    SV_SetAdvertisedProtocolInfo(info);
    Check(std::string(info) == "\\hostname\\probe\\mapname\\mp_crash\\protocol\\6",
          "the reply reports the captured 6 and leaves the other keys alone");
    Check(std::strstr(info, "\\protocol\\9") == NULL, "the stale protocol pair is replaced");

    // The mirror case: a start that captured 1 is never upgraded to 6.
    ResetWorld();
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 1, "a default start captures 1");
    InternalSetAdvertisedProtocol(6);
    Dvar_SetInt((dvar_s *)protocolInfo, 6);
    SV_CaptureAdvertisedProtocol();
    CheckEq(SV_GetAdvertisedProtocol(), 1, "a frozen 1 is never upgraded to 6 by a later write");
    CheckEq(static_cast<long long>(R.dvarSetIntValues.size()), 2,
            "only the two writes made by this test were performed");
}

// ---- SV_GetChallenge: retail GUIDs, switch off ----------------------------

static void TestGetChallengeRetailGuidDisabled()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    Check(!SV_UnverifiedGuidsEnabled(), "the shipped default refuses unverified GUIDs");
    int token = IssueRetailChallenge(from, kRetailGuid);
    CheckText(LastOob(), kGuidPolicyMessage, "a retail GUID is refused with the policy message");
    CheckEq(static_cast<long long>(token), -1, "no challenge token was issued");
    Check(AllChallengesZeroed(), "no challenge slot was allocated");
    CheckEq(R.steamChecks, 0, "a refused retail GUID never reaches the Steam backend");
    CheckEq(static_cast<long long>(R.banCheckedIdentities.size()), 0,
            "a refused retail GUID never reaches the ban check");

    // A 32-hex field is also valid base64, so this proves the retail branch was
    // taken and nothing was decoded as a Steam ticket.
    ResetWorld();
    SetArgv("getchallenge", "0", kRetailGuid, std::string(), 3);
    SV_GetChallenge(from);
    CheckText(LastOob(), kGuidPolicyMessage,
              "a base64-shaped 32-hex field is still classified as a retail GUID");
    CheckEq(R.steamChecks, 0, "the base64 decoder and Steam backend are never reached");

    // A refused retail retry must also invalidate a challenge already issued to
    // this address, so a stale token cannot be replayed after a failed retry.
    ResetWorld();
    token = IssueChallenge(from);
    Check(token > 0, "a Steam challenge was issued first");
    Check(FindSlot(from) >= 0, "the Steam challenge is recorded");
    IssueRetailChallenge(from, kRetailGuid);
    CheckText(LastOob(), kGuidPolicyMessage, "the retail retry is still refused");
    Check(AllChallengesZeroed(), "the refused retail retry invalidated the earlier Steam token");
}

// ---- SV_GetChallenge: retail GUIDs, switch on -----------------------------

static void TestGetChallengeRetailGuidEnabled()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    EnableUnverifiedGuids();

    int token = IssueRetailChallenge(from, kRetailGuid);
    Check(token > 0, "an enabled server issues a challenge for a retail GUID");
    int slot = FindSlot(from);
    Check(slot >= 0, "the retail challenge is recorded for the source address");
    if (slot >= 0)
    {
        CheckText(SlotIdentity(slot), kRetailGuid, "the GUID is stored as the identity");
        CheckEq(static_cast<long long>(strlen(svs.challenges[slot].cdkeyHash)), 32,
                "the stored identity is 32 characters");
        CheckEq(static_cast<int>(svs.challenges[slot].cdkeyHash[32]), 0,
                "byte 32 of the stored identity is NUL");
        bool exact = true;
        for (int i = 0; i < 32; ++i)
            if (svs.challenges[slot].cdkeyHash[i] != kRetailGuid[i])
                exact = false;
        Check(exact, "the stored identity is the exact GUID, byte for byte");
    }
    CheckEq(R.steamChecks, 0, "no Steam backend call for a retail GUID");
    Check(R.banCheckedIdentities.size() >= 1, "the permanent-ban check ran");
    if (!R.banCheckedIdentities.empty())
        CheckText(R.banCheckedIdentities[0], kRetailGuid,
                  "the permanent-ban check received the normalised GUID");

    // Uppercase and mixed-case spellings normalise to the same identity.
    ResetWorld();
    EnableUnverifiedGuids();
    token = IssueRetailChallenge(from, kRetailGuidUpper);
    Check(token > 0, "an uppercase GUID is accepted");
    CheckText(SlotIdentity(FindSlot(from)), kRetailGuid, "an uppercase GUID is stored as lowercase");
    ResetWorld();
    EnableUnverifiedGuids();
    token = IssueRetailChallenge(from, kRetailGuidMixed);
    Check(token > 0, "a mixed-case GUID is accepted");
    CheckText(SlotIdentity(FindSlot(from)), kRetailGuid, "a mixed-case GUID is stored as lowercase");

    // Any source address, with the loopback test bypass off the whole time.
    struct SourceCase
    {
        netadr_t adr;
        const char *what;
    };
    const SourceCase sources[] = {
        { MakeAdr(203, 0, 113, 7, 28000), "a public address" },
        { MakeAdr(192, 168, 1, 20, 28960), "a LAN address" },
        { MakeAdr(10, 1, 2, 3, 28962), "a 10/8 address" },
        { MakeAdr(127, 0, 0, 1, 28961), "a 127.0.0.1 source" },
        { MakeAdr(172, 16, 0, 9, 28963), "a 172.16/12 address" },
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
    {
        ResetWorld();
        EnableUnverifiedGuids();
        Check(!g_loopbackAuthTestFrozenEnabled, "the loopback test bypass is off");
        int issued = IssueRetailChallenge(sources[i].adr, kRetailGuid);
        Check(issued > 0, std::string("a retail GUID is accepted from ") + sources[i].what);
        CheckText(SlotIdentity(FindSlot(sources[i].adr)), kRetailGuid,
                  std::string("the identity from ") + sources[i].what + " is the GUID");
        CheckEq(R.steamChecks, 0, std::string("no Steam backend call from ") + sources[i].what);
    }

    // All-digit and all-zero spellings are GUIDs here, never Steam identities.
    const char *const numericGuids[] = { "12345678901234567890123456789012",
                                         "00000000000000000000000000000000" };
    for (size_t i = 0; i < sizeof(numericGuids) / sizeof(numericGuids[0]); ++i)
    {
        ResetWorld();
        EnableUnverifiedGuids();
        int issued = IssueRetailChallenge(from, numericGuids[i]);
        Check(issued > 0, std::string("the GUID \"") + numericGuids[i] + "\" is accepted");
        CheckText(SlotIdentity(FindSlot(from)), numericGuids[i], "the numeric GUID is stored verbatim");
        CheckEq(R.steamChecks, 0, "a numeric GUID is never treated as a Steam identity");
    }
}

// ---- SV_GetChallenge: malformed retail GUIDs ------------------------------

static void TestGetChallengeRetailGuidMalformed()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    const char *const bad[] = {
        "9fa4b618b026886e7fe966bb184ae77",        // 31 characters
        "9fa4b618b026886e7fe966bb184ae7700",      // 33 characters
        "",                                       // empty
        "9fa4b618b026886e7fe966bb184ae77g",       // non-hex letter
        "9fa4b618b026-86e7fe966bb184ae770",       // separator
        "\331\2419fa4b618b026886e7fe966bb184",  // non-ASCII first bytes
        " 9fa4b618b026886e7fe966bb184ae770",      // leading whitespace
        "9fa4b618b026886e7fe966bb184ae770 ",      // trailing whitespace
        "+9fa4b618b026886e7fe966bb184ae77",       // sign
        "-9fa4b618b026886e7fe966bb184ae77",       // sign
        "0x9fa4b618b026886e7fe966bb184ae7",       // 0x prefix
        "local-test-000012340000",                // synthetic namespace
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        ResetWorld();
        EnableUnverifiedGuids();
        std::string label = std::string("retail GUID \"") + Escape(bad[i]) + "\"";
        int token = IssueRetailChallenge(from, bad[i]);
        CheckText(LastOob(), kMalformedGuidMessage, label + " is refused as malformed");
        CheckEq(static_cast<long long>(token), -1, label + " gets no token");
        Check(AllChallengesZeroed(), label + " allocates no challenge slot");
        CheckEq(R.steamChecks, 0, label + " never reaches the Steam backend");
    }
}

// ---- SV_GetChallenge: field-count classification --------------------------

static void TestGetChallengeArgcClassification()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // argc 1 and 2: no identity field at all.
    SetArgv("getchallenge", std::string(), std::string(), std::string(), 1);
    SV_GetChallenge(from);
    CheckText(LastOob(), kMissingFieldsMessage, "argc 1 is reported as missing fields");
    ResetWorld();
    SetArgv("getchallenge", "0", std::string(), std::string(), 2);
    SV_GetChallenge(from);
    CheckText(LastOob(), kMissingFieldsMessage, "argc 2 is reported as missing fields");
    Check(AllChallengesZeroed(), "argc 2 allocates no slot");

    // argc 3 depends only on the GUID policy, not on the field contents.
    ResetWorld();
    SetArgv("getchallenge", "0", kRetailGuid, std::string(), 3);
    SV_GetChallenge(from);
    CheckText(LastOob(), kGuidPolicyMessage, "argc 3 takes the retail branch while disabled");
    ResetWorld();
    EnableUnverifiedGuids();
    SetArgv("getchallenge", "0", kRetailGuid, std::string(), 3);
    SV_GetChallenge(from);
    Check(OobChallengeToken() > 0, "argc 3 takes the retail branch while enabled");
    CheckEq(R.steamChecks, 0, "the retail branch never calls the Steam backend");

    // argc 4 is the Steam handshake, unaffected by the GUID policy.
    ResetWorld();
    EnableUnverifiedGuids();
    SetArgv("getchallenge", "0", kGoodTicket, kGoodIdentity, 4);
    SV_GetChallenge(from);
    Check(OobChallengeToken() > 0, "argc 4 still takes the Steam path when GUIDs are enabled");
    CheckEq(R.steamChecks, 1, "argc 4 always verifies with the Steam backend");
    CheckText(SlotIdentity(FindSlot(from)), kGoodIdentity, "argc 4 stores the Steam identity");

    // argc 5 and above is not a handshake this server implements.
    ResetWorld();
    EnableUnverifiedGuids();
    SetArgv("getchallenge", "0", kRetailGuid, kGoodIdentity, 5);
    SV_GetChallenge(from);
    CheckText(LastOob(), kUnsupportedHandshakeMessage, "argc 5 is refused as unsupported");
    Check(AllChallengesZeroed(), "argc 5 allocates no slot");
    CheckEq(R.steamChecks, 0, "argc 5 never reaches the Steam backend");
}

// ---- SV_GetChallenge: Steam is never replaced by the GUID path ------------

static void TestGetChallengeSteamNeverFallsBackToGuid()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // A ticket field of 32 hex digits is a valid base64 payload *and* a
    // syntactically valid GUID. With GUIDs enabled it must still be verified as
    // a Steam ticket, because argc decided the method, not the field shape.
    ResetWorld();
    EnableUnverifiedGuids();
    SetArgv("getchallenge", "0", kRetailGuid, kGoodIdentity, 4);
    SV_GetChallenge(from);
    Check(OobChallengeToken() > 0, "the Steam handshake is accepted on its own merits");
    CheckText(SlotIdentity(FindSlot(from)), kGoodIdentity,
              "the Steam identity is stored, not the GUID-shaped ticket");
    CheckEq(R.steamChecks, 1, "the GUID-shaped ticket was verified, not reinterpreted");

    // Backend rejection fails closed: no fallback to the GUID branch, and no
    // identity is left behind for a later directconnect to pick up.
    ResetWorld();
    EnableUnverifiedGuids();
    g_steamAcceptsTicket = false;
    SetArgv("getchallenge", "0", kRetailGuid, kGoodIdentity, 4);
    SV_GetChallenge(from);
    CheckEq(R.steamChecks, 1, "the backend was consulted exactly once");
    Check(AllChallengesZeroed(), "a rejected Steam ticket leaves no challenge");
#if defined(__linux__) && defined(KISAK_DEDICATED)
    CheckText(LastOob(), "error\nSteam authentication is unavailable in this native Linux build.",
              "the native Linux unavailable message is preserved");
#else
    CheckText(LastOob(), "error\x0a\x15" "Your Steam Client Ticket was Invalid",
              "the Steam rejection message is preserved");
#endif
}

// ---- temporary ban keys ---------------------------------------------------

static void TestTempBanKeyHardening()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // A short identity bans exactly that identity.
    SV_BanGuidBriefly("abc");
    CheckEq(SV_IsTempBannedGuid("abc"), 1, "a short identity bans itself");
    CheckEq(SV_IsTempBannedGuid("abd"), 0, "a neighbouring short identity is not banned");
    CheckEq(SV_IsTempBannedGuid("abcdef"), 0, "a longer identity sharing the prefix is not banned");
    CheckEq(SV_IsTempBannedGuid("ab"), 0, "the prefix alone is not banned");

    // The 32nd byte participates: two 32-character keys that differ only at
    // index 31 are different keys.
    ResetWorld();
    const char *keyA = "9fa4b618b026886e7fe966bb184ae770";
    const char *keyB = "9fa4b618b026886e7fe966bb184ae771";
    SV_BanGuidBriefly(keyA);
    CheckEq(SV_IsTempBannedGuid(keyA), 1, "a 32-character identity bans itself");
    CheckEq(SV_IsTempBannedGuid(keyB), 0, "an identity differing only at byte 32 is not banned");

    // Empty identities: never banned, and banning one does not poison the table.
    ResetWorld();
    CheckEq(SV_IsTempBannedGuid(""), 0, "an empty identity is never temporarily banned");
    SV_BanGuidBriefly("");
    CheckEq(SV_IsTempBannedGuid(""), 0, "banning an empty identity does not ban it");
    CheckEq(SV_IsTempBannedGuid("abc"), 0, "banning an empty identity bans nothing else");
    CheckEq(static_cast<long long>(SV_FindFreeTempBanSlot()), 0, "the empty ban left the slot reusable");

    // A temporary ban still rejects the challenge and clears the slot.
    ResetWorld();
    EnableUnverifiedGuids();
    SV_BanGuidBriefly(kRetailGuid);
    int token = IssueRetailChallenge(from, kRetailGuid);
    CheckText(LastOob(), "error\x0a\x15" "You are temporarily banned from this server",
              "a temporarily banned GUID is refused");
    CheckEq(static_cast<long long>(token), -1, "a temporarily banned GUID gets no token");
    Check(AllChallengesZeroed(), "a temporarily banned GUID leaves no challenge");

    // An expired ban no longer rejects.
    ResetWorld();
    EnableUnverifiedGuids();
    SV_BanGuidBriefly(kRetailGuid);
    svs.tempBans[0].banTime = svs.time - 700000;
    token = IssueRetailChallenge(from, kRetailGuid);
    Check(token > 0, "an expired GUID ban no longer rejects the GUID");
}

static void TestPermanentBanGetsNormalizedGuid()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);

    // An uppercase claim is ban-checked under its normalised spelling, so a ban
    // entry written in lowercase catches every spelling of the same GUID.
    ResetWorld();
    EnableUnverifiedGuids();
    g_bannedGuid = true;
    IssueRetailChallenge(from, kRetailGuidUpper);
    Check(R.banCheckedIdentities.size() >= 1, "the permanent-ban check ran for a retail GUID");
    if (!R.banCheckedIdentities.empty())
        CheckText(R.banCheckedIdentities[0], kRetailGuid,
                  "the permanent-ban check received the normalised GUID");
    CheckText(LastOob(), "error\x0a\x15" "You are permanently banned from this server",
              "a permanently banned GUID is refused");
    Check(AllChallengesZeroed(), "a permanently banned GUID leaves no challenge");
    CheckEq(R.steamChecks, 0, "a banned GUID never reaches the Steam backend");

    // With the switch off the identity is refused before any ban check, so the
    // ordering of policy and ban lookup is observable.
    ResetWorld();
    g_bannedGuid = true;
    IssueRetailChallenge(from, kRetailGuidUpper);
    CheckText(LastOob(), kGuidPolicyMessage, "the policy rejection comes first");
    CheckEq(static_cast<long long>(R.banCheckedIdentities.size()), 0,
            "a policy-rejected GUID never reaches the ban check");
}

// ---- SV_DirectConnect: identity policy ------------------------------------

static void TestDirectConnectGuidIdentity()
{
    netadr_t from = MakeAdr(203, 0, 113, 7, 28000);
    const int token = 0x51A7E;
    ConnectArgs version1 = { "1", token, 49001, "" };

    // Switch off: a challenge record carrying a GUID identity is refused even
    // with the right token. This is what stops a forged or stale slot from
    // admitting an identity the process never agreed to accept.
    int slot = SeedChallenge(from, kRetailGuid, token);
    Check(slot >= 0, "a GUID challenge record was seeded");
    ConnectAttempt(from, MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "a GUID identity is refused while the switch is off");
    CheckEq(R.netchanSetups, 0, "a refused GUID identity sets up no netchan");
    Check(!ClientIsConnected(0), "a refused GUID identity connects no client");

    // Switch on: accepted, and connected with the identity copied onto the client.
    ResetWorld();
    EnableUnverifiedGuids();
    slot = SeedChallenge(from, kRetailGuid, token);
    ConnectAttempt(from, MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "a GUID identity connects when enabled");
    Check(ClientIsConnected(0), "the GUID client reached CS_CONNECTED");
    CheckText(std::string(svs.clients[0].cdkeyHash), kRetailGuid, "the GUID is copied onto the client");
    CheckEq(svs.clients[0].challenge, token, "the client records the accepted token");
    CheckEq(R.netchanSetups, 1, "exactly one netchan was set up");

    // The token still has to match, from the right address.
    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuid, token);
    ConnectArgs wrongToken = { "1", token + 1, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(wrongToken).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "a wrong token is refused for a GUID identity");

    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuid, token);
    ConnectAttempt(MakeAdr(203, 0, 113, 7, 28001), MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "another source port cannot use the token");

    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuid, token);
    ConnectAttempt(MakeAdr(203, 0, 113, 8, 28000), MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "another address cannot use the token");

    // A non-canonical spelling is not accepted, even with the switch on: the
    // handshake always stores lowercase, so an uppercase slot is not its output.
    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuidUpper, token);
    ConnectAttempt(from, MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "a non-canonical GUID identity is refused");

    // A synthetic local test label outside test mode is refused.
    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, "local-test-000012340000", token);
    ConnectAttempt(from, MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "a synthetic test identity is refused outside test mode");

    // userinfo is never an identity source: a Steam-looking value there cannot
    // rescue a GUID slot while the switch is off.
    ResetWorld();
    SeedChallenge(from, kRetailGuid, token);
    {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "\\protocol\\1\\challenge\\%d\\qport\\49001\\guid\\%s\\cdkey\\%s",
                 token, kGoodIdentity, kGoodIdentity);
        ConnectAttempt(from, buffer);
    }
    CheckText(LastOob(), "error\nEXE_BAD_CHALLENGE", "a userinfo identity cannot rescue a refused GUID slot");
    Check(!ClientIsConnected(0), "no client was connected from the userinfo claim");

    // ...and it cannot replace the identity the handshake stored.
    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuid, token);
    {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "\\protocol\\1\\challenge\\%d\\qport\\49001\\guid\\%s",
                 token, kGoodIdentity);
        ConnectAttempt(from, buffer);
    }
    CheckText(LastOob(), "connectResponse mods/kisak", "the GUID client connected");
    CheckText(std::string(svs.clients[0].cdkeyHash), kRetailGuid,
              "the challenge identity wins over a userinfo claim");

    // The protocol gate is unchanged by the GUID policy.
    ResetWorld();
    EnableUnverifiedGuids();
    SeedChallenge(from, kRetailGuid, token);
    ConnectArgs protocol21 = { "21", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(protocol21).c_str());
    Check(LastOob().find("This server requires protocol version 1") != std::string::npos,
          "the protocol gate still rejects protocol 21 with GUIDs enabled");
    Check(!ClientIsConnected(0), "a rejected protocol connects no client");

    // Retail 1.7 hosting: protocol 6 plus a GUID identity. The two switches are
    // independent, so both have to be on.
    ResetWorld();
    EnableUnverifiedGuids();
    dvar_allowprotocol6.current.enabled = true;
    SeedChallenge(from, kRetailGuid, token);
    ConnectArgs protocol6 = { "6", token, 49001, "" };
    ConnectAttempt(from, MakeUserinfo(protocol6).c_str());
    CheckText(LastOob(), "connectResponse mods/kisak", "a protocol 6 GUID client connects");
    Check(ClientIsConnected(0), "the protocol 6 GUID client reached CS_CONNECTED");
    CheckText(std::string(svs.clients[0].cdkeyHash), kRetailGuid, "the protocol 6 client keeps the GUID");

    // The private-password slot gate is unchanged by the GUID policy.
    ResetWorld();
    EnableUnverifiedGuids();
    dvar_maxclients.current.integer = 1;
    dvar_privateclients.current.integer = 1;
    dvar_privatepassword.current.string = "secret";
    SeedChallenge(from, kRetailGuid, token);
    ConnectArgs wrongPassword = { "1", token, 49001, "wrong" };
    ConnectAttempt(from, MakeUserinfo(wrongPassword).c_str());
    CheckText(LastOob(), "error\nEXE_SERVERISFULL",
              "a wrong private password still finds no slot with GUIDs enabled");
    Check(!ClientIsConnected(0), "a wrong password connects no client");
}

#if defined(__linux__) && defined(KISAK_DEDICATED)
static void TestBothSwitchesFailClosed()
{
    netadr_t loop = MakeAdr(127, 0, 0, 1, 28960);
    EnableUnverifiedGuids();
    g_loopbackAuthTestFrozenEnabled = true;
    g_loopbackAuthTestCanBypass = true;

    // This configuration cannot start (NET_InitLoopbackAuthTest is fatal), but
    // if it were ever reached the packet path must still admit nobody.
    SetArgv("getchallenge");
    SV_GetChallenge(loop);
    CheckText(LastOob(), "error\nsv_loopbackTestAuth and sv_allowUnverifiedGuids cannot both be enabled.",
              "the packet path refuses both switches at once");
    Check(AllChallengesZeroed(), "no challenge is issued when both switches are on");
    CheckEq(R.steamChecks, 0, "no backend call when both switches are on");

    // SV_DirectConnect goes through the same guard.
    const int token = 0x51A7E;
    SeedChallenge(loop, "local-test-000012340000", token);
    ConnectArgs version1 = { "1", token, 49001, "" };
    ConnectAttempt(loop, MakeUserinfo(version1).c_str());
    CheckText(LastOob(), "error\nsv_loopbackTestAuth and sv_allowUnverifiedGuids cannot both be enabled.",
              "directconnect refuses both switches at once");
    Check(!ClientIsConnected(0), "no client connects when both switches are on");
}
#endif
'''

DRIVER_MAIN = r'''
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("server handshake authentication (src/server_mp/sv_client_mp.cpp + sv_handshake_auth.h)\n");
#if defined(__linux__) && defined(KISAK_DEDICATED)
    printf("build: native-linux-dedicated (KISAK_DEDICATED defined)\n");
#else
    printf("build: nonnative (KISAK_DEDICATED not defined)\n");
#endif
    printf("sizeof(netadr_t)=%u sizeof(client_t)=%u sizeof(challenge_t)=%u\n",
           (unsigned)sizeof(netadr_t), (unsigned)sizeof(client_t), (unsigned)sizeof(challenge_t));

    Run("SV_ParseSteamIdentity", TestParseSteamIdentity);
    Run("SV_ValidateSteamTicketBase64", TestValidateSteamTicketBase64);
    Run("SV_ParseRetailGuid", TestParseRetailGuid);
    Run("sv_allowUnverifiedGuids: capture and veto", TestUnverifiedGuidsCapture);
    Run("sv_advertisedProtocol: capture immutability", TestAdvertisedProtocolCapture);
#if defined(__linux__) && defined(KISAK_DEDICATED)
    Run("SV_IsLoopbackTestIdentity", TestLoopbackTestIdentity);
#endif
    Run("SV_GetChallenge: missing and malformed fields", TestGetChallengeMissingFields);
    Run("SV_GetChallenge: malformed authentication fields", TestGetChallengeMalformedFields);
    Run("SV_GetChallenge: field-count classification", TestGetChallengeArgcClassification);
    Run("SV_GetChallenge: retail GUIDs disabled", TestGetChallengeRetailGuidDisabled);
    Run("SV_GetChallenge: retail GUIDs enabled", TestGetChallengeRetailGuidEnabled);
    Run("SV_GetChallenge: malformed retail GUIDs", TestGetChallengeRetailGuidMalformed);
    Run("SV_GetChallenge: Steam never falls back to GUID", TestGetChallengeSteamNeverFallsBackToGuid);
    Run("SV_GetChallenge: temporary ban keys", TestTempBanKeyHardening);
    Run("SV_GetChallenge: permanent ban identity", TestPermanentBanGetsNormalizedGuid);
    Run("SV_GetChallenge: backend rejection", TestGetChallengeBackendRejection);
    Run("SV_GetChallenge: accepted authentication", TestGetChallengeAccepted);
    Run("SV_GetChallenge: failed authentication clears the challenge", TestGetChallengeFailureClearsIssuedChallenge);
    Run("SV_GetChallenge: identity length", TestGetChallengeIdentityLength);
    Run("SV_GetChallenge: bans", TestGetChallengeBans);
#if defined(__linux__) && defined(KISAK_DEDICATED)
    Run("SV_GetChallenge: loopback test identity", TestLocalTestChallengeIdentity);
    Run("SV_GetChallenge: loopback test identity stability", TestLocalTestChallengeIsStableAndUnique);
    Run("SV_GetChallenge: loopback test guard", TestLocalTestGuardRejectsUnpermittedSource);
#endif
    Run("SV_DirectConnect: version rejection", TestDirectConnectVersionRejection);
    Run("SV_DirectConnect: protocol 6 admission", TestDirectConnectProtocol6Admission);
    Run("SV_DirectConnect: challenge rejection", TestDirectConnectBadChallenge);
    Run("SV_DirectConnect: success path", TestDirectConnectSuccess);
    Run("SV_DirectConnect: GUID identity policy", TestDirectConnectGuidIdentity);
#if defined(__linux__) && defined(KISAK_DEDICATED)
    Run("SV_DirectConnect: identity rules in test mode", TestDirectConnectTestModeIdentityRules);
    Run("SV_DirectConnect: guard ordering", TestDirectConnectGuardOrdering);
    Run("both switches: fail closed", TestBothSwitchesFailClosed);
#endif
    Run("SV_DirectConnect: capacity and password", TestDirectConnectCapacityAndPassword);
    Run("SV_DirectConnect: game rejection", TestDirectConnectGameRejection);
    Run("SV_DirectConnect: reconnect limit", TestDirectConnectReconnectLimit);
    Run("SV_DirectConnect: loopback source", TestDirectConnectLoopbackSource);
    Run("SV_DropClient: Steam notification", TestDropClientSteamNotification);
    Run("SV_DropClient: identity classification", TestDropClientIdentityClassification);
    Run("SV_DropClient: zombie client", TestDropClientZombie);

    printf("\n%i checks, %i failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
'''

FOOTER_SENTINEL = "// ---- end of extracted production code ----\n"


def build_harness(client_functions: dict, base64_functions: dict) -> str:
    extracted_client = ""
    extracted_client += "// ---- extracted from src/server_mp/sv_client_mp.cpp (verbatim) ----\n"
    # SV_BanGuidBriefly() calls SV_FindFreeTempBanSlot(), which production defines
    # after it; declare it first so the extracted text compiles as-is.
    extracted_client += "uint32_t SV_FindFreeTempBanSlot();\n"
    extracted_client += client_functions["SV_CaptureUnverifiedGuids"] + "\n"
    extracted_client += client_functions["SV_UnverifiedGuidsEnabled"] + "\n"
    extracted_client += client_functions["SV_CaptureAdvertisedProtocol"] + "\n"
    extracted_client += client_functions["SV_GetAdvertisedProtocol"] + "\n"
    extracted_client += client_functions["SV_SetAdvertisedProtocolInfo"] + "\n"
    extracted_client += "#if defined(__linux__) && defined(KISAK_DEDICATED)\n"
    extracted_client += client_functions["SV_RejectUnsafeTestHandshake"]
    extracted_client += "#endif\n\n"
    extracted_client += client_functions["SV_FindFreeTempBanSlot"] + "\n"
    extracted_client += client_functions["SV_BanGuidBriefly"] + "\n"
    extracted_client += client_functions["SV_IsTempBannedGuid"] + "\n"
    extracted_client += client_functions["SV_GetChallenge"] + "\n"
    extracted_client += client_functions["SV_DirectConnect"] + "\n"
    extracted_client += client_functions["SV_DropClient"]
    extracted_client += FOOTER_SENTINEL

    extracted_base64 = "// ---- extracted from src/universal/base64.cpp (verbatim) ----\n"
    extracted_base64 += base64_functions["b64_int"] + "\n"
    extracted_base64 += base64_functions["b64_decode"]
    extracted_base64 += FOOTER_SENTINEL

    header_include = ("// ---- the production header, unmodified ----\n"
                      f'#include "{AUTH_HEADER.as_posix()}"\n')

    return (
        PREAMBLE
        + header_include
        + "\n"
        + extracted_client
        + "\n"
        + extracted_base64
        + DRIVER_HEAD
        + DRIVER_MAIN
    )


def run_variant(compiler, flags, label, source, timeout, workdir):
    """Compile and run one build configuration. Returns (ok, banner)."""
    harness = workdir / f"handshake_{label}.cpp"
    harness.write_text(source, encoding="utf-8")

    binary = workdir / f"handshake_{label}"
    command = [compiler] + list(flags) + [
        "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
        "-Wno-unused-function", "-Wno-missing-field-initializers",
        str(harness), "-o", str(binary),
    ]

    compiled = subprocess.run(command, capture_output=True, text=True)
    banner = f"[{label}] {' '.join(command)}"
    if compiled.returncode != 0:
        print(f"FAIL: [{label}] harness did not compile", file=sys.stderr)
        print(banner, file=sys.stderr)
        print(compiled.stdout, file=sys.stderr)
        print(compiled.stderr, file=sys.stderr)
        return False, banner
    if compiled.stderr.strip():
        print(f"[{label}] compiler diagnostics:", file=sys.stderr)
        print(compiled.stderr.strip(), file=sys.stderr)

    try:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"FAIL: [{label}] harness did not finish within {timeout}s", file=sys.stderr)
        return False, banner

    sys.stdout.write(run.stdout)
    if run.stderr.strip():
        sys.stderr.write(f"[{label}] stderr:\n")
        sys.stderr.write(run.stderr)
    if run.returncode != 0:
        print(f"FAIL: [{label}] harness reported failures (exit {run.returncode})", file=sys.stderr)
        return False, banner
    return True, banner


def toolchain_supports(compiler, flags, workdir):
    """True if ``compiler`` can build and link a trivial program with ``flags``.

    This is a capability probe for optional configurations (-m32, sanitizers),
    not a substitute for a real harness build: every configuration that runs is
    compiled and linked with the real source and its diagnostics are reported.
    """
    probe = workdir / "toolchain_probe.cpp"
    probe.write_text("int main() { return 0; }\n", encoding="utf-8")
    binary = workdir / "toolchain_probe"
    result = subprocess.run([compiler] + list(flags) + [str(probe), "-o", str(binary)],
                            capture_output=True, text=True)
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                    formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", ""),
                        help="C++ compiler (default: $CXX, then clang++-19, clang++, c++)")
    parser.add_argument("--compiler", dest="cxx_alias", default="",
                        help="alias for --cxx, kept for callers that use the older name")
    parser.add_argument("--timeout", type=int, default=180,
                        help="hard timeout for each compiled harness, in seconds")
    parser.add_argument("--no-sanitize", action="store_true",
                        help="compile without -fsanitize=address,undefined")
    parser.add_argument("--m32", action="store_true",
                        help="require a 32-bit (-m32) build in addition to 64-bit;"
                             " fails if the toolchain cannot produce one")
    parser.add_argument("--keep", action="store_true", help="keep the generated harness")
    args = parser.parse_args()

    compiler = args.cxx or args.cxx_alias
    if not compiler:
        for candidate in ("clang++-19", "clang++", "c++"):
            if shutil.which(candidate):
                compiler = candidate
                break
    if not compiler:
        print("FAIL: no C++ compiler found", file=sys.stderr)
        return 2
    if not shutil.which(compiler):
        print(f"FAIL: compiler {compiler!r} is not on PATH", file=sys.stderr)
        return 2

    for path in (CLIENT_SOURCE, AUTH_HEADER, BASE64_SOURCE):
        if not path.is_file():
            print(f"FAIL: {path} is missing", file=sys.stderr)
            return 2

    client_source = CLIENT_SOURCE.read_text(encoding="utf-8", errors="replace")
    base64_source = BASE64_SOURCE.read_text(encoding="utf-8", errors="replace")

    client_functions = {}
    for name, expected in CLIENT_FUNCTIONS:
        text = extract_definition(client_source, name, expected, CLIENT_SOURCE)
        client_functions[name] = text.replace("__cdecl", "")

    base64_functions = {}
    for name, expected in BASE64_FUNCTIONS:
        text = extract_definition(base64_source, name, expected, BASE64_SOURCE)
        base64_functions[name] = text

    source = build_harness(client_functions, base64_functions)

    workdir = Path(tempfile.mkdtemp(prefix="kisakcod_handshake_auth_"))
    print(f"compiler: {compiler}")
    print(f"generated harness: {workdir}")

    # ASan/UBSan matching the other asset-free checks; a sanitizer report is a
    # failure, not a warning.
    sanitize_flags = []
    if not args.no_sanitize:
        sanitize_flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if not toolchain_supports(compiler, sanitize_flags, workdir):
            print("NOTE: address/undefined sanitizers are unavailable for this toolchain;"
                  " building without them", file=sys.stderr)
            sanitize_flags = []

    # Widths. The shipped Linux server target is 32-bit, so the 32-bit
    # configuration is built whenever the toolchain can produce one, and --m32
    # makes it mandatory. The probe only chooses *flags* for an optional
    # configuration; every configuration listed below is then compiled and
    # linked for real, and a compile failure there is reported as a failure --
    # there is no retry with different flags and no silent substitution.
    build_flags_32 = None
    if toolchain_supports(compiler, sanitize_flags + ["-m32"], workdir):
        build_flags_32 = sanitize_flags + ["-m32"]
    elif toolchain_supports(compiler, ["-m32"], workdir):
        build_flags_32 = ["-m32"]
        print("NOTE: this toolchain has no 32-bit address/undefined sanitizer runtime;"
              " the 32-bit configuration is built without sanitizers"
              + (" while the 64-bit configurations keep them" if sanitize_flags else ""),
              file=sys.stderr)

    if args.m32:
        if build_flags_32 is None:
            print("FAIL: --m32 requires a 32-bit build and this toolchain cannot produce one",
                  file=sys.stderr)
            return 2
        widths = [("64", list(sanitize_flags)), ("32", build_flags_32)]
    else:
        widths = [("64", list(sanitize_flags))]
        if build_flags_32 is not None:
            widths.append(("32", build_flags_32))
        else:
            print("NOTE: this toolchain cannot produce 32-bit binaries, so only the 64-bit"
                  " configuration is built; the shipped Linux target is 32-bit, so pass --m32"
                  " on a multilib toolchain to require it", file=sys.stderr)

    variants = []
    for suffix, macros in (("native-linux-dedicated", ["-DKISAK_DEDICATED"]), ("nonnative", [])):
        for width, width_flags in widths:
            variants.append((f"{suffix}-{width}", list(width_flags) + macros))

    print("configurations: " + ", ".join(label for label, _ in variants))

    results = []
    try:
        for label, flags in variants:
            ok, banner = run_variant(compiler, flags, label, source, args.timeout, workdir)
            results.append((label, ok, banner))
        if args.keep:
            print(f"kept harness sources in {workdir}")
    finally:
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)

    failures = [label for label, ok, _ in results if not ok]
    print()
    for label, ok, banner in results:
        print(f"{'PASS' if ok else 'FAIL'}  {label}: {banner}")
    if failures:
        print(f"\nFAILED: {', '.join(failures)}", file=sys.stderr)
        return 1
    print(f"\nall handshake authentication checks passed in every configuration"
          f" ({len(results)} builds)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
