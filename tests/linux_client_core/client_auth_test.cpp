#include <client_mp/cl_auth.h>
#include <client_mp/client_mp.h>
#include <universal/assertive.h>
#include <win32/win_storage.h>
#include <cstdlib>
#include <algorithm>
#include <qcommon/huffman.h>
#include <cstdarg>
#include <universal/base64.h>
#include <cstdio>
#include <cstring>
#include <string>

// Resend test host boundary; the actual Linux provider and state machine run.
clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
clientStatic_t cls;
static clientConnection_t connection;
clientConnection_t *CL_GetLocalClientConnection(int n) { iassert(n == 0); return &connection; }
static dvar_t zeroDvar{};
const dvar_t *net_lanauthorize = &zeroDvar, *onlinegame = &zeroDvar;
int32_t dvar_modifiedFlags;
huffman_t msgHuff;
uint32_t Sys_Milliseconds() { std::abort(); } // Stats branch is not entered.
void *Sys_GetValue(int) { std::abort(); } // va() in the connect branch is not entered.
void _copyDWord(uint32_t *dest, uint32_t value, uint32_t count) { std::fill_n(dest, count, value); }
static int sends, authErrors;
void MyAssertHandler(const char *, int, int, const char *, ...) { std::abort(); }
void Com_Error(errorParm_t, const char *, ...) { std::abort(); }
void Com_Printf(int, const char *, ...) {}
void Com_PrintError(int, const char *format, ...) {
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    iassert(std::strstr(text, "authentication unavailable")); ++authErrors;
}
bool Sys_IsLANAddress(netadr_t) { return true; }
void CL_RequestAuthorization(int) { std::abort(); } // LAN configuration excludes legacy auth.
bool NET_OutOfBandPrint(netsrc_t, netadr_t, const char *) { ++sends; return true; }
bool NET_OutOfBandData(netsrc_t, netadr_t, const uint8_t *, int) { ++sends; return true; }
char *Dvar_InfoString(int, char) { std::abort(); }
bool LiveStorage_DoWeHaveStats() { std::abort(); }
playerStatNetworkData *LiveStorage_GetStatBuffer() { std::abort(); }

static int checks, failures;
static void check(bool value, const char *name)
{
    ++checks;
    if (!value) ++failures;
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
}

int main()
{
    // Synthetic bytes exist only in this unit fixture, never in the executable
    // provider or in any socket send path.
    const unsigned char ticket[] = {0, 255, 1};
    char output[1536];
    auto result = CL_FormatSteamChallenge(ticket, sizeof(ticket), 76561198000000000ULL, output, sizeof(output));
    check(result == ClientAuthResult::ready && std::strcmp(output,
        "getchallenge 0 \"AP8B\" \"76561198000000000\"") == 0, "existing challenge wire format including embedded-zero ticket");
    std::string expected(output);
    check(CL_FormatSteamChallenge(ticket, sizeof(ticket), 76561198000000000ULL, output, expected.size() + 1)
        == ClientAuthResult::ready && expected == output, "exact output capacity includes terminator");
    check(CL_FormatSteamChallenge(ticket, sizeof(ticket), 76561198000000000ULL, output, expected.size())
        == ClientAuthResult::bufferTooSmall && output[0] == 0, "short output fails closed without partial request");
    check(CL_FormatSteamChallenge(nullptr, 3, 1, output, sizeof(output)) == ClientAuthResult::invalidTicket && !output[0], "null ticket rejected");
    check(CL_FormatSteamChallenge(ticket, 0, 1, output, sizeof(output)) == ClientAuthResult::invalidTicket && !output[0], "empty ticket rejected");
    check(CL_FormatSteamChallenge(ticket, 1025, 1, output, sizeof(output)) == ClientAuthResult::invalidTicket && !output[0], "oversize rejected before ticket reads");
    check(CL_FormatSteamChallenge(ticket, 3, 0, output, sizeof(output)) == ClientAuthResult::invalidTicket && !output[0], "missing identity rejected");
    check(CL_FormatSteamChallenge(ticket, 3, 1, nullptr, 10) == ClientAuthResult::bufferTooSmall, "null output rejected");
    output[0] = 'x';
    check(CL_FormatSteamChallenge(ticket, 3, 1, output, 0) == ClientAuthResult::bufferTooSmall && output[0] == 'x', "zero capacity does not write");
    unsigned char maximum[1024], decoded[1024];
    for (unsigned i = 0; i < sizeof(maximum); ++i) maximum[i] = i;
    result = CL_FormatSteamChallenge(maximum, sizeof(maximum), UINT64_MAX, output, sizeof(output));
    char *start = std::strchr(output, '"');
    char *end = start ? std::strchr(start + 1, '"') : nullptr;
    bool roundtrip = result == ClientAuthResult::ready && start && end
        && b64_decode(reinterpret_cast<unsigned char *>(start + 1), end - start - 1, decoded) == sizeof(maximum)
        && !std::memcmp(maximum, decoded, sizeof(maximum));
    check(roundtrip, "maximum supported binary ticket roundtrips with full-width identity");
    for (unsigned length = 1; length <= 2; ++length) {
        result = CL_FormatSteamChallenge(ticket, length, 1, output, sizeof(output));
        check(result == ClientAuthResult::ready && std::strstr(output, length == 1 ? "AA==" : "AP8="), "base64 tail padding retained");
    }
    std::strcpy(output, "must be cleared");
    check(CL_GetPlatformChallenge(output, sizeof(output)) == ClientAuthResult::unavailable && !output[0], "Linux provider unavailable with empty request");
    CL_CancelPlatformAuthTicket();
    check(CL_GetPlatformChallenge(output, sizeof(output)) == ClientAuthResult::unavailable && !output[0], "cancel does not enable unavailable provider");
    clientUIActives[0].connectionState = CA_CONNECTING;
    cls.realtime = 2999; CL_CheckForResend(0);
    check(!connection.connectPacketCount && !sends && !authErrors, "resend waits before three-second boundary");
    cls.realtime = 3000; CL_CheckForResend(0);
    check(connection.connectTime == 3000 && connection.connectPacketCount == 1 && !sends && authErrors == 1,
        "unavailable provider advances retry bookkeeping without a challenge");
    cls.realtime = 5999; CL_CheckForResend(0);
    check(connection.connectPacketCount == 1 && authErrors == 1 && !sends, "unavailable authentication respects retry throttle");
    cls.realtime = 6000; CL_CheckForResend(0);
    check(connection.connectPacketCount == 2 && connection.connectTime == 6000 && authErrors == 2 && !sends,
        "second scheduled retry remains fail closed");
    connection.demoplaying = true; cls.realtime = 9000; CL_CheckForResend(0);
    check(connection.connectPacketCount == 2 && authErrors == 2 && !sends, "demo playback does not retry authentication");
    connection.demoplaying = false; clientUIActives[0].connectionState = CA_DISCONNECTED; CL_CheckForResend(0);
    check(connection.connectPacketCount == 2 && authErrors == 2 && !sends, "disconnected state does not retry authentication");
    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
