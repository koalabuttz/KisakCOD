#pragma once

#include <cstddef>
#include <cstdint>

// Network authentication fields are untrusted strings. Validate them before
// using the legacy Base64 decoder or interpreting a claimed Steam identity.
inline bool SV_ParseSteamIdentity(const char* text, uint64_t* identity)
{
    if (!text || !*text || !identity)
        return false;
    uint64_t value = 0;
    size_t digits = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
    {
        // A uint64 decimal identity fits in 20 digits and in the existing
        // 33-byte GUID field. Do not admit overlong zero-padded spellings.
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

#if defined(__linux__) && defined(KISAK_DEDICATED)
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
