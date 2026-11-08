#include "cl_auth.h"
#include "cl_keycode.h"
#include <universal/base64.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <cstdio>
#include <cstring>
#if defined(WIN32) || defined(KISAK_LINUX_STEAM)
#include <win32/win_steam.h>
#endif

namespace {
// The method the last CL_PrepareRemoteAuth picked, and the key code GUID it
// read. Only the GUID is kept; the key itself never outlives the read.
ClientAuthMethod s_authMethod = ClientAuthMethod::none;
char s_keyCodeGuid[33];

bool IsGuid(const char *guid)
{
    if (!guid)
        return false;
    for (int i = 0; i < 32; ++i)
    {
        const char c = guid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return guid[32] == 0;
}
}

ClientAuthResult CL_FormatSteamChallenge(const unsigned char *ticket, uint32_t size,
    uint64_t steamId, char *output, size_t capacity)
{
    if (output && capacity) output[0] = '\0';
    // The former client validation decoded into a 1024-byte buffer.
    // Bound to that capacity before base64 size arithmetic.
    if (!ticket || !size || size > 1024 || !steamId)
        return ClientAuthResult::invalidTicket;
    if (!output || !capacity) return ClientAuthResult::bufferTooSmall;
    unsigned char encoded[1369];
    const uint32_t encodedSize = b64_encode(ticket, size, encoded);
    unsigned char decoded[1024];
    if (b64_decode(encoded, encodedSize, decoded) != size || std::memcmp(ticket, decoded, size))
        return ClientAuthResult::invalidTicket;
    int length = std::snprintf(output, capacity, "getchallenge 0 \"%s\" \"%llu\"",
        encoded, static_cast<unsigned long long>(steamId));
    if (length < 0 || static_cast<size_t>(length) >= capacity)
    {
        output[0] = '\0';
        return ClientAuthResult::bufferTooSmall;
    }
    return ClientAuthResult::ready;
}

ClientAuthResult CL_FormatKeyCodeChallenge(const char *guid, char *output, size_t capacity)
{
    if (output && capacity) output[0] = '\0';
    if (!IsGuid(guid)) return ClientAuthResult::invalidTicket;
    if (!output || !capacity) return ClientAuthResult::bufferTooSmall;
    int length = std::snprintf(output, capacity, "getchallenge 0 \"%s\"", guid);
    if (length < 0 || static_cast<size_t>(length) >= capacity)
    {
        output[0] = '\0';
        return ClientAuthResult::bufferTooSmall;
    }
    return ClientAuthResult::ready;
}

ClientAuthMethod CL_PrepareRemoteAuth(const netadr_t &server, const char *homePath, bool keyCodeOnly)
{
    s_authMethod = ClientAuthMethod::none;
    s_keyCodeGuid[0] = 0;
#if defined(WIN32) || defined(KISAK_LINUX_STEAM)
    if (!keyCodeOnly && g_steamInitialized && Steam_UpdateClientAuthTicket(server))
    {
        s_authMethod = ClientAuthMethod::steam;
        return s_authMethod;
    }
#else
    (void)server;
#endif

    char path[1024] = {};
    KeyCodeStatus fileStatus = KeyCodeStatus::missing;
    if (homePath && *homePath)
    {
        std::snprintf(path, sizeof(path), "%s/codkey.txt", homePath);
        fileStatus = CL_ReadKeyCodeFile(path, s_keyCodeGuid);
    }
    const char *source = path;
    KeyCodeStatus status = fileStatus;
    if (status == KeyCodeStatus::missing)
    {
        status = CL_ReadKeyCodeRegistry(s_keyCodeGuid);
        source = "the registry (HKLM\\SOFTWARE\\Activision\\Call of Duty 4, codkey)";
    }
    if (status == KeyCodeStatus::ok)
    {
        Com_Printf(CON_CHANNEL_CLIENT,
            "[Kisak] Joining with the key code from %s, as GUID %s. Only servers that accept "
            "unverified GUIDs (sv_allowUnverifiedGuids 1) let it in.\n", source, s_keyCodeGuid);
        s_authMethod = ClientAuthMethod::keyCode;
        return s_authMethod;
    }
    s_keyCodeGuid[0] = 0;

    if (status == KeyCodeStatus::missing)
    {
#if defined(WIN32) || defined(KISAK_LINUX_STEAM)
        Com_PrintError(CON_CHANNEL_CLIENT,
            "Client authentication unavailable: %s, and no key code was found. Put your CoD4 key code "
            "in %s to join servers that accept key codes.\n",
            keyCodeOnly ? "cl_keyCodeOnly is set" : "Steam has no ticket for this server",
            path[0] ? path : "codkey.txt in the KisakCOD home folder");
#else
        Com_PrintError(CON_CHANNEL_CLIENT,
            "Client authentication unavailable: this build has no Steam, and no key code was found. "
            "Put your CoD4 key code in %s to join servers that accept key codes.\n",
            path[0] ? path : "codkey.txt in the KisakCOD home folder");
#endif
    }
    else if (status == KeyCodeStatus::unsafePermissions)
    {
        Com_PrintError(CON_CHANNEL_CLIENT,
            "Client authentication unavailable: %s must be a file only you can read (chmod 600 it).\n", source);
    }
    else
    {
        Com_PrintError(CON_CHANNEL_CLIENT,
            "Client authentication unavailable: the key code in %s is %s. It needs 20 letters and digits, "
            "as printed on the key (the last four check the rest).\n", source,
            status == KeyCodeStatus::badChecksum ? "mistyped (its check characters do not match)"
                : status == KeyCodeStatus::malformed ? "not a key code" : "unreadable");
    }
    return s_authMethod;
}

ClientAuthResult CL_GetPlatformChallenge(char *output, size_t capacity)
{
    if (output && capacity) output[0] = '\0';
    if (s_authMethod == ClientAuthMethod::keyCode)
        return CL_FormatKeyCodeChallenge(s_keyCodeGuid, output, capacity);
#if defined(WIN32) || defined(KISAK_LINUX_STEAM)
    unsigned char *ticket = nullptr;
    uint32 size = 0;
    if (!Steam_GetRawClientTicket(&ticket, &size)) return ClientAuthResult::unavailable;
    return CL_FormatSteamChallenge(ticket, size, Steam_GetClientSteamID64(), output, capacity);
#else
    return ClientAuthResult::unavailable;
#endif
}

void CL_CancelPlatformAuthTicket()
{
#if defined(WIN32) || defined(KISAK_LINUX_STEAM)
    Steam_CancelClientTicket();
#endif
}

const char *CL_AuthResultName(ClientAuthResult result)
{
    switch (result)
    {
    case ClientAuthResult::ready: return "ready";
    case ClientAuthResult::unavailable: return "unavailable";
    case ClientAuthResult::invalidTicket: return "invalid-ticket";
    case ClientAuthResult::bufferTooSmall: return "buffer-too-small";
    }
    return "invalid-result";
}
