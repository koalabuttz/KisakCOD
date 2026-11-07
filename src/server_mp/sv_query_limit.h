#pragma once

#include <cstdint>
#include <cstring>

// Rate limits for out-of-band replies (getstatus, getinfo, getchallenge, rcon).
//
// Retail answers every connectionless query from whatever address the datagram
// claims, so a ~20-byte spoofed getstatus makes the server send several KB to a
// victim: CoD4 servers were used as DDoS reflectors this way.  The only retail
// throttle was one global 500 ms gap on rcon, which also let anyone lock the
// admin out by spamming bad passwords.
//
// These limits follow the leaky-bucket scheme of ioquake3 and CoD4x: each source
// address gets a small burst, then one reply per period, and each kind of reply
// also has a global bucket so a flood from many spoofed addresses cannot turn
// into unlimited outbound traffic.  Nothing on the wire changes: a limited query
// is dropped, exactly as a lost UDP packet would be, and clients and server
// browsers already retry.  Private and loopback addresses are exempt (the server
// code checks that before calling in here), as in CoD4x.
//
// Header-only and free of engine types so the asset-free check
// (scripts/tests/test_server_remote_input.py) compiles exactly these rules.

struct SvRateBucket
{
    uint32_t lastTime; // ms, Sys_Milliseconds clock
    int count;         // events still "in the bucket"
};

// Allows up to `burst` events at once; one drains every `periodMs`.
// Returns true when the event is allowed (and counts it).
inline bool SV_RateBucketAllow(SvRateBucket* bucket, uint32_t now, int burst, int periodMs)
{
    if (!bucket || burst <= 0 || periodMs <= 0)
        return false;

    // Unsigned difference, so the 49-day millisecond wrap is harmless.
    const uint32_t elapsed = now - bucket->lastTime;
    const uint32_t drained = elapsed / static_cast<uint32_t>(periodMs);
    if (bucket->count <= 0 || drained >= static_cast<uint32_t>(bucket->count))
    {
        bucket->count = 0;
        bucket->lastTime = now;
    }
    else
    {
        bucket->count -= static_cast<int>(drained);
        bucket->lastTime += drained * static_cast<uint32_t>(periodMs);
    }

    if (bucket->count < burst)
    {
        ++bucket->count;
        return true;
    }
    return false;
}

struct SvAddressBucket
{
    uint32_t address; // IPv4, network byte order as stored in netadr_t::ip
    bool used;
    SvRateBucket bucket;
};

// A fixed table of per-address buckets.  An address probes a few slots from its
// hash; when none is free, the least recently used probed slot is reused, so a
// flood of spoofed addresses can only reset other addresses' buckets (which
// makes them more permissive, never less) and the global bucket still caps the
// total.
constexpr int SV_ADDRESS_BUCKETS = 1024; // power of two
constexpr int SV_ADDRESS_PROBES = 8;

struct SvAddressLimiter
{
    SvAddressBucket slots[SV_ADDRESS_BUCKETS];
    SvRateBucket global;
};

inline uint32_t SV_AddressBucketHash(uint32_t address, uint32_t salt)
{
    uint32_t h = (address ^ salt) * 2654435761u;
    h ^= h >> 15;
    return h & (SV_ADDRESS_BUCKETS - 1);
}

inline SvRateBucket* SV_AddressBucketFor(SvAddressLimiter* limiter, uint32_t address, uint32_t salt)
{
    const uint32_t start = SV_AddressBucketHash(address, salt);
    SvAddressBucket* reuse = nullptr;
    for (int probe = 0; probe < SV_ADDRESS_PROBES; ++probe)
    {
        SvAddressBucket* slot = &limiter->slots[(start + probe) & (SV_ADDRESS_BUCKETS - 1)];
        if (slot->used && slot->address == address)
            return &slot->bucket;
        if (!slot->used)
        {
            if (!reuse || reuse->used)
                reuse = slot;
        }
        else if (!reuse || (reuse->used && slot->bucket.lastTime - reuse->bucket.lastTime >= 0x80000000u))
        {
            // slot is older than reuse (unsigned wrap-safe "slot < reuse")
            reuse = slot;
        }
    }
    reuse->used = true;
    reuse->address = address;
    reuse->bucket.count = 0;
    reuse->bucket.lastTime = 0;
    return &reuse->bucket;
}

