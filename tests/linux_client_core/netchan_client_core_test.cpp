// Asset-free native test for the real client/server netchan command codec.
//
// This executable links the *production* translation units
//   src/client_mp/cl_net_chan_mp.cpp   (CL_Netchan_Encode / CL_Netchan_Decode)
//   src/server_mp/sv_net_chan_mp.cpp   (SV_Netchan_Encode / SV_Netchan_Decode)
// unmodified, against the production headers. Nothing is extracted, copied or
// re-implemented from those files: the only code here is the small set of
// boundary symbols the codec calls into, plus test scenarios.
//
// Boundary doubles (deliberately tiny, see docs/linux-client.md):
//   clients[]                        - the real clientActive_t array the inline
//                                      CL_GetLocalClientGlobals() returns from
//   CL_GetLocalClientConnection()    - returns an owned clientConnection_t
//   MyAssertHandler()                - aborts loudly; iassert() is live here so
//                                      the codec's own invariants are enforced
// Everything else the two TUs reference (NetProf_*, Netchan_Transmit, Com_Printf,
// CL_DrawString, net_profile, svs, sv_maxclients, ...) is only reachable from
// functions this test never calls, and is discarded by -ffunction-sections +
// -Wl,--gc-sections rather than being stubbed.

#ifndef KISAK_MP
#error This test is MultiPlayer only
#endif

#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <qcommon/net_chan_mp.h>
#include <client_mp/client_mp.h>
#include <server_mp/server_mp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// ---------------------------------------------------------------------------
// Boundary doubles
// ---------------------------------------------------------------------------

clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];

static clientConnection_t s_connection[STATIC_MAX_LOCAL_CLIENTS];

clientConnection_t *__cdecl CL_GetLocalClientConnection(int32_t localClientNum)
{
    iassert(localClientNum >= 0 && localClientNum < STATIC_MAX_LOCAL_CLIENTS);
    return &s_connection[localClientNum];
}

// iassert is compiled in for this harness (RELEASE_ASSERTS), so the production
// invariants inside the codec are actually evaluated. A violation must fail the
// run, never be swallowed.
void MyAssertHandler(const char *filename, int line, int type, const char *fmt, ...)
{
    va_list ap;
    std::printf("FATAL: assertion fired at %s:%d (type %d): ", filename, line, type);
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
    std::abort();
}

// SV_Netchan_Encode has no header declaration (it is only called from its own
// TU), so the test declares the production signature itself.
void __cdecl SV_Netchan_Encode(client_t *client, uint8_t *data, int size);

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

static void fail(const char *what, const char *detail)
{
    std::printf("FAIL: %s: %s\n", what, detail);
    ++g_failures;
}

static void ok(const char *what)
{
    ++g_checks;
    std::printf("ok: %s\n", what);
}

static void check_bytes(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    if (std::memcmp(got, want, n) != 0)
    {
        char buf[512];
        int used = std::snprintf(buf, sizeof(buf), "want");
        for (size_t i = 0; i < n && used < (int)sizeof(buf) - 8; ++i)
            used += std::snprintf(buf + used, sizeof(buf) - used, " %02x", want[i]);
        used += std::snprintf(buf + used, sizeof(buf) - used, " got");
        for (size_t i = 0; i < n && used < (int)sizeof(buf) - 8; ++i)
            used += std::snprintf(buf + used, sizeof(buf) - used, " %02x", got[i]);
        fail(what, buf);
        return;
    }
    ok(what);
}

static void check_true(const char *what, bool cond, const char *detail = "condition false")
{
    if (!cond)
    {
        fail(what, detail);
        return;
    }
    ok(what);
}

// Both sides of a real connection share the challenge and server id; the
// command ring the client mirrored and the server's record of what it sent hold
// the same text. These helpers build exactly that aligned state.
static void reset_state()
{
    std::memset(&s_connection[0], 0, sizeof(s_connection[0]));
    std::memset(&clients[0], 0, sizeof(clients[0]));
}

// The client picks the key text with the *command* sequence and mixes the
// message sequence into the key, so both have to be seeded deliberately.
static void seed_client_command(clientConnection_t &clc, int seq, const char *cmd)
{
    std::snprintf(clc.serverCommands[seq & (MAX_RELIABLE_COMMANDS - 1)],
                  sizeof(clc.serverCommands[0]), "%s", cmd);
    clc.serverCommandSequence = seq;
}

