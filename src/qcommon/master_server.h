#pragma once

#include <cstdint>
#include <cstring>

// Rules for the internet server list ("master server").
//
// Retail CoD4 asked cod4master.activision.com for the list of internet games
// and dedicated servers sent that host a heartbeat every three minutes.  That
// host is gone, and this port must not talk to anyone the person running it did
// not choose.  So the master is a setting, `masterServerName`, that is empty by
// default:
//
//   * empty: the client sends no list request and the server sends no
//     heartbeat or end-of-game status.  LAN scan, favourites and `connect` are
//     how servers are found.
//   * set: the client asks that host for its list, and a `dedicated 2` server
//     sends it heartbeats, the same packets as retail.
//
// The wire format is the Quake 3 one retail used, which dpmaster and similar
// masters speak: `heartbeat COD-4`, the master's `getinfo <challenge>` answered
// by the normal infoResponse, `getservers <protocol> full empty` answered by
// `getserversResponse` packets of `\` + 4 address bytes + 2 port bytes, ended by
// `\EOT`.
//
// Header-only and free of engine types so the asset-free check
// (scripts/tests/test_master_server.py) compiles exactly these rules.

// True when the setting names a master at all.  Blank counts as unset.
inline bool Master_NameIsSet(const char* name)
{
    if (!name)
        return false;
    for (; *name; ++name)
    {
        if (*name != ' ' && *name != '\t' && *name != '\r' && *name != '\n')
            return true;
    }
    return false;
}

// "host:port" carries its own port; a bare host uses `masterPort`.
inline bool Master_NameHasPort(const char* name)
{
    return name && std::strchr(name, ':') != nullptr;
}

// ---------------------------------------------------------------------------
// getserversResponse
// ---------------------------------------------------------------------------

struct MasterServerEntry
{
    uint8_t ip[4];
    uint8_t portHigh; // port as sent: high byte first
    uint8_t portLow;
};

constexpr int MASTER_MAX_ENTRIES_PER_PACKET = 256; // retail's per-packet limit

// A master must only be able to point the client at ordinary hosts: the client
// sends each listed address a getinfo.  Port 0, 0.x.x.x and the multicast,
// reserved and broadcast ranges (224.0.0.0 and up) are dropped.  Private and
// loopback addresses stay, so a master on the LAN or on the same machine works.
inline bool Master_AddressIsListable(const uint8_t ip[4], uint8_t portHigh, uint8_t portLow)
{
    if (!portHigh && !portLow)
        return false;
    if (ip[0] == 0 || ip[0] >= 224)
        return false;
    return true;
}

// Reads the server addresses out of one getserversResponse packet.
//
// `data` is the whole datagram (the four 0xFF bytes and the command included),
// as retail scans it.  An entry is a backslash, six bytes, and it only counts
// when another backslash follows it, exactly as in retail; the closing `\EOT`
// is shorter than an entry plus its separator, so it can never be read as one
// and no address is mistaken for it.  Nothing outside [data, data + size) is
// read: retail looked for "EOT" up to three bytes past the end of the packet.
//
// Returns the number of entries written to `out`.  `skipped`, when given,
// receives the number of well-formed entries dropped as unlistable.
inline int Master_ParseServersResponse(
    const uint8_t* data, int size, MasterServerEntry* out, int maxOut, int* skipped = nullptr)
{
    int count = 0;
    int dropped = 0;
    int pos = 0;

    if (skipped)
        *skipped = 0;
    if (!data || size <= 0 || !out || maxOut <= 0)
        return 0;

    while (pos < size && data[pos] != '\\')
        ++pos;

    // data[pos] is a backslash; an entry needs six bytes and a separator after it.
    while (pos < size && size - pos >= 8)
    {
        const uint8_t* entry = data + pos + 1;
        if (entry[6] != '\\')
            break;
        if (Master_AddressIsListable(entry, entry[4], entry[5]))
        {
            if (count >= maxOut || count >= MASTER_MAX_ENTRIES_PER_PACKET)
                break;
            std::memcpy(out[count].ip, entry, 4);
            out[count].portHigh = entry[4];
            out[count].portLow = entry[5];
            ++count;
        }
        else
        {
            ++dropped;
        }
        pos += 7;
    }

    if (skipped)
        *skipped = dropped;
    return count;
}

