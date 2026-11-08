#include <client_mp/cl_auth.h>
#include <client_mp/cl_keycode.h>
#include <universal/sha256.h>
#include <sys/stat.h>
#include <unistd.h>
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
static std::string lastPrint;
bool NET_OutOfBandPrint(netsrc_t, netadr_t, const char *text) { ++sends; lastPrint = text; return true; }
bool NET_OutOfBandData(netsrc_t, netadr_t, const uint8_t *, int) { ++sends; return true; }
char *Dvar_InfoString(int, int) { std::abort(); }
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

    // ---- key codes (made-up key; no real key is used anywhere) ----
    auto hex = [](const unsigned char *bytes, size_t size) {
        std::string text;
        char pair[3];
        for (size_t i = 0; i < size; ++i) { std::snprintf(pair, sizeof(pair), "%02x", bytes[i]); text += pair; }
        return text;
    };
    unsigned char digest[32];
    Sha256 sha;
    Sha256_Init(&sha); Sha256_Update(&sha, "abc", 3); Sha256_Final(&sha, digest);
    check(hex(digest, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    std::string thousand(1000, 'a');
    Sha256_Init(&sha);
    for (size_t at = 0; at < thousand.size(); at += 7)
        Sha256_Update(&sha, thousand.data() + at, std::min<size_t>(7, thousand.size() - at));
    Sha256_Final(&sha, digest);
    check(hex(digest, 32) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
        "SHA-256 of 1000 bytes fed in uneven pieces");
    check(CL_KeyCodeCrc("123456789", 9) == 0xBB3D, "key checksum is CRC-16/ARC (check value 0xBB3D)");
    char key[17], sum[5];
    check(CL_ParseKeyCode("k1sa-kc0d-test-key9-8b44", key, sum) && !std::strcmp(key, "K1SAKC0DTESTKEY9")
        && !std::strcmp(sum, "8B44"), "key code parses with dashes in lower case");
    check(CL_ParseKeyCode(" K1SAKC0DTESTKEY98B44\r\n", key, sum), "key code parses bare with spaces and a line ending");
    check(CL_KeyCodeChecksumMatches("K1SAKC0DTESTKEY9", "8b44") && !CL_KeyCodeChecksumMatches("K1SAKC0DTESTKEY9", "8B45"),
        "checksum match is exact, letter case aside");
    check(!CL_ParseKeyCode("K1SAKC0DTESTKEY98B4", key, sum), "19 characters rejected");
    check(!CL_ParseKeyCode("K1SAKC0DTESTKEY98B440", key, sum), "21 characters rejected");
    check(!CL_ParseKeyCode("K1SAKC0DTESTKEY9_8B44", key, sum), "other punctuation rejected");
    check(!CL_ParseKeyCode("K1SAKC0DTESTKEY9\xC3" "8B4", key, sum), "non-ASCII rejected");
    char guid[33];
    check(CL_KeyCodeGuidFromText("K1SAK-C0DTE-STKEY-98B44", guid) == KeyCodeStatus::ok
        && !std::strcmp(guid, "33e2463444a0b14224e9592469933bd1"), "GUID is the first half of the domain-separated SHA-256");
    check(CL_KeyCodeGuidFromText("K1SAKC0DTESTKEY98B45", guid) == KeyCodeStatus::badChecksum && !guid[0], "mistyped key gives no GUID");
    check(CL_FormatKeyCodeChallenge("33e2463444a0b14224e9592469933bd1", output, sizeof(output)) == ClientAuthResult::ready
        && !std::strcmp(output, "getchallenge 0 \"33e2463444a0b14224e9592469933bd1\""), "key code request is the retail two-field form");
    check(CL_FormatKeyCodeChallenge("33e2463444a0b14224e9592469933bd", output, sizeof(output)) == ClientAuthResult::invalidTicket
        && !output[0], "31-digit GUID refused");
    check(CL_FormatKeyCodeChallenge("33e2463444a0b14224e9592469933bdz", output, sizeof(output)) == ClientAuthResult::invalidTicket,
        "non-hex GUID refused");
    check(CL_FormatKeyCodeChallenge("33e2463444a0b14224e9592469933bd1", output, 49) == ClientAuthResult::bufferTooSmall
        && !output[0], "short output fails closed");

    // The resend state machine sends the key code request once a connect picked it.
    char home[] = "/tmp/kisak-auth-XXXXXX";
    check(mkdtemp(home) != nullptr, "temporary home folder");
    const std::string keyPath = std::string(home) + "/codkey.txt";
    std::FILE *file = std::fopen(keyPath.c_str(), "w");
    std::fputs("K1SAK-C0DTE-STKEY-98B44\n", file);
    std::fclose(file);
    chmod(keyPath.c_str(), 0600);
    netadr_t server{};
    server.type = NA_IP;
    check(CL_PrepareRemoteAuth(server, home, false) == ClientAuthMethod::keyCode, "key file picks the key code method");
    connection = {};
    sends = 0;
    clientUIActives[0].connectionState = CA_CONNECTING;
    cls.realtime = 20000; CL_CheckForResend(0);
    check(sends == 1 && lastPrint == "getchallenge 0 \"33e2463444a0b14224e9592469933bd1\"" && authErrors == 2,
        "resend sends the key code GUID request");
    chmod(keyPath.c_str(), 0640);
    check(CL_PrepareRemoteAuth(server, home, false) == ClientAuthMethod::none && authErrors == 3,
        "group-readable key file refused");
    cls.realtime = 30000; CL_CheckForResend(0);
    check(sends == 1 && authErrors == 4, "after a refused key file, resend sends nothing");
    unlink(keyPath.c_str());
    check(CL_PrepareRemoteAuth(server, home, true) == ClientAuthMethod::none && authErrors == 5,
        "no key file and no Steam: none");
    rmdir(home);
    clientUIActives[0].connectionState = CA_DISCONNECTED;

    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