static void seed_server_command(client_t &sv, int ack, const char *cmd)
{
    std::snprintf(sv.reliableCommandInfo[ack & (MAX_RELIABLE_COMMANDS - 1)].cmd,
                  sizeof(sv.reliableCommandInfo[0].cmd), "%s", cmd);
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

// Bytes below are independently calculated vectors: they follow from the
// documented key-stream XOR written out above each case, and were worked out
// separately from this codebase rather than captured from the code under test.
// That way a change to the transform fails here instead of being blessed.
static void test_golden_vectors()
{
    // client -> server direction, CL_Netchan_Encode
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x13;
    clients[0].serverId = 0x2A;
    clc.serverMessageSequence = 0x07;
    seed_client_command(clc, 0x07, "abc");

    uint8_t buf[5] = {0x00, 0x11, 0x22, 0x33, 0x44};
    CL_Netchan_Encode(buf, 5);
    const uint8_t wantClient[5] = {0x5f, 0x8a, 0xda, 0x09, 0x1c};
    check_bytes("golden client encode (challenge^serverId^serverMessageSequence)", buf, wantClient, 5);

    // server -> client direction, SV_Netchan_Encode
    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = 0x13;
    sv->header.netchan.outgoingSequence = 0x07;
    std::snprintf(sv->lastClientCommandString, sizeof(sv->lastClientCommandString), "abc");

    uint8_t buf2[5] = {0x00, 0x11, 0x22, 0x33, 0x44};
    SV_Netchan_Encode(sv, buf2, 5);
    const uint8_t wantServer[5] = {0x75, 0xa0, 0xf0, 0x23, 0x36};
    check_bytes("golden server encode (challenge^outgoingSequence)", buf2, wantServer, 5);
    delete sv;
}

// An empty command entry makes the index reset every iteration, so the key
// never advances: the payload is XORed with one constant byte.
static void test_empty_command()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x13;
    clients[0].serverId = 0x2A;
    clc.serverMessageSequence = 0x07;
    seed_client_command(clc, 0x07, "");

    uint8_t buf[5] = {0x00, 0x11, 0x22, 0x33, 0x44};
    CL_Netchan_Encode(buf, 5);
    const uint8_t want[5] = {0x3e, 0x2f, 0x1c, 0x0d, 0x7a};
    check_bytes("client encode with empty command is a constant-XOR", buf, want, 5);

    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = 0x13;
    sv->header.netchan.outgoingSequence = 0x07;
    sv->lastClientCommandString[0] = '\0';
    uint8_t buf2[5] = {0x00, 0x11, 0x22, 0x33, 0x44};
    SV_Netchan_Encode(sv, buf2, 5);
    const uint8_t want2[5] = {0x14, 0x05, 0x36, 0x27, 0x50};
    check_bytes("server encode with empty command is a constant-XOR", buf2, want2, 5);
    delete sv;
}

// The client encodes with the server command ring it received; the server
// decodes with its own record of the command it sent. When the mirrored text,
// challenge, server id and sequence/ack agree, decode must reproduce the
// plaintext exactly.
static void test_client_to_server_roundtrip()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0xA5A5;
    clients[0].serverId = 0x5A;
    clc.serverMessageSequence = 41;
    seed_client_command(clc, 41, "set sv_hostname kisak");

    const uint8_t plain[9] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t wire[9];
    std::memcpy(wire, plain, sizeof(wire));
    CL_Netchan_Encode(wire, 9);

    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = clc.challenge;
    sv->serverId = 0x5A;
    sv->messageAcknowledge = clc.serverMessageSequence;
    sv->reliableAcknowledge = 41;
    seed_server_command(*sv, 41, "set sv_hostname kisak");

    SV_Netchan_Decode(sv, wire, 9);
    check_true("client encode -> server decode restores plaintext",
               std::memcmp(wire, plain, sizeof(plain)) == 0);
    delete sv;
}

