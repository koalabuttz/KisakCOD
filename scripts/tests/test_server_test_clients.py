"""Exercise the production server test-client code, asset-free.

The multi-player server can fill its own client slots with internal bots:

  ``SV_AddTestClient``  (``src/server_mp/sv_client_mp.cpp``) picks a free
                        ``client_t`` slot, builds a ``connect`` userinfo string,
                        synthesises a ``NA_BOT`` address, hands it to the real
                        ``SV_DirectConnect`` path, marks the client as a test
                        client, sends the gamestate and enters the world with a
                        zeroed usercmd.
  ``SV_BotUserMove``    (``src/server_mp/sv_main_mp.cpp``) fabricates the usercmd
                        a bot would have sent, from the player's current weapon
                        and a few ``random()`` draws.
  ``SV_UpdateBots``     drives ``SV_BotUserMove`` once per frame for every
                        active ``NA_BOT`` client, and re-enters retained internal clients after a map load.

Those three function bodies are extracted from production text unchanged and
compiled against doubles for the surrounding engine services (``svs``, the
dvars, command tokenising, ``SV_DirectConnect``, gamestate/enter-world, the game
client lookup, ``random``/``crandom``). Every signature is checked against the
expected production signature before it is used, so a source change fails the
run instead of testing other code.

What this pins down, and why each invariant matters for a load test:

  * capacity      a full server returns NULL and creates nothing; bots never
                  exceed ``sv_maxclients``, never take a non-free slot, and
                  never disturb the real clients already sitting in the gaps
  * refusal       when ``SV_DirectConnect`` admits nobody, or admits a client
                  under a different address, ``SV_AddTestClient`` returns NULL
                  and sends neither a gamestate nor an enter-world
  * address       the synthesised ``netadr_t`` is deterministic in all 20 bytes
                  (``NA_BOT`` type, both port bytes, zero ip, zero ipx), so no
                  uninitialised stack byte reaches ``Netchan_Setup``; each bot
                  gets its own source port. Both the address handed to
                  ``SV_DirectConnect`` and the address used for the client
                  lookup are compared byte for byte, and the build compiles the
                  extracted code with ``-ftrivial-auto-var-init=pattern`` --
                  proven at run time by a probe, not assumed -- so a stack
                  object the function forgets to clear keeps a non-zero pattern
                  and the comparison fails instead of passing on a zeroed stack.
                  ``--mutations`` demonstrates exactly that by rebuilding
                  temporary copies with the initialisation removed.
  * slot reuse    a slot that something *outside* this function freed (a plain
                  ``memset``, i.e. no ``SV_DropClient``) is reused, and the new
                  bot still gets a fresh port instead of colliding with the
                  departed one
  * connect path  the bot path reaches the ``SV_DirectConnect`` boundary with an
                  ``NA_BOT`` address and a well formed ``connect`` userinfo
                  string, and it consumes one tokenized string per attempt
  * usercmd       weapon is copied as the byte the production struct holds
                  (with the production range assert), buttons/movement only
                  appear when the player has no archived time, attack only when
                  ``sv_botsPressAttackBtn`` allows it, movement/angles follow the
                  documented draw order, and the delta message always tracks the
                  outgoing sequence
  * dispatch      only active ``NA_BOT`` clients are driven; free/zombie slots and
                  real loopback/IP clients are skipped

Explicit limitations:

  * This is a double-tested unit boundary, not a running server. It cannot show
    map loading, script spawns, team assignment, entity state, snapshot encoding
    or shutdown; ``scripts/tests/test_linux_bots.py`` (opt-in, isolated) covers
    that, and still cannot show client-side or AI behaviour, because a test
    client is not a client and this is not an AI.
  * ``SV_DirectConnect`` here is a double that admits the first ``CS_FREE`` slot
    and records what it was given. The test therefore proves what the bot path
    *offers* that boundary - an ``NA_BOT`` address and a ``connect`` userinfo
    string - and not that the real path skips authentication: challenge, CD-key
    and ``SV_AuthorizeRequest`` decisions live inside the real
    ``SV_DirectConnect`` and are not reached here.
  * The cleanup case frees a slot with ``memset``. That is not ``SV_DropClient``,
    which also shuts down the netchan, downloads and userinfo, so nothing about
    a real disconnect is claimed.
  * ``crandom()`` is a sign stand-in; production only truncates it to a usercmd
    angle, so the angle cases assert the sign that the draw selects, not a
    distribution.

Usage:
    python3 scripts/tests/test_server_test_clients.py
    python3 scripts/tests/test_server_test_clients.py --cxx clang++ --m32
    python3 scripts/tests/test_server_test_clients.py --sanitize --mutations
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLIENT_SOURCE = ROOT / "src" / "server_mp" / "sv_client_mp.cpp"
MAIN_SOURCE = ROOT / "src" / "server_mp" / "sv_main_mp.cpp"

# (source, name, exact start of the definition text). The start text is verified
# so a signature change cannot silently turn this into a different test.
FUNCTIONS = (
    (CLIENT_SOURCE, "SV_AddTestClient", "gentity_s *__cdecl SV_AddTestClient()"),
    (MAIN_SOURCE, "SV_BotUserMove", "void __cdecl SV_BotUserMove(client_t *cl)"),
    (MAIN_SOURCE, "SV_UpdateBots", "void __cdecl SV_UpdateBots()"),
)

# Damage applied to a temporary copy of the extracted production text. Each one
# must make the run fail; a mutation that no longer applies is an error, never a
# silent pass. This is what makes the address/initialisation claim falsifiable
# instead of decorative.
MUTATIONS = (
    ("omit the memset of the address handed to SV_DirectConnect",
     "memset(&a, 0, sizeof(a));",
     "/* mutation: no initialisation */"),
    ("omit the memset of the address recorded for the connect call",
     "memset(&v1, 0, sizeof(v1));",
     "/* mutation: no initialisation */"),
    ("use the old misaligned word store instead of a memset",
     "memset(&a, 0, sizeof(a));",
     "*reinterpret_cast<int *>(reinterpret_cast<char *>(&a) + 2) = 0;"),
    # This one keeps every address byte correct, so nothing but the sanitizer can
    # notice it. It is the case that only -fno-sanitize-recover=all turns into a
    # failure: with recovery enabled UBSan prints the misaligned store and the run
    # still exits 0, which would report a defect as a pass.
    ("keep the memset but add a misaligned word store on the address",
     "memset(&a, 0, sizeof(a));",
     "memset(&a, 0, sizeof(a));\n"
     "    *reinterpret_cast<int *>(reinterpret_cast<char *>(&a) + 2) = 0;"),
)

TOKEN_RE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)


def extract_definition(source: str, name: str, expected_start: str, path: Path) -> str:
    """Return the full definition of ``name``: return type, body, closing brace."""
    for match in re.finditer(r"(?<![\w:.>])" + re.escape(name) + r"\s*\(", source):
        opening = match.end() - 1
        line_start = source.rfind("\n", 0, match.start()) + 1
        start = line_start
        while start > 0 and source[start - 1] in " \t":
            start -= 1
        text = source[start:].lstrip()
        if not text.startswith(expected_start):
            continue
        depth = 0
        for token in TOKEN_RE.finditer(source, opening):
            if token.group() == "{":
                depth += 1
            elif token.group() == "}":
                depth -= 1
                if depth == 0:
                    return text[: token.end() - start]
        raise SystemExit(f"{path}: unterminated body for {name}")
    raise SystemExit(
        f"{path}: no definition starting with {expected_start!r}; "
        "the production signature changed and this test no longer applies"
    )


SHIM_HEADER = r"""
// Minimal doubles for the engine services the extracted functions call.
#ifndef KISAK_TESTCLIENTS_SHIM_H
#define KISAK_TESTCLIENTS_SHIM_H
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <cmath>