// ---------------------------------------------------------------------------
// Which getserversResponse packets the client accepts
// ---------------------------------------------------------------------------

// Retail accepted a server list from any address at any time, so anyone who
// could reach the client's port could fill its list and make it send getinfo
// to up to 20000 addresses of their choosing.  A list is now accepted only from
// the master the player's own `globalservers` asked, and only for a short time
// after asking; a master sends its packets within a few milliseconds.

constexpr uint32_t MASTER_RESPONSE_WINDOW_MS = 10000;

struct MasterQuery
{
    bool asked;
    uint8_t ip[4];
    uint16_t port; // as stored in netadr_t (network byte order)
    uint32_t askedTime; // ms, Sys_Milliseconds clock
};

inline void Master_QueryAsked(MasterQuery* query, const uint8_t ip[4], uint16_t port, uint32_t now)
{
    if (!query)
        return;
    query->asked = true;
    std::memcpy(query->ip, ip, 4);
    query->port = port;
    query->askedTime = now;
}

inline bool Master_QueryAccepts(const MasterQuery* query, const uint8_t ip[4], uint16_t port, uint32_t now)
{
    if (!query || !query->asked)
        return false;
    if (std::memcmp(query->ip, ip, 4) != 0 || query->port != port)
        return false;
    // Unsigned difference, so the 49-day millisecond wrap is harmless.
    return now - query->askedTime <= MASTER_RESPONSE_WINDOW_MS;
}

// ---------------------------------------------------------------------------
// Server side: when to look the master up, when to send a heartbeat
// ---------------------------------------------------------------------------

constexpr int MASTER_HEARTBEAT_MSEC = 180000;       // retail: every three minutes
constexpr uint32_t MASTER_RESOLVE_RETRY_MS = 900000; // a failed lookup is retried after 15 minutes

// Only a `dedicated 2` ("internet") server with a master set talks to it, as in
// retail.  Listen servers and `dedicated 1` (LAN) servers never do.
inline bool Master_ServerUsesMaster(int dedicatedMode, const char* masterName)
{
    return dedicatedMode == 2 && Master_NameIsSet(masterName);
}

// A heartbeat is due when the server uses a master and its clock has reached
// the scheduled time.  Retail forces one by scheduling INT_MIN.
inline bool Master_HeartbeatDue(int dedicatedMode, const char* masterName, int serverTime, int nextHeartbeatTime)
{
    return Master_ServerUsesMaster(dedicatedMode, masterName) && serverTime >= nextHeartbeatTime;
}

struct MasterResolveState
{
    char name[256];      // the setting the cached address belongs to
    int port;
    bool attempted;
    bool resolved;
    uint32_t attemptTime; // ms, Sys_Milliseconds clock
};

// Name lookups block the server, so the address is looked up once per setting.
// A changed name or port is looked up at once; a failed lookup is retried, but
// no more often than every MASTER_RESOLVE_RETRY_MS.
inline bool Master_ShouldResolve(const MasterResolveState* state, const char* name, int port, uint32_t now)
{
    if (!state || !Master_NameIsSet(name))
        return false;
    if (!state->attempted || state->port != port || std::strncmp(state->name, name, sizeof(state->name) - 1) != 0)
        return true;
    if (state->resolved)
        return false;
    return now - state->attemptTime >= MASTER_RESOLVE_RETRY_MS;
}

inline void Master_ResolveAttempted(MasterResolveState* state, const char* name, int port, bool resolved, uint32_t now)
{
    if (!state)
        return;
    std::strncpy(state->name, name ? name : "", sizeof(state->name) - 1);
    state->name[sizeof(state->name) - 1] = 0;
    state->port = port;
    state->attempted = true;
    state->resolved = resolved;
    state->attemptTime = now;
}
