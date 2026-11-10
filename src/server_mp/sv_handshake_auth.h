#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Network authentication fields are untrusted strings. Validate them before
// using the legacy Base64 decoder or interpreting a claimed Steam identity.
inline bool SV_ParseSteamIdentity(const char* text, uint64_t* identity)
{
    if (!text || !*text || !identity)
        return false;
    // KISAK: one spelling per identity. The ban lists and the challenge table
    // compare the identity as text, so "0123" must not be a second name for
    // the player "123" that the same ticket would pass for. No SteamID64
    // starts with a zero.
    if (*text == '0')
        return false;
    uint64_t value = 0;
    size_t digits = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
    {
        // A uint64 decimal identity fits in 20 digits and in the existing
        // 33-byte GUID field.
        if (++digits > 20 || *p < '0' || *p > '9')
            return false;
        const uint64_t digit = *p - '0';
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (!value)
        return false;
    *identity = value;
    return true;
}

// Two stored identities name the same player when they are the same SteamID64
// or otherwise the same text.  (SV_ParseSteamIdentity accepts one spelling per
// SteamID64, so for those the two tests agree.)  An empty identity (a local or
// bot client) matches nothing, not even another empty one.
inline bool SV_IdentitiesMatch(const char* a, const char* b)
{
    if (!a || !b || !*a || !*b)
        return false;
    uint64_t steamA = 0;
    uint64_t steamB = 0;
    const bool aIsSteam = SV_ParseSteamIdentity(a, &steamA);
    const bool bIsSteam = SV_ParseSteamIdentity(b, &steamB);
    if (aIsSteam || bIsSteam)
        return aIsSteam && bIsSteam && steamA == steamB;
    return strcmp(a, b) == 0;
}

// A getchallenge for a SteamID64 that a connected client already holds would
// have the Steam backend replace that client's auth session.  It is refused
// unless it can only be that same player coming back: it comes from the same
// address and the connected client has been silent for a while (a crashed or
// closed game; a running one sends packets all the time).  So a packet with a
// claimed SteamID64, which costs nothing to send, never ends the session of
// someone who is playing.  The client repeats its getchallenge every three
// seconds, so a returning player waits for the second or third one.
constexpr int SV_STEAM_RECONNECT_SILENCE_MSEC = 5000;

inline bool SV_SteamReconnectMayReplaceSession(bool sameBaseAddress, int serverTime, int holderLastPacketTime)
{
    return sameBaseAddress && serverTime - holderLastPacketTime >= SV_STEAM_RECONNECT_SILENCE_MSEC;
}

// A retail CoD4 client authenticates with
//
//     getchallenge <clientChallenge> "<32 hex digits>"
//
// The quoted field is an operator-visible GUID, not a credential: nothing in the
// exchange proves that the sender owns it.  Accepting one is therefore an
// explicit, opt-in trust decision (see sv_allowUnverifiedGuids), and the accepted
// form is pinned to exactly 32 ASCII hex digits so that a GUID has a single
// spelling.  Normalising to lowercase here means two clients claiming the same
// GUID compare equal against bans and against each other no matter which case
// they sent.
//
// Everything else is rejected: a shorter or longer field, trailing characters,
// whitespace, a sign, a "0x" prefix, and any non-ASCII byte.  Rejection is by
// byte inspection, so an embedded NUL simply ends the hex run and fails on the
// length check that follows.
inline bool SV_ParseRetailGuid(const char* text, char out[33])
{
    if (!text || !out)
        return false;
    char normalized[33] = {};
    for (size_t i = 0; i < 32; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        char digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<char>(c);
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<char>(c);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<char>(c - 'A' + 'a');
        else
            return false;
        normalized[i] = digit;
    }
    // Exactly 32 digits: a 33rd character (including a longer field) is rejected.
    if (text[32] != 0)
        return false;
    memcpy(out, normalized, sizeof(normalized));
    return true;
}

inline bool SV_ValidateSteamTicketBase64(const char* text, size_t outputCapacity, size_t* encodedLength)
{
    if (!text || !encodedLength)
        return false;
    size_t length = 0;
    while (text[length])
    {
        // Bound scanning as well as decoded output. A complete 4-byte group
        // produces at most three bytes in the legacy decoder.
        if (length >= (outputCapacity / 3) * 4)
            return false;
        ++length;
    }
    if (!length || length % 4)
        return false;
    size_t padding = 0;
    if (text[length - 1] == '=') ++padding;
    if (length > 1 && text[length - 2] == '=') ++padding;
    for (size_t i = 0; i < length - padding; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/'))
            return false;
    }
    const size_t decodedLength = length / 4 * 3 - padding;
    if (!decodedLength || decodedLength > outputCapacity)
        return false;
    *encodedLength = length;
    return true;
}

#if (defined(__linux__) || defined(__APPLE__) || defined(__vita__)) && defined(KISAK_DEDICATED)
inline bool SV_IsLoopbackTestIdentity(const char* identity)
{
    static const char prefix[] = "local-test-";
    if (!identity)
        return false;
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i)
        if (identity[i] != prefix[i])
            return false;
    return true;
}
#endif