#define __cdecl
typedef unsigned long long _QWORD;
#ifndef LOBYTE
#define LOBYTE(x) static_cast<unsigned char>((x) & 0xff)
#endif
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PROF_SCOPED(name)
#define iassert(e) ((void)0)

// Same enumerator order as src/qcommon/net_chan_mp.h; NA_BOT is the zero value.
enum netadrtype_t { NA_BOT, NA_BAD, NA_LOOPBACK, NA_BROADCAST, NA_IP, NA_IPX, NA_BROADCAST_IPX };

// Real 20-byte layout: 4 type, 4 ip, 2 port, 10 ipx.
struct netadr_t {
    netadrtype_t type;
    unsigned char ip[4];
    unsigned short port;
    unsigned char ipx[10];
};
static_assert(sizeof(netadr_t) == 20, "netadr_t layout drifted");
static_assert(NA_BOT == 0, "NA_BOT must stay the zero enumerator");

enum clientState_t { CS_FREE = 0, CS_ZOMBIE, CS_CONNECTED, CS_CLIENTLOADING, CS_ACTIVE };

struct netchan_t {
    int outgoingSequence;
    netadr_t remoteAddress;
    int qport;
};

struct clientHeader_t {
    int state;
    int deltaMessage;
    netchan_t netchan;
};

struct gentity_s;

struct client_t {
    clientHeader_t header;
    int bIsTestClient;
    const char *name;
    int gentityId;
    gentity_s *gentity;
};

struct gentity_s {
    int number;
    void *client;
};

// Mirrors src/bgame: playerState_s.weapon is an int that production asserts fits
// in a byte before it is copied into the usercmd.
struct playerState_s {
    int weapon;
};
extern playerState_s gPlayerStates[64];

// Mirrors src/qcommon/msg_mp.h exactly: the movement deltas and the weapon are
// narrow, so the test checks the byte the server will actually transmit.
struct usercmd_s {                       // sizeof=0x20
    int32_t serverTime;
    int32_t buttons;
    int32_t angles[3];
    uint8_t weapon;
    uint8_t offHandIndex;
    char forwardmove;
    char rightmove;
    float meleeChargeYaw;
    uint8_t meleeChargeDist;
    char selectedLocation[2];
};
static_assert(sizeof(usercmd_s) == 0x20, "usercmd_s layout drifted");

// Production reads dvars as ``dvar->current.integer`` / ``->current.enabled``.
struct dvarValue_t { int integer; int enabled; float value; };
struct dvar_t { dvarValue_t current; };

enum { BUTTON_ATTACK = 1, BUTTON_USE = 2, BUTTON_USE_RELOAD = 4 };

struct serverStatic_t {
    client_t clients[64];
};

extern serverStatic_t svs;
extern int svMaxClients_hook;
extern gentity_s gEntities[64];

// ---- records the test asserts on -------------------------------------------
struct DirectConnectRecord {
    netadr_t from;
    std::string tokenized;
    int freeSlotAtEntry;
    int assignedSlot;
    bool full;
};
extern std::vector<DirectConnectRecord> gConnectCalls;
extern std::vector<int> gGamestateCalls;
extern bool gGamestateDropsClient;
extern std::vector<int> gEnterWorldCalls;
extern std::vector<usercmd_s> gEnterWorldCmds;
extern std::vector<std::pair<int, usercmd_s> > gThinkCalls;
extern std::string gTokenizeValue;
extern int gTokenizeCalls;
extern int gEndTokenizeCalls;
extern int gResetSkeletonCacheCalls;