// Per-address limit only.
inline bool SV_AddressLimitAllow(SvAddressLimiter* limiter, uint32_t address, uint32_t salt,
                                 uint32_t now, int burst, int periodMs)
{
    if (!limiter)
        return false;
    return SV_RateBucketAllow(SV_AddressBucketFor(limiter, address, salt), now, burst, periodMs);
}

// The limits each kind of reply gets.  Per address: CoD4x's numbers for
// getstatus (2, then one per 2 s) and getinfo (4, then one per 2 s).  The
// global buckets bound total output under a spoofed flood while staying well
// above what server browsers and trackers send to one server.
struct SvQueryLimitRule
{
    int addressBurst;
    int addressPeriodMs;
    int globalBurst;
    int globalPeriodMs;
};

enum SvQueryKind
{
    SV_QUERY_STATUS,    // getstatus: up to ~8 KB reply
    SV_QUERY_INFO,      // getinfo: ~1 KB reply
    SV_QUERY_CHALLENGE, // getchallenge: short reply, takes a challenge slot
    SV_QUERY_RCON,      // rcon: per address before the password check; global on failure replies
    SV_QUERY_KIND_COUNT
};

inline SvQueryLimitRule SV_QueryLimitRuleFor(SvQueryKind kind)
{
    switch (kind)
    {
    case SV_QUERY_STATUS:    return { 2, 2000, 20, 100 };  // global: 20, then 10/s
    case SV_QUERY_INFO:      return { 4, 2000, 50, 20 };   // global: 50, then 50/s
    case SV_QUERY_CHALLENGE: return { 4, 1000, 50, 20 };   // global: 50, then 50/s
    case SV_QUERY_RCON:      return { 10, 200, 10, 100 };  // admin tools: 10, then 5/s; bad replies 10/s
    default:                 return { 0, 1, 0, 1 };
    }
}

struct SvQueryLimits
{
    SvAddressLimiter kinds[SV_QUERY_KIND_COUNT];
    uint32_t salt;
};

// Per-address bucket first, so an address that is already over its own limit
// cannot drain the global bucket for everyone else.
inline bool SV_QueryLimitAllow(SvQueryLimits* limits, SvQueryKind kind, uint32_t address, uint32_t now)
{
    if (!limits || kind < 0 || kind >= SV_QUERY_KIND_COUNT)
        return false;
    const SvQueryLimitRule rule = SV_QueryLimitRuleFor(kind);
    SvAddressLimiter* limiter = &limits->kinds[kind];
    if (!SV_AddressLimitAllow(limiter, address, limits->salt, now, rule.addressBurst, rule.addressPeriodMs))
        return false;
    if (kind == SV_QUERY_RCON)
        return true; // rcon's global bucket only limits failure replies
    return SV_RateBucketAllow(&limiter->global, now, rule.globalBurst, rule.globalPeriodMs);
}

// rcon: the reply to a wrong password.  A correct password is never limited by
// this, so flooding bad passwords cannot lock the admin out.
inline bool SV_RconFailureReplyAllow(SvQueryLimits* limits, uint32_t now)
{
    if (!limits)
        return false;
    const SvQueryLimitRule rule = SV_QueryLimitRuleFor(SV_QUERY_RCON);
    return SV_RateBucketAllow(&limits->kinds[SV_QUERY_RCON].global, now, rule.globalBurst, rule.globalPeriodMs);
}

inline uint32_t SV_QueryLimitAddressKey(const unsigned char ip[4])
{
    uint32_t key;
    memcpy(&key, ip, sizeof(key));
    return key;
}