// Mirror image: the server encodes with the last client command it saw, the
// client decodes with its reliable command ring.
static void test_server_to_client_roundtrip()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x1234;
    clc.serverMessageSequence = 77;
    clc.reliableAcknowledge = 77;
    std::snprintf(clc.reliableCommands[77 & (MAX_RELIABLE_COMMANDS - 1)],
                  sizeof(clc.reliableCommands[0]), "cg_scoreboard 1");

    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = 0x1234;
    sv->header.netchan.outgoingSequence = 77;
    std::snprintf(sv->lastClientCommandString, sizeof(sv->lastClientCommandString), "cg_scoreboard 1");

    const uint8_t plain[6] = {0x00, 0xff, 0x10, 0x20, 0x30, 0x40};
    uint8_t wire[6];
    std::memcpy(wire, plain, sizeof(wire));
    SV_Netchan_Encode(sv, wire, 6);

    CL_Netchan_Decode(wire, 6);
    check_true("server encode -> client decode restores plaintext",
               std::memcmp(wire, plain, sizeof(plain)) == 0);
    delete sv;
}

// The client and server each keep a 128-entry command ring, indexed with
// & (MAX_RELIABLE_COMMANDS - 1). The ring *selector* and the *key material* are
// different fields, and these cases pin that distinction:
//
//   CL_Netchan_Encode : ring <- serverCommandSequence, key <- serverMessageSequence
//   CL_Netchan_Decode : ring <- reliableAcknowledge,   key <- serverMessageSequence
//   SV_Netchan_Decode : ring <- reliableAcknowledge,   key <- messageAcknowledge
//
// A roundtrip cannot show this on its own: the two sides only agree when the
// selector value and the key value are equal, which is exactly the conflation
// being ruled out. So each case below holds the key field fixed and moves only
// the selector, with different text parked in the competing slots.

// Independently calculated: the selector is 128 (which wraps to slot 0) while
// the key field stays at 41, and slot 41 holds different text than slot 0.
// Reading the key field as the selector would produce the slot-41 stream.
static void test_ring_selector_golden()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0xC3;
    clients[0].serverId = 0x2F;
    clc.serverMessageSequence = 41;         // key material, deliberately != selector

    std::snprintf(clc.serverCommands[0], sizeof(clc.serverCommands[0]), "zero");
    std::snprintf(clc.serverCommands[41], sizeof(clc.serverCommands[0]), "fortyone");
    clc.serverCommandSequence = 128;        // selects slot 0

    const uint8_t plain[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t wire[6];
    std::memcpy(wire, plain, sizeof(wire));
    CL_Netchan_Encode(wire, 6);

    // key = challenge ^ serverId ^ serverMessageSequence = 0xC3 ^ 0x2F ^ 0x29 = 0xC5
    const uint8_t want[6] = {0xbf, 0x64, 0x25, 0xea, 0xe7, 0x3c};
    check_bytes("golden client encode: selector 128 -> slot 0, key field 41", wire, want, 6);
}