// Every address operand the extracted code hands to the client lookup, so the
// test can inspect the bytes of the lookup address and not only the ones that
// reached SV_DirectConnect.
extern std::vector<netadr_t> gCompareAddrOperands;

// How the SV_DirectConnect double behaves. DC_ADMIT_MISMATCHED matches the real
// engine assigning a client under an address the caller did not pass.
enum { DC_ADMIT = 0, DC_REFUSE_FULL = 1, DC_ADMIT_MISMATCHED = 2 };
extern int gDirectConnectMode;

// deterministic random()/crandom(): pops the next scripted double
extern std::vector<double> gRandomScript;
extern size_t gRandomCursor;
extern int gAssertCount;

// The engine's random()/crandom() live in q_shared.h; libc declares
// random() too, so map the extracted text's calls onto shim helpers.
inline double shim_random() {
    if (gRandomCursor >= gRandomScript.size()) return 1.0;  // "no event"
    return gRandomScript[gRandomCursor++];
}
inline double shim_crandom() {
    // Real crandom() is in [-1,1]; production only truncates it to an integer
    // angle, so a deterministic sign is enough (documented limitation).
    return shim_random() < 0.5 ? -1.0 : 1.0;
}
#define random() shim_random()
#define crandom() shim_crandom()

inline void MyAssertHandler(const char *, int, int, const char *, const char *expr) {
    ++gAssertCount;
    std::printf("SHIM ASSERT: %s\n", expr ? expr : "(null)");
}

// Storage the extracted text reaches through the engine's dvar pointers. The
// reference members let the test write ``storage.integer`` while production
// still sees ``dvar->current.integer``.
struct dvarStorage_t {
    dvar_t d;
    int &integer;
    int &enabled;
    dvarStorage_t(int i, int e) : d{ { i, e, 0.0f } }, integer(d.current.integer),
                                  enabled(d.current.enabled) {}
};

extern dvarStorage_t com_sv_running_storage;
extern dvarStorage_t sv_maxclients_storage;
extern dvarStorage_t sv_botsPressAttackBtn_storage;
extern const dvar_t *com_sv_running;
extern const dvar_t *sv_maxclients;
extern const dvar_t *sv_botsPressAttackBtn;
extern uint16_t botport;

// The engine services the extracted text calls.
void SV_Cmd_TokenizeString(char *text);
void SV_Cmd_EndTokenizedString();
void SV_DirectConnect(netadr_t from);
bool NET_CompareBaseAdr(netadr_t a, netadr_t b);
void SV_SendClientGameState(client_t *client);
void SV_ClientEnterWorld(client_t *client, usercmd_s *cmd);
gentity_s *SV_GentityNum(int num);
int G_GetClientArchiveTime(int clientNum);
void SV_ClientThink(client_t *cl, usercmd_s *cmd);
void SV_ResetSkeletonCache();
playerState_s *SV_GameClientNum(int num);

// The production definitions under test, declared for the test translation unit.
gentity_s *SV_AddTestClient();
void SV_BotUserMove(client_t *cl);
void SV_UpdateBots();

#endif  // KISAK_TESTCLIENTS_SHIM_H
"""

SHIM_SOURCE = r"""
#include "shim.h"

serverStatic_t svs;
int svMaxClients_hook = 32;
gentity_s gEntities[64];

std::vector<DirectConnectRecord> gConnectCalls;
std::vector<int> gGamestateCalls;
bool gGamestateDropsClient;
std::vector<int> gEnterWorldCalls;
std::vector<usercmd_s> gEnterWorldCmds;
std::vector<std::pair<int, usercmd_s> > gThinkCalls;
std::string gTokenizeValue;
int gTokenizeCalls = 0;
int gEndTokenizeCalls = 0;
int gResetSkeletonCacheCalls = 0;
std::vector<netadr_t> gCompareAddrOperands;
int gDirectConnectMode = DC_ADMIT;
std::vector<double> gRandomScript;
size_t gRandomCursor = 0;
int gAssertCount = 0;

// Engine globals the extracted text names.
dvarStorage_t com_sv_running_storage(0, 1);
const dvar_t *com_sv_running = &com_sv_running_storage.d;
dvarStorage_t sv_maxclients_storage(32, 0);
const dvar_t *sv_maxclients = &sv_maxclients_storage.d;
dvarStorage_t sv_botsPressAttackBtn_storage(0, 1);
const dvar_t *sv_botsPressAttackBtn = &sv_botsPressAttackBtn_storage.d;

uint16_t botport = 1;

playerState_s gPlayerStates[64];

void SV_Cmd_TokenizeString(char *text) {
    ++gTokenizeCalls;
    gTokenizeValue = text ? text : "";
}
void SV_Cmd_EndTokenizedString() { ++gEndTokenizeCalls; }

// Mirrors the production slot choice: the first CS_FREE client inside
// sv_maxclients is taken, and a full server sends nothing and assigns nothing.
// The two refusal modes let the test drive the branches SV_AddTestClient has to
// survive when this boundary does not produce the client it expected.
void SV_DirectConnect(netadr_t from) {
    DirectConnectRecord record;
    record.from = from;
    record.tokenized = gTokenizeValue;
    record.freeSlotAtEntry = -1;
    record.assignedSlot = -1;
    record.full = false;
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        if (svs.clients[i].header.state == CS_FREE) {
            record.freeSlotAtEntry = i;
            break;
        }
    }
    if (record.freeSlotAtEntry < 0 || gDirectConnectMode == DC_REFUSE_FULL) {
        record.full = true;
        gConnectCalls.push_back(record);
        return;
    }
    client_t *cl = &svs.clients[record.freeSlotAtEntry];
    std::memset(reinterpret_cast<void *>(cl), 0, sizeof(*cl));
    cl->header.state = CS_CONNECTED;
    cl->header.netchan.remoteAddress = from;
    if (gDirectConnectMode == DC_ADMIT_MISMATCHED)
        cl->header.netchan.remoteAddress.port =
            static_cast<unsigned short>(from.port + 7);
    cl->gentityId = record.freeSlotAtEntry;
    // Production SV_BotUserMove returns early unless the client has an entity.
    cl->gentity = &gEntities[record.freeSlotAtEntry];
    record.assignedSlot = record.freeSlotAtEntry;
    gConnectCalls.push_back(record);
}

