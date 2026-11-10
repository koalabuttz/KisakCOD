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

const char registrySource[] = "the registry (HKLM\\SOFTWARE\\Activision\\Call of Duty 4, codkey)";

// Where a connect gets the key code: <homePath>/codkey.txt, else (when there
// is no such file) the retail install's registry value on Windows. Fills the
// file's path, and names the place the answer came from.
KeyCodeStatus FindKeyCode(const char *homePath, char *path, size_t pathSize, char guid[33], const char **source)
{
    KeyCodeStatus status = KeyCodeStatus::missing;
    path[0] = 0;
    guid[0] = 0;
    *source = path;
    if (homePath && *homePath)
    {
        const int length = std::snprintf(path, pathSize, "%s/codkey.txt", homePath);
        if (length < 0 || static_cast<size_t>(length) >= pathSize)
        {
            path[0] = 0;
            return KeyCodeStatus::unreadable;
        }
        status = CL_ReadKeyCodeFile(path, guid);
    }
    if (status == KeyCodeStatus::missing)
    {
        status = CL_ReadKeyCodeRegistry(guid);
        // Without a registry value the file is the place to put one.
        if (status != KeyCodeStatus::missing)
            *source = registrySource;
    }
    return status;
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
    const char *source = path;
    const KeyCodeStatus status = FindKeyCode(homePath, path, sizeof(path), s_keyCodeGuid, &source);
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

KeyCodeStatus CL_SaveKeyCode(const char *homePath, const char *keyText)
{
    char path[1024] = {};
    if (!homePath || !*homePath)
    {
        Com_PrintError(CON_CHANNEL_CLIENT, "The key code was not saved: there is no KisakCOD home folder (fs_homepath).\n");
        return KeyCodeStatus::unwritable;
    }
    const int length = std::snprintf(path, sizeof(path), "%s/codkey.txt", homePath);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(path))
    {
        Com_PrintError(CON_CHANNEL_CLIENT, "The key code was not saved: the home folder's path is too long.\n");
        return KeyCodeStatus::unwritable;
    }
    KeyCodeStatus status = CL_WriteKeyCodeFile(path, keyText);
    char guid[33] = {};
    // Read it back the way a connect will, so "saved" means usable.
    if (status == KeyCodeStatus::ok)
        status = CL_ReadKeyCodeFile(path, guid);
    if (status == KeyCodeStatus::ok)
    {
        Com_Printf(CON_CHANNEL_CLIENT,
            "[Kisak] Key code saved to %s. Servers that accept key codes will know you as GUID %s.\n", path, guid);
    }
    else if (status == KeyCodeStatus::malformed || status == KeyCodeStatus::badChecksum)
    {
        Com_PrintError(CON_CHANNEL_CLIENT,
            "The key code was not saved: it is %s. It needs 20 letters and digits, as printed on the key "
            "(the last four check the rest).\n",
            status == KeyCodeStatus::badChecksum ? "mistyped (its check characters do not match)" : "not a key code");
    }
    else
    {
        Com_PrintError(CON_CHANNEL_CLIENT, "The key code was not saved: %s could not be written.\n", path);
    }
    return status;
}

KeyCodeStatus CL_StoredKeyCodeStatus(const char *homePath)
{
    char path[1024];
    char guid[33];
    const char *source = nullptr;
    return FindKeyCode(homePath, path, sizeof(path), guid, &source);
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