// Same key material throughout; only the selector crosses 127 -> 128. The ring
// must move, so the ciphertext must change even though challenge, serverId and
// message sequence never do.
static void test_command_ring_wrap()
{
    check_true("reliable command ring is 128 entries", MAX_RELIABLE_COMMANDS == 128);

    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x0F0F;
    clients[0].serverId = 0x33;
    clc.serverMessageSequence = 41;         // held fixed across both encodes

    std::snprintf(clc.serverCommands[127], sizeof(clc.serverCommands[0]), "last");
    std::snprintf(clc.serverCommands[0], sizeof(clc.serverCommands[0]), "zero");

    clc.serverCommandSequence = 127;
    uint8_t a[8];
    std::memset(a, 0, sizeof(a));
    CL_Netchan_Encode(a, 8);

    clc.serverCommandSequence = 128;
    uint8_t b[8];
    std::memset(b, 0, sizeof(b));
    CL_Netchan_Encode(b, 8);

    check_true("selector 127 vs 128 picks different ring slots (key field fixed)",
               std::memcmp(a, b, sizeof(a)) != 0);

    // And the roundtrip must still hold exactly at the wrap point.
    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = clc.challenge;
    sv->serverId = 0x33;
    sv->messageAcknowledge = clc.serverMessageSequence;
    sv->reliableAcknowledge = 128;
    sv->reliableSequence = 129;
    seed_server_command(*sv, 128, "zero");

    const uint8_t plain[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t wire[8];
    std::memcpy(wire, plain, sizeof(wire));
    CL_Netchan_Encode(wire, 8);
    SV_Netchan_Decode(sv, wire, 8);
    check_true("roundtrip holds across the wrap (selector 128 -> slot 0)",
               std::memcmp(wire, plain, sizeof(plain)) == 0);
    delete sv;
}

// CL_Netchan_Decode selects with reliableAcknowledge and keys with
// serverMessageSequence. Hold the key field, move the selector.
static void test_client_decode_ring_selector()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0xC3;
    clc.serverMessageSequence = 41;         // key field, fixed

    std::snprintf(clc.reliableCommands[0], sizeof(clc.reliableCommands[0]), "zero");
    std::snprintf(clc.reliableCommands[41], sizeof(clc.reliableCommands[0]), "fortyone");

    const uint8_t plain[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    // key = challenge ^ serverMessageSequence = 0xC3 ^ 0x29 = 0xEA
    clc.reliableAcknowledge = 128;          // slot 0
    uint8_t wire[6];
    std::memcpy(wire, plain, sizeof(wire));
    CL_Netchan_Decode(wire, 6);
    const uint8_t wantSlot0[6] = {0x90, 0x4b, 0x0a, 0xc5, 0xc8, 0x13};
    check_bytes("golden client decode: selector 128 -> slot 0", wire, wantSlot0, 6);

    clc.reliableAcknowledge = 41;           // slot 41
    uint8_t wire41[6];
    std::memcpy(wire41, plain, sizeof(wire41));
    CL_Netchan_Decode(wire41, 6);
    const uint8_t wantSlot41[6] = {0x8c, 0x43, 0x02, 0xfb, 0xf5, 0x3a};
    check_bytes("golden client decode: selector 41 -> slot 41", wire41, wantSlot41, 6);

    check_true("client decode selects with reliableAcknowledge, not the message sequence",
               std::memcmp(wire, wantSlot41, sizeof(wire)) != 0);
}

// SV_Netchan_Decode selects with reliableAcknowledge and keys with
// messageAcknowledge (plus serverId), which is a third arrangement again. The
// production iassert requires reliableSequence - reliableAcknowledge to stay in
// [0, MAX_RELIABLE_COMMANDS), so a valid pair is set for each selector.
static void test_server_decode_ring_and_ack()
{
    reset_state();
    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = 0xC3;
    sv->serverId = 0x2F;
    sv->messageAcknowledge = 41;            // key field, fixed

    std::snprintf(sv->reliableCommandInfo[0].cmd,
                  sizeof(sv->reliableCommandInfo[0].cmd), "zero");
    std::snprintf(sv->reliableCommandInfo[41].cmd,
                  sizeof(sv->reliableCommandInfo[0].cmd), "fortyone");

    const uint8_t plain[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    // key = challenge ^ serverId ^ messageAcknowledge = 0xC3 ^ 0x2F ^ 0x29 = 0xC5
    sv->reliableAcknowledge = 128;          // slot 0
    sv->reliableSequence = 129;             // >= ack, difference 1 < 128
    uint8_t wire[6];
    std::memcpy(wire, plain, sizeof(wire));
    SV_Netchan_Decode(sv, wire, 6);
    const uint8_t wantSlot0[6] = {0xbf, 0x64, 0x25, 0xea, 0xe7, 0x3c};
    check_bytes("golden server decode: selector 128 -> slot 0", wire, wantSlot0, 6);

    sv->reliableAcknowledge = 41;           // slot 41
    sv->reliableSequence = 42;
    uint8_t wire41[6];
    std::memcpy(wire41, plain, sizeof(wire41));
    SV_Netchan_Decode(sv, wire41, 6);
    const uint8_t wantSlot41[6] = {0xa3, 0x6c, 0x2d, 0xd4, 0xda, 0x15};
    check_bytes("golden server decode: selector 41 -> slot 41", wire41, wantSlot41, 6);

    check_true("server decode selects with reliableAcknowledge, not messageAcknowledge",
               std::memcmp(wire, wantSlot41, sizeof(wire)) != 0);

    // The ack is key material here: move only it, keep the selector settled.
    sv->reliableAcknowledge = 128;
    sv->reliableSequence = 129;
    sv->messageAcknowledge = 42;
    uint8_t wireAck[6];
    std::memcpy(wireAck, plain, sizeof(wireAck));
    SV_Netchan_Decode(sv, wireAck, 6);
    check_true("server decode keys on messageAcknowledge (ack alone changes ciphertext)",
               std::memcmp(wireAck, wantSlot0, sizeof(wireAck)) != 0);
    delete sv;
}

// challenge, serverId, message sequence and ack are all key material: changing
// any one of them must change the ciphertext.
static void test_key_material_sensitivity()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x11;
    clients[0].serverId = 0x22;
    clc.serverMessageSequence = 0x1;

    auto enc = [&](uint8_t *dst, size_t n) {
        std::memset(dst, 0x41, n);
        CL_Netchan_Encode(dst, (int)n);
    };

    uint8_t base[16];
    seed_client_command(clc, 0x1, "challenge");
    enc(base, sizeof(base));

    uint8_t v[16];
    clc.challenge ^= 0x01;
    enc(v, sizeof(v));
    check_true("challenge changes the key stream", std::memcmp(base, v, sizeof(base)) != 0);
    clc.challenge ^= 0x01;

    clients[0].serverId ^= 0x01;
    enc(v, sizeof(v));
    check_true("serverId changes the key stream", std::memcmp(base, v, sizeof(base)) != 0);
    clients[0].serverId ^= 0x01;

    // Only the message sequence moves here; the command text and the ring slot
    // that selects it stay put, so this isolates the key contribution.
    clc.serverMessageSequence += 1;
    enc(v, sizeof(v));
    check_true("message sequence changes the key stream", std::memcmp(base, v, sizeof(base)) != 0);
    clc.serverMessageSequence -= 1;

    // And with the key material restored the stream must reproduce exactly.
    enc(v, sizeof(v));
    check_true("restoring key material reproduces the stream",
               std::memcmp(base, v, sizeof(base)) == 0);
}

// Zero-length payloads must not touch memory.
static void test_zero_length()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x77;
    seed_client_command(clc, 0, "guard");
    uint8_t sentinel[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CL_Netchan_Encode(sentinel, 0);
    const uint8_t want[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    check_bytes("zero-length client encode writes nothing", sentinel, want, 4);

    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = 0x77;
    std::snprintf(sv->lastClientCommandString, sizeof(sv->lastClientCommandString), "guard");
    SV_Netchan_Encode(sv, sentinel, 0);
    check_bytes("zero-length server encode writes nothing", sentinel, want, 4);
    delete sv;
}

// A payload longer than the command text exercises several ring wraps and the
// odd/even shift alternation over a long run.
static void test_long_payload_many_wraps()
{
    reset_state();
    clientConnection_t &clc = s_connection[0];
    clc.challenge = 0x9C;
    clients[0].serverId = 0x61;
    clc.serverMessageSequence = 5;
    seed_client_command(clc, 5, "ab");

    client_t *sv = new client_t();
    std::memset(sv, 0, sizeof(client_t));
    sv->challenge = clc.challenge;
    sv->serverId = 0x61;
    sv->messageAcknowledge = 5;
    sv->reliableAcknowledge = 5;
    seed_server_command(*sv, 5, "ab");

    const size_t n = 1024;
    std::vector<uint8_t> plain(n), wire(n);
    for (size_t i = 0; i < n; ++i)
        plain[i] = (uint8_t)(i * 7 + 3);
    std::memcpy(wire.data(), plain.data(), n);

    CL_Netchan_Encode(wire.data(), (int)n);
    check_true("1024-byte payload is transformed",
               std::memcmp(wire.data(), plain.data(), n) != 0);
    SV_Netchan_Decode(sv, wire.data(), (int)n);
    check_true("1024-byte payload roundtrips over many command wraps",
               std::memcmp(wire.data(), plain.data(), n) == 0);
    delete sv;
}

int main()
{
    std::printf("== native client core: netchan command codec ==\n");
    test_golden_vectors();
    test_empty_command();
    test_client_to_server_roundtrip();
    test_server_to_client_roundtrip();
    // Ring selector vs key material: these are the cases a roundtrip cannot
    // give you, because a roundtrip only agrees when the two fields are equal.
    test_ring_selector_golden();
    test_command_ring_wrap();
    test_client_decode_ring_selector();
    test_server_decode_ring_and_ack();
    test_key_material_sensitivity();
    test_zero_length();
    test_long_payload_many_wraps();

    std::printf("-- %d checks, %d failures --\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