// Mirrors NET_CompareBaseAdrSigned (src/qcommon/net_chan_mp.cpp): an NA_BOT
// address is compared by source port, exactly like NA_LOOPBACK. Every operand is
// recorded so the test can also inspect the bytes of the address that is used
// for the client lookup, not only the one that reached SV_DirectConnect.
bool NET_CompareBaseAdr(netadr_t a, netadr_t b) {
    gCompareAddrOperands.push_back(a);
    if (a.type != b.type) return false;
    switch (a.type) {
    case NA_LOOPBACK:
    case NA_BOT:
        return a.port == b.port;
    case NA_IP:
        return std::memcmp(a.ip, b.ip, 4) == 0;
    case NA_IPX:
        return std::memcmp(a.ipx, b.ipx, 10) == 0;
    default:
        return false;
    }
}

void SV_SendClientGameState(client_t *client) {
    gGamestateCalls.push_back(static_cast<int>(client - svs.clients));
    client->header.state = gGamestateDropsClient ? CS_ZOMBIE : CS_CLIENTLOADING;
}

void SV_ClientEnterWorld(client_t *client, usercmd_s *cmd) {
    gEnterWorldCalls.push_back(static_cast<int>(client - svs.clients));
    gEnterWorldCmds.push_back(cmd ? *cmd : usercmd_s());
    client->header.state = CS_ACTIVE;
}

gentity_s *SV_GentityNum(int num) {
    gEntities[num].number = num;
    return &gEntities[num];
}

playerState_s *SV_GameClientNum(int num) { return &gPlayerStates[num]; }

int G_GetClientArchiveTime(int) { return gArchiveTime_hook; }

void SV_ClientThink(client_t *cl, usercmd_s *cmd) {
    gThinkCalls.push_back(std::make_pair(static_cast<int>(cl - svs.clients), *cmd));
}

void SV_ResetSkeletonCache() { ++gResetSkeletonCacheCalls; }
"""

TEST_SOURCE = r"""
#include "shim.h"
#include <cstdio>
#include <cstring>

int gArchiveTime_hook = 0;
static int failures = 0;

