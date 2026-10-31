#include "cl_auth.h"
#include <universal/base64.h>
#include <cstdio>
#include <cstring>
#ifdef WIN32
#include <win32/win_steam.h>
#endif

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

ClientAuthResult CL_GetPlatformChallenge(char *output, size_t capacity)
{
    if (output && capacity) output[0] = '\0';
#ifdef WIN32
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
#ifdef WIN32
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