static void Check(bool ok, const char *what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// A bot address with nothing left in ip/ipx but the expected type and port.
static bool DefinedBotAddress(const netadr_t &a, unsigned short port) {
    netadr_t expected;
    std::memset(&expected, 0, sizeof(expected));
    expected.type = NA_BOT;
    expected.port = port;
    return std::memcmp(&a, &expected, sizeof(a)) == 0;
}

// The same, for an operand probed once per slot: the port varies per bot, but
// nothing may be left in ip/ipx and the type must be NA_BOT.
static bool WellDefinedBotAddress(const netadr_t &a) {
    for (int i = 0; i < 4; ++i)
        if (a.ip[i] != 0) return false;
    for (int i = 0; i < 10; ++i)
        if (a.ipx[i] != 0) return false;
    return a.type == NA_BOT && a.port != 0;
}

static void Reset() {
    std::memset(&svs, 0, sizeof(svs));
    std::memset(&gPlayerStates, 0, sizeof(gPlayerStates));
    sv_maxclients_storage.integer = 32;
    sv_botsPressAttackBtn_storage.enabled = 1;
    com_sv_running_storage.enabled = 1;
    gConnectCalls.clear();
    gGamestateCalls.clear();
    gGamestateDropsClient = false;
    gEnterWorldCalls.clear();
    gEnterWorldCmds.clear();
    gThinkCalls.clear();
    gCompareAddrOperands.clear();
    gDirectConnectMode = DC_ADMIT;
    gRandomScript.clear();
    gRandomCursor = 0;
    gAssertCount = 0;
    gArchiveTime_hook = 0;
    gTokenizeCalls = 0;
    gEndTokenizeCalls = 0;
    gTokenizeValue.clear();
    botport = 1;
}

static void FreeSlot(int index) {
    std::memset(&svs.clients[index], 0, sizeof(client_t));
}

static void ScriptDraws(const double *first, const double *last) {
    gRandomScript.assign(first, last);
    gRandomCursor = 0;
}

// A single bot on a single-slot server, ready for a SV_BotUserMove case.
static void OneBot() {
    Reset();
    sv_maxclients_storage.integer = 1;
    SV_AddTestClient();
    gThinkCalls.clear();
}

int main() {
    std::printf("== capacity: a full server creates nothing ==\n");
    Reset();
    sv_maxclients_storage.integer = 3;
    for (int i = 0; i < 3; ++i) svs.clients[i].header.state = CS_ACTIVE;
    Check(SV_AddTestClient() == nullptr, "no entity when every slot is taken");
    Check(gConnectCalls.empty(), "no connect attempt when every slot is taken");
    Check(gEnterWorldCalls.empty(), "nothing enters the world");

    std::printf("\n== capacity: bots fill free slots up to sv_maxclients ==\n");
    Reset();
    sv_maxclients_storage.integer = 3;
    gentity_s *e0 = SV_AddTestClient();
    gentity_s *e1 = SV_AddTestClient();
    gentity_s *e2 = SV_AddTestClient();
    Check(e0 && e1 && e2, "three bots on a three-slot server");
    Check(gConnectCalls.size() == 3, "exactly three connects");
    Check(SV_AddTestClient() == nullptr, "fourth bot refused at capacity");
    Check(gConnectCalls.size() == 3, "no connect attempted past capacity");
    Check(gGamestateCalls.size() == 3, "gamestate sent once per created bot");
    Check(gEnterWorldCalls.size() == 3, "enter-world once per created bot");
    Check(svs.clients[0].bIsTestClient && svs.clients[1].bIsTestClient
              && svs.clients[2].bIsTestClient,
          "every created client is flagged as a test client");

    std::printf("\n== capacity: real clients in the gaps keep their slots ==\n");
    Reset();
    sv_maxclients_storage.integer = 4;
    svs.clients[1].header.state = CS_ACTIVE;
    svs.clients[1].header.netchan.remoteAddress.type = NA_LOOPBACK;
    svs.clients[3].header.state = CS_ACTIVE;
    svs.clients[3].header.netchan.remoteAddress.type = NA_IP;
    Check(SV_AddTestClient() != nullptr && SV_AddTestClient() != nullptr,
          "two bots fit around two real clients");
    Check(gConnectCalls.size() == 2 && gConnectCalls[0].assignedSlot == 0
              && gConnectCalls[1].assignedSlot == 2,
          "the bots take the gaps 0 and 2, not the occupied slots");
    Check(svs.clients[1].header.state == CS_ACTIVE
              && svs.clients[1].header.netchan.remoteAddress.type == NA_LOOPBACK
              && svs.clients[3].header.state == CS_ACTIVE
              && svs.clients[3].header.netchan.remoteAddress.type == NA_IP,
          "the real clients are untouched");
    Check(!svs.clients[1].bIsTestClient && !svs.clients[3].bIsTestClient,
          "the real clients are not flagged as test clients");

    std::printf("\n== refusal: no admission means no client, no gamestate ==\n");
    Reset();
    sv_maxclients_storage.integer = 2;
    gDirectConnectMode = DC_REFUSE_FULL;
    Check(SV_AddTestClient() == nullptr, "no entity when the connect is refused");
    Check(gConnectCalls.size() == 1 && gConnectCalls[0].full,
          "the refusal was offered to SV_DirectConnect");
    Check(gGamestateCalls.empty() && gEnterWorldCalls.empty(),
          "nothing is sent to a client that does not exist");
    Check(gEndTokenizeCalls == 1, "the tokenized string is still released");

    Reset();
    sv_maxclients_storage.integer = 2;
    gDirectConnectMode = DC_ADMIT_MISMATCHED;
    Check(SV_AddTestClient() == nullptr,
          "no entity when the admitted client has another address");
    Check(gConnectCalls.size() == 1 && gConnectCalls[0].assignedSlot == 0,
          "the admission itself did happen");
    Check(gGamestateCalls.empty() && gEnterWorldCalls.empty(),
          "an unmatched client gets neither gamestate nor enter-world");

    std::printf("\n== address: synthesised netadr_t is fully defined ==\n");
    Reset();
    sv_maxclients_storage.integer = 3;
    SV_AddTestClient();
    SV_AddTestClient();
    SV_AddTestClient();
    for (size_t i = 0; i < gConnectCalls.size(); ++i) {
        // Compare the whole 20-byte object against a zero expectation. The
        // build uses -ftrivial-auto-var-init=pattern, so a byte the production
        // code failed to initialise holds the pattern instead of zero here.
        Check(DefinedBotAddress(gConnectCalls[i].from,
                                static_cast<unsigned short>(i + 1)),
              "bot address is NA_BOT with a zero ip/ipx and its own port");
    }
    bool lookupDefined = gCompareAddrOperands.size() >= gConnectCalls.size();
    for (size_t i = 0; i < gCompareAddrOperands.size(); ++i)
        lookupDefined = lookupDefined && WellDefinedBotAddress(gCompareAddrOperands[i]);
    Check(lookupDefined, "the address used for the client lookup is defined too");
    Check(gConnectCalls[0].from.port != gConnectCalls[1].from.port,
          "each bot gets a distinct source port");
    Check(gConnectCalls[1].from.port > gConnectCalls[0].from.port,
          "bot ports advance");

    std::printf("\n== userinfo: the fake connect string is well formed ==\n");
    const std::string &ui = gConnectCalls[0].tokenized;
    Check(ui.rfind("connect \"", 0) == 0, "starts with connect \"");
    Check(ui.find("\\protocol\\1") != std::string::npos, "advertises protocol 1");
    Check(ui.find("\\cl_punkbuster\\0") != std::string::npos, "declares no punkbuster");
    Check(ui.find("\\name\\bot") != std::string::npos, "names the bot after botport");
    Check(gTokenizeCalls == 3, "exactly one tokenize per bot, none left over");
    Check(gEndTokenizeCalls == 3, "every tokenize is released");

    std::printf("\n== enter world: zeroed usercmd, no stale input ==\n");
    Check(gEnterWorldCmds.size() == 3, "one usercmd per bot");
    bool zeroed = true;
    for (size_t i = 0; i < gEnterWorldCmds.size(); ++i) {
        const usercmd_s &c = gEnterWorldCmds[i];
        zeroed = zeroed && c.buttons == 0 && c.weapon == 0 && c.forwardmove == 0
            && c.rightmove == 0 && c.angles[0] == 0 && c.serverTime == 0;
    }
    Check(zeroed, "every bot enters the world with a zeroed usercmd");

    std::printf("\n== cleanup: an externally freed slot is reusable ==\n");
    Reset();
    sv_maxclients_storage.integer = 2;
    SV_AddTestClient();
    SV_AddTestClient();
    unsigned short secondPort = gConnectCalls[1].from.port;
    FreeSlot(0);  // slot 0 is freed from outside SV_AddTestClient (a memset)
    Check(SV_AddTestClient() != nullptr, "a freed slot can be filled again");
    Check(gConnectCalls.size() == 3, "the replacement connected");
    Check(gConnectCalls[2].assignedSlot == 0, "the lowest free slot is reused");
    Check(gConnectCalls[2].from.port > secondPort,
          "the replacement does not reuse the departed bot's port");
    Check(svs.clients[0].bIsTestClient == 1, "the replacement is a test client too");

    std::printf("\n== usercmd: weapon is carried as the byte the server sends ==\n");
    OneBot();
    SV_GameClientNum(0)->weapon = 42;
    SV_BotUserMove(&svs.clients[0]);
    Check(gThinkCalls.size() == 1, "one think per move");
    Check(!gThinkCalls.empty() && gThinkCalls[0].second.weapon == 42,
          "the bot's current weapon is sent");
    Check(svs.clients[0].header.deltaMessage
              == svs.clients[0].header.netchan.outgoingSequence - 1,
          "deltaMessage tracks the outgoing sequence");

    OneBot();
    SV_GameClientNum(0)->weapon = 0x1FF;   // does not fit the usercmd byte
    SV_BotUserMove(&svs.clients[0]);
    Check(gAssertCount == 1, "an out-of-byte weapon trips the production assert");
    Check(!gThinkCalls.empty() && gThinkCalls[0].second.weapon == 0xFF,
          "the usercmd carries the truncated byte, not the player state int");

    std::printf("\n== usercmd: no archived time means no fabricated input ==\n");
    OneBot();
    gArchiveTime_hook = 250;
    gRandomScript.assign(64, 0.0);  // every draw would trigger everything
    gRandomCursor = 0;
    SV_BotUserMove(&svs.clients[0]);
    Check(gThinkCalls.size() == 1, "still one think");
    const usercmd_s &calm = gThinkCalls.back().second;
    Check(calm.buttons == 0 && calm.forwardmove == 0 && calm.rightmove == 0
              && calm.angles[0] == 0 && calm.angles[1] == 0 && calm.angles[2] == 0,
          "no buttons, movement or angles while archive time is pending");
    Check(gRandomCursor == 0, "no random draws are consumed in that path");

    std::printf("\n== usercmd: attack is gated by sv_botsPressAttackBtn ==\n");
    OneBot();
    gRandomScript.assign(64, 0.0);
    gRandomCursor = 0;
    SV_BotUserMove(&svs.clients[0]);
    Check((gThinkCalls.back().second.buttons & BUTTON_ATTACK) != 0,
          "attack is pressed when the dvar allows it");
    Check((gThinkCalls.back().second.buttons & BUTTON_USE) != 0, "use is pressed");
    Check((gThinkCalls.back().second.buttons & BUTTON_USE_RELOAD) != 0,
          "reload rides with use");

    OneBot();
    sv_botsPressAttackBtn_storage.enabled = 0;
    gRandomScript.assign(64, 0.0);
    gRandomCursor = 0;
    SV_BotUserMove(&svs.clients[0]);
    Check((gThinkCalls.back().second.buttons & BUTTON_ATTACK) == 0,
          "no attack when the dvar is off");
    Check((gThinkCalls.back().second.buttons & BUTTON_USE) != 0,
          "use is unaffected by the attack dvar");

    std::printf("\n== usercmd: movement and angles follow the draw order ==\n");
    {
        // Every optional branch declines: the bot stays put.
        static const double draws[] = { 0.9, 0.9, 0.9, 0.9, 0.9, 0.9,
                                        0.9, 0.9, 0.9, 0.9 };
        OneBot();
        ScriptDraws(draws, draws + 10);
        SV_BotUserMove(&svs.clients[0]);
        const usercmd_s &still = gThinkCalls.back().second;
        Check(still.buttons == 0 && still.forwardmove == 0 && still.rightmove == 0
                  && still.angles[0] == 0 && still.angles[1] == 0
                  && still.angles[2] == 0,
              "high draws leave the bot stationary with no buttons or angles");
        Check(gRandomCursor == 9, "that path consumes exactly nine draws");
    }
    {
        // Every "< 0.33"/"< 0.5" branch taken: full speed forward and right.
        static const double draws[] = { 0.1, 0.1, 0.1, 0.1, 0.1, 0.1,
                                        0.1, 0.1, 0.1, 0.1 };
        OneBot();
        ScriptDraws(draws, draws + 10);
        SV_BotUserMove(&svs.clients[0]);
        const usercmd_s &moving = gThinkCalls.back().second;
        Check(moving.forwardmove == 127 && moving.rightmove == 127,
              "low draws give full forward/right movement");
        Check(moving.angles[0] == -360 && moving.angles[1] == -360
                  && moving.angles[2] == -360,
              "low draws also pick the negative crandom angle");
    }
    {
        // The backward branch: draw >= 0.33 then < 0.5 for forward only.
        static const double draws[] = { 0.9, 0.9, 0.9, 0.1, 0.9, 0.9,
                                        0.9, 0.9, 0.9 };
        OneBot();
        ScriptDraws(draws, draws + 9);
        SV_BotUserMove(&svs.clients[0]);
        const usercmd_s &backing = gThinkCalls.back().second;
        Check(backing.forwardmove == -127 && backing.rightmove == 0,
              "the second draw of a declined forward branch backs the bot up");
        Check(backing.buttons == 0 && backing.angles[0] == 0,
              "backing up alone leaves buttons and angles clear");
    }
    {
        // Mixed crandom signs: the angle sign follows the draw, not the branch.
        static const double draws[] = { 0.9, 0.9, 0.9, 0.9, 0.9, 0.9,
                                        0.1, 0.1, 0.1, 0.9, 0.1, 0.9 };
        OneBot();
        ScriptDraws(draws, draws + 12);
        SV_BotUserMove(&svs.clients[0]);
        const usercmd_s &turning = gThinkCalls.back().second;
        Check(turning.angles[0] == -360 && turning.angles[1] == 360
                  && turning.angles[2] == 360,
              "each angle takes the sign of its own crandom draw");
        Check(turning.forwardmove == 0 && turning.rightmove == 0,
              "turning alone does not fabricate movement");
    }

    std::printf("\n== dispatch: only connected bots are driven ==\n");
    Reset();
    sv_maxclients_storage.integer = 4;
    SV_AddTestClient();                                    // 0: NA_BOT, active
    svs.clients[1].header.state = CS_ACTIVE;               // 1: real loopback client
    svs.clients[1].header.netchan.remoteAddress.type = NA_LOOPBACK;
    svs.clients[2].header.state = CS_ACTIVE;               // 2: real IP client
    svs.clients[2].header.netchan.remoteAddress.type = NA_IP;
    FreeSlot(3);                                           // 3: free slot
    gThinkCalls.clear();
    SV_UpdateBots();
    Check(gThinkCalls.size() == 1, "exactly one client was driven");
    Check(!gThinkCalls.empty() && gThinkCalls[0].first == 0, "only the NA_BOT client");
    Check(gResetSkeletonCacheCalls > 0, "the skeleton cache is reset per frame");

    std::printf("\n== retained bots re-enter after full map load ==\n");
    Reset();
    sv_maxclients_storage.integer = 8;
    for (int i = 0; i < 8; ++i) {
        svs.clients[i].header.netchan.remoteAddress.type = NA_BOT;
        svs.clients[i].bIsTestClient = 1;
        svs.clients[i].gentity = &gEntities[i];
    }
    svs.clients[0].header.state = CS_CONNECTED;
    svs.clients[1].header.state = CS_ACTIVE;
    svs.clients[2].header.state = CS_ZOMBIE;
    svs.clients[3].header.state = CS_FREE;
    svs.clients[4].header.state = CS_CONNECTED;
    svs.clients[4].header.netchan.remoteAddress.type = NA_IP;
    svs.clients[5].header.state = CS_CONNECTED;
    svs.clients[5].header.netchan.remoteAddress.type = NA_LOOPBACK;
    svs.clients[6].header.state = CS_CONNECTED;
    svs.clients[6].bIsTestClient = 0;
    svs.clients[7].header.state = CS_CLIENTLOADING;
    SV_UpdateBots();
    Check(gGamestateCalls == std::vector<int>{0}, "only retained connected internal bot gets a new gamestate");
    Check(gEnterWorldCalls == std::vector<int>{0}, "only successfully primed bot enters world");
    usercmd_s zero{};
    Check(gEnterWorldCmds.size() == 1 && !std::memcmp(&gEnterWorldCmds[0], &zero, sizeof(zero)),
          "new map begins with a completely zeroed usercmd");
    Check(gThinkCalls.size() == 2 && gThinkCalls[0].first == 0 && gThinkCalls[1].first == 1,
          "only active bots think; zombie, free and unprimed clients do not");
    SV_UpdateBots();
    Check(gGamestateCalls.size() == 1 && gEnterWorldCalls.size() == 1, "active bots are not re-entered every frame");
    gGamestateDropsClient = true;
    svs.clients[0].header.state = CS_CONNECTED;
    gThinkCalls.clear();
    SV_UpdateBots();
    Check(svs.clients[0].header.state == CS_ZOMBIE && gEnterWorldCalls.size() == 1,
          "gamestate failure cannot resurrect a dropped bot");
    Check(gThinkCalls.size() == 1 && gThinkCalls[0].first == 1, "dropped bot receives no movement");

    std::printf("\n== server-not-running is asserted, not silently accepted ==\n");
    Reset();
    com_sv_running_storage.enabled = 0;
    Check(gAssertCount == 0, "no assertion before the call");
    SV_AddTestClient();
    Check(gAssertCount == 1, "adding a test client with no server asserts");

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nAll server test-client invariants hold.\n");
    return 0;
}
"""

# Proves that -ftrivial-auto-var-init=pattern is actually applied to an
# uninitialised stack object by this compiler and this -m32/-m64 setting. The
# address canary above is worthless without it, so the probe is mandatory.
PATTERN_PROBE = r"""
#include <cstring>
#include <cstdio>
struct probe_t { unsigned char bytes[32]; };
int main() {
    probe_t p;                                  // deliberately not initialised
    unsigned char zero[sizeof(p)];
    std::memset(zero, 0, sizeof(zero));
    bool uniform = true;
    for (unsigned i = 1; i < sizeof(p.bytes); ++i)
        uniform = uniform && p.bytes[i] == p.bytes[0];
    std::printf("auto-var-init pattern byte = 0x%02x\n", unsigned(p.bytes[0]));
    return (p.bytes[0] != 0 && uniform) ? 0 : 1;
}
"""

AUTO_VAR_INIT_FLAG = "-ftrivial-auto-var-init=pattern"


def compile_flags(cxx: str, m32: bool, sanitize: bool) -> list:
    flags = [cxx, "-std=c++17", "-g", "-O1", "-fno-omit-frame-pointer",
             "-Wall", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
             AUTO_VAR_INIT_FLAG]
    if m32:
        flags.append("-m32")
    if sanitize:
        # -fno-sanitize-recover=all is what makes a finding fatal. Without it
        # UBSan prints and carries on, so a misaligned store or another
        # undefined behaviour in the extracted production text could leave the
        # process exiting 0 and turn a real defect into a green run.
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    return flags


def probe_pattern_init(cxx: str, m32: bool, workdir: Path) -> None:
    """Fail loudly unless this compiler really pattern-initialises stack locals."""
    source = workdir / "probe_auto_var_init.cpp"
    binary = workdir / "probe_auto_var_init"
    source.write_text(PATTERN_PROBE)
    command = [cxx, "-std=c++17", "-O1", AUTO_VAR_INIT_FLAG]
    if m32:
        command.append("-m32")
    command += [str(source), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode != 0:
        raise SystemExit(
            "this test needs a compiler that supports "
            f"{AUTO_VAR_INIT_FLAG} (clang >= 12 or gcc >= 12):\n"
            + built.stderr.strip()
            + "\nWithout it an uninitialised stack object cannot be told apart "
              "from a cleared one, so the address claim would be unprovable."
        )
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(
            f"{cxx} accepted {AUTO_VAR_INIT_FLAG} but did not apply it to an "
            "uninitialised stack object, so the address canary cannot detect a "
            "missing initialisation. Refusing to report a green run."
        )
    print("  " + run.stdout.strip())


def build_and_run(cxx: str, m32: bool, sanitize: bool, workdir: Path,
                  mutate=None) -> bool:
    shim_dir = workdir / "shim"
    shim_dir.mkdir()
    (shim_dir / "shim.h").write_text(SHIM_HEADER + "\nextern int gArchiveTime_hook;\n")
    (shim_dir / "shim_state.cpp").write_text('#include "shim.h"\n' + SHIM_SOURCE)

    extracted = []
    for path, name, start in FUNCTIONS:
        extracted.append(extract_definition(path.read_text(), name, start, path))
    production_text = "\n\n".join(extracted)
    if mutate is not None:
        label, needle, replacement = mutate
        if production_text.count(needle) != 1:
            raise SystemExit(
                f"mutation {label!r} no longer applies: expected exactly one "
                f"{needle!r} in the extracted production text, found "
                f"{production_text.count(needle)}. Update MUTATIONS with the "
                "current initialisation code instead of letting it pass silently."
            )
        production_text = production_text.replace(needle, replacement)
    (shim_dir / "production.cpp").write_text(
        '#include "shim.h"\n#include <cstring>\n#include <cstdio>\n\n'
        + production_text
        + "\n"
    )
    (shim_dir / "test.cpp").write_text(TEST_SOURCE)

    binary = workdir / "test_server_test_clients"
    command = compile_flags(cxx, m32, sanitize)
    command += [str(shim_dir / "production.cpp"), str(shim_dir / "shim_state.cpp"),
                str(shim_dir / "test.cpp"), "-o", str(binary)]
    print("+ " + " ".join(command))
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode != 0:
        print(built.stdout)
        print(built.stderr)
        print("FAIL: the extracted production code did not compile against the doubles")
        return False
    if built.stderr.strip():
        print(built.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    print(run.stdout, end="")
    if run.stderr.strip():
        print(run.stderr, end="")
    return run.returncode == 0


def run_mutations(cxx: str, m32: bool, sanitize: bool) -> bool:
    """Rebuild damaged temporary copies and require every one of them to fail."""
    print("\n== mutations: the address claim is falsifiable ==")
    ok = True
    for label, needle, replacement in MUTATIONS:
        print(f"\n-- mutation: {label}")
        with tempfile.TemporaryDirectory(prefix="kisak-testclients-mut-") as temporary:
            passed = build_and_run(cxx, m32, sanitize, Path(temporary),
                                   mutate=(label, needle, replacement))
        if passed:
            print(f"FAIL: mutation {label!r} still produced a green run")
            ok = False
        else:
            print(f"PROVEN: mutation {label!r} is caught")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--m32", action="store_true", help="build 32-bit (the shipped server target)")
    parser.add_argument("--sanitize", action="store_true",
                        help="add AddressSanitizer and UndefinedBehaviorSanitizer with "
                             "recovery disabled, so any finding aborts the run")
    parser.add_argument("--mutations", action="store_true",
                        help="also rebuild temporary copies with the address initialisation "
                             "removed or replaced by a misaligned store, and require each to fail")
    args = parser.parse_args()

    if args.mutations and not args.sanitize:
        print("note: --mutations is most meaningful together with --sanitize, because the "
              "misaligned-store mutation relies on UBSan to be fatal\n")

    print(f"== pattern initialisation probe ({'32-bit' if args.m32 else '64-bit'}) ==")
    with tempfile.TemporaryDirectory(prefix="kisak-testclients-probe-") as temporary:
        probe_pattern_init(args.cxx, args.m32, Path(temporary))

    with tempfile.TemporaryDirectory(prefix="kisak-testclients-") as temporary:
        ok = build_and_run(args.cxx, args.m32, args.sanitize, Path(temporary))
    if not ok:
        return 1
    if args.mutations:
        if not run_mutations(args.cxx, args.m32, args.sanitize):
            return 1
    print("\nAsset-free server test-client invariants passed.")
    print("Not covered here: real SV_DirectConnect admission/auth, SV_DropClient, map load, "
          "script spawns/team/class, entity state, snapshots, shutdown. "
          "A test client is not a client and this is not an AI.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
