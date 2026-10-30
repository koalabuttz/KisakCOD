// net.cpp -- native POSIX (IPv4/UDP) network layer for the Linux build.
//
// This is the POSIX counterpart of src/win32/win_net.cpp: same exported
// surface, same engine address representation, reimplemented on socket(2),
// bind(2), recvfrom(2), sendto(2), getaddrinfo(3), select(2) and getifaddrs(3).
//
// Byte order contract (unchanged from win_net.cpp):
//
//   netadr_t.ip[4]  -- the four address octets in network order
//   netadr_t.port   -- the port in NETWORK order (BigShort on the way in)
//
// Because of that, NetadrToSockadr()/SockadrToNetadr() copy sin_addr/sin_port
// verbatim and never convert.  htons() appears only where a host-order port
// number becomes a sockaddr field (bind and connect), which is exactly the
// contract the engine expects.
//
// Deliberately NOT ported from win_net.cpp:
//   * IPX (AF_IPX).  net_noipx is still registered so existing config files
//     keep parsing, but no IPX socket is ever opened.
//   * SOCKS.  net_socks* are registered for config compatibility only; an
//     enabled net_socksEnabled produces one explicit "unsupported" warning.
//   * net_forcenonlocal.  It was declared in win_net.cpp but never registered
//     or read anywhere in the engine, so it is dead weight.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "net_internal.h"

// 65507 == 65535 - sizeof(struct iphdr) - sizeof(struct udphdr)
static const int NET_MAX_UDP_PAYLOAD = 65507;

// Largest port offset NET_OpenIP() will try past net_port before giving up.
static const int NET_MAX_PORT_ATTEMPTS = 10;

#define MAX_IPS 16

// The POSIX invalid socket.  Deliberately -1 and never 0: descriptor 0 is a
// perfectly valid socket when stdin happens to be closed.
static const int NET_INVALID_SOCKET = -1;

static bool netInitialized = false;
static bool networkingEnabled = false;

static const dvar_t *net_noudp;
static const dvar_t *net_noipx;
static const dvar_t *net_socksEnabled;
static const dvar_t *net_socksServer;
static const dvar_t *net_socksPort;
static const dvar_t *net_socksUsername;
static const dvar_t *net_socksPassword;

static int ip_socket = -1;

static int numIP;
static byte localIP[MAX_IPS][4];

// Last bind parameters that actually produced a socket.  Used by NET_GetDvars()
// so that a latched net_ip/net_port change makes NET_Restart() re-bind; reading
// the values instead of the dvar "modified" flags keeps this consumed exactly
// once and cannot wedge into a rebind loop.
static char lastNetIP[MAX_STRING_CHARS];
static int lastNetPort = -1;

static bool warnedSocksUnsupported = false;

// ---- loopback local-auth test guard state ---------------------------------
//
// `loopbackAuthTestFrozen` records that the start-up decision was captured and
// is never cleared again, so NET_Shutdown()/NET_Init() and net_restart inside
// one process can neither grant nor revoke the permission.  The captured value
// lives in `loopbackAuthTestEnabled`; it is the only authority for "was the
// bypass switched on at start-up", and it is never recomputed from the cvar.
// See NET_CanBypassLoopbackAuth() for what else has to be true per packet.
static const dvar_t *sv_loopbackTestAuth;

static bool loopbackAuthTestFrozen = false;
static bool loopbackAuthTestEnabled = false;

static bool warnedLoopbackAuthTest = false;
static bool warnedLoopbackAuthTestRevoked = false;

//=============================================================================
// Small helpers
//=============================================================================

/*
====================
NET_SetNonBlocking

fcntl() O_NONBLOCK while preserving the flags the descriptor already has.
====================
*/
static bool NET_SetNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);

    if (flags == -1)
        return false;

    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
}

/*
====================
NET_CloseSocket

Idempotent close: bracketing with -1 is what makes NET_Restart/NET_Shutdown
safe to call repeatedly without leaking descriptors.
====================
*/
static void NET_CloseSocket(int *sock)
{
    if (*sock != NET_INVALID_SOCKET)
    {
        close(*sock);
        *sock = NET_INVALID_SOCKET;
    }
}

/*
====================
NET_SleepNs

EINTR-safe relative sleep, used when there is no socket to wait on.
====================
*/
static void NET_SleepNs(int msec)
{
    struct timespec remaining;

    if (msec <= 0)
        return;

    remaining.tv_sec = msec / 1000;
    remaining.tv_nsec = (long)(msec % 1000) * 1000000L;

    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
    {
        // nanosleep() already wrote the unconsumed time back into `remaining`.
        if (remaining.tv_sec == 0 && remaining.tv_nsec == 0)
            break;
    }
}

/*
====================
NET_RemainingMs

Milliseconds left until `deadline` (CLOCK_MONOTONIC), rounded up, clamped at 0.

A deadline recorded a moment ago is nearly always a fractional number of
milliseconds away, so rounding down would return 0 while time still remains.
select() reads a 0 timeout as "do not wait", which turns a 1 ms sleep into a
poll: the caller returns immediately, calls again, and an idle server spins a
core. Rounding up keeps any non-zero remainder a real wait.
====================
*/
static int NET_RemainingMs(const struct timespec *deadline)
{
    struct timespec now;
    int64_t remainingNs;
    int64_t remainingMs;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return 0;

    // Subtract as one signed nanosecond count. The two fields cannot be
    // compared independently: a deadline a fraction of a second out still has
    // a negative nanosecond difference that only the borrowed second cancels.
    remainingNs = ((int64_t)deadline->tv_sec - (int64_t)now.tv_sec) * 1000000000LL
                + ((int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec);

    if (remainingNs <= 0)
        return 0;

    remainingMs = (remainingNs + 999999LL) / 1000000LL;

    if (remainingMs > INT32_MAX)
        return INT32_MAX;
    return (int)remainingMs;
}

/*
====================
NET_DeadlineFromNow
====================
*/
static void NET_DeadlineFromNow(int msec, struct timespec *deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, deadline) == -1)
    {
        memset(deadline, 0, sizeof(*deadline));
        return;
    }

    deadline->tv_sec += msec / 1000;
    deadline->tv_nsec += (long)(msec % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

/*
====================
NET_ResolveIPv4

Numeric first (inet_pton, then the legacy inet_aton short forms Windows'
inet_addr() accepts), then name lookup restricted to AF_INET so the result is
always a deterministic IPv4 address.  A name that does not resolve, including
the empty string, fails - it never falls through to a guessed address.
====================
*/
static bool NET_ResolveIPv4(const char *host, struct in_addr *out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    bool resolved = false;

    if (!host || !host[0])
        return false;

    if (inet_pton(AF_INET, host, out) == 1)
        return true;

    // "127.1", "10.0.0" and friends: classful shorthand that inet_addr() (used
    // by win_net.cpp) accepts and inet_pton() rejects.
    if (inet_aton(host, out))
        return true;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &result) == 0 && result)
    {
        *out = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
        resolved = true;
    }

    if (result)
        freeaddrinfo(result);

    return resolved;
}

//=============================================================================
// Address conversion
//=============================================================================

/*
====================
NET_ErrorString

strerror() text for the current errno, with the numeric code appended so log
lines stay unambiguous across locales.
====================
*/
const char *NET_ErrorString()
{
    static char errorString[256];
    int code = errno;
    const char *message = strerror(code);

    snprintf(errorString, sizeof(errorString), "%s (errno %d)", message ? message : "unknown error", code);
    return errorString;
}

/*
====================
NetadrToSockadr
====================
*/
void NetadrToSockadr(netadr_t *a, struct sockaddr *s)
{
    struct sockaddr_in *in = (struct sockaddr_in *)s;

    memset(s, 0, sizeof(*s));
    in->sin_family = AF_INET;

    if (a->type == NA_BROADCAST)
    {
        // Port and address are both already in network order.
        in->sin_port = a->port;
        in->sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    else if (a->type == NA_IP)
    {
        in->sin_port = a->port;
        memcpy(&in->sin_addr, a->ip, sizeof(a->ip));
    }
}

/*
====================
SockadrToNetadr
====================
*/
void SockadrToNetadr(struct sockaddr *s, netadr_t *a)
{
    if (s->sa_family == AF_INET)
    {
        struct sockaddr_in *in = (struct sockaddr_in *)s;

        a->type = NA_IP;
        memcpy(a->ip, &in->sin_addr, sizeof(a->ip));
        a->port = in->sin_port;
    }
}

/*
====================
Sys_StringToSockaddr

Resolves a bare host string.  The port is left 0 on purpose: NET_StringToAdr()
in qcommon/net_chan_mp.cpp splits "host:port" itself and writes a->port with
BigShort(), so resolving a port here would fight the caller.
====================
*/
qboolean Sys_StringToSockaddr(const char *s, struct sockaddr *sadr)
{
    struct sockaddr_in *in = (struct sockaddr_in *)sadr;

    memset(sadr, 0, sizeof(*sadr));
    in->sin_family = AF_INET;
    in->sin_port = 0;

    if (!NET_ResolveIPv4(s, &in->sin_addr))
    {
        memset(sadr, 0, sizeof(*sadr));
        return qfalse;
    }

    return qtrue;
}

/*
====================
Sys_StringToAdr
====================
*/
qboolean Sys_StringToAdr(const char *s, netadr_t *a)
{
    struct sockaddr_in sadr;

    if (!Sys_StringToSockaddr(s, (struct sockaddr *)&sadr))
        return qfalse;

    memset(a, 0, sizeof(*a));
    SockadrToNetadr((struct sockaddr *)&sadr, a);
    return qtrue;
}

//=============================================================================
// Datagram I/O
//=============================================================================

/*
====================
Sys_GetPacket

Never called by the game logic, only by the system event pump.  Non-blocking:
returns qfalse as soon as the socket has nothing queued.
====================
*/
qboolean Sys_GetPacket(netadr_t *net_from, msg_t *net_message)
{
    struct sockaddr_in from;
    socklen_t fromlen;
    int maxsize;
    int ret;

    if (ip_socket == NET_INVALID_SOCKET || !net_message || !net_message->data)
        return qfalse;

    maxsize = net_message->maxsize;
    if (maxsize <= 0)
        return qfalse;

    // Drain until a usable datagram is found.  Each iteration consumes exactly
    // one datagram, so this cannot spin.
    for (;;)
    {
        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));

        // MSG_TRUNC makes the return value the REAL datagram length, so an
        // oversize datagram is recognisable instead of looking like a full
        // buffer read.  The copy itself is still capped at maxsize.
        ret = (int)recvfrom(ip_socket, net_message->data, (size_t)maxsize, MSG_TRUNC,
                            (struct sockaddr *)&from, &fromlen);

        if (ret == -1)
        {
            // Empty queue (and EINTR) is the normal idle case, not a failure.
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNRESET)
                Com_PrintError(CON_CHANNEL_SYSTEM, "NET_GetPacket: %s\n", NET_ErrorString());
            return qfalse;
        }

        if (ret == 0)
        {
            // Zero-length datagrams are legal UDP; the engine treats them as an
            // empty message, exactly as win_net.cpp did.
            SockadrToNetadr((struct sockaddr *)&from, net_from);
            net_message->readcount = 0;
            net_message->cursize = 0;
            return qtrue;
        }

        if (ret > maxsize)
        {
            // Truncated by MSG_TRUNC: nothing was written past maxsize.  Report
            // it (matching win_net.cpp's oversize warning) and drop the packet.
            SockadrToNetadr((struct sockaddr *)&from, net_from);
            Com_Printf(CON_CHANNEL_SYSTEM, "Oversize packet from %s\n", NET_AdrToString(*net_from));
            continue;
        }

        SockadrToNetadr((struct sockaddr *)&from, net_from);
        net_message->readcount = 0;
        net_message->cursize = ret;
        return qtrue;
    }
}

/*
====================
Sys_SendPacket

Returns non-zero when the datagram was handed to the kernel, or dropped for a
reason the engine treats as non-fatal; zero only on a genuine send failure.
====================
*/
char Sys_SendPacket(int length, uint8_t *data, netadr_t to)
{
    struct sockaddr_in addr;
    int ret;

    switch (to.type)
    {
    case NA_BROADCAST:
    case NA_IP:
        break;
    default:
        Com_Error(ERR_FATAL, "Sys_SendPacket: bad address type");
        return 0;
    }

    if (ip_socket == NET_INVALID_SOCKET)
        return 1;

    if (!data || length < 0 || length > NET_MAX_UDP_PAYLOAD)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "Sys_SendPacket: %i byte packet is not a valid UDP payload\n", length);
        return 0;
    }

    NetadrToSockadr(&to, (struct sockaddr *)&addr);

    ret = (int)sendto(ip_socket, data, (size_t)length, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == length)
        return 1;

    if (ret != -1)
    {
        // UDP never partly sends; a short count means the datagram was not sent.
        Com_PrintError(CON_CHANNEL_SYSTEM, "Sys_SendPacket: short send (%i of %i bytes)\n", ret, length);
        return 0;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 1;

    // Some links refuse the broadcast address itself; win_net.cpp also kept
    // that quiet so a LAN game does not spam the console.
    if (to.type == NA_BROADCAST && (errno == EADDRNOTAVAIL || errno == EACCES || errno == EPERM))
        return 1;

    Com_PrintError(CON_CHANNEL_SYSTEM, "Sys_SendPacket: %s\n", NET_ErrorString());
    return 0;
}

//=============================================================================
// LAN classification
//=============================================================================

/*
====================
Sys_IsLANAddress_IgnoreSubnet
====================
*/
bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr)
{
    if (adr.type == NA_LOOPBACK)
        return true;
    if (adr.type == NA_BOT)
        return true;
    if (adr.type != NA_IP)
        return false;
    if (adr.ip[0] == 10)
        return true;
    if (adr.ip[0] == 127)
        return true;
    if (adr.ip[0] == 169 && adr.ip[1] == 254)
        return true;
    if (adr.ip[0] == 172 && (adr.ip[1] & 0xF0) == 0x10)
        return true;
    return adr.ip[0] == 192 && adr.ip[1] == 168;
}

/*
====================
Sys_IsLANAddress

LAN clients get their rate var ignored.  Beyond the private ranges, an address
counts as LAN when it shares a /24 with one of our own interfaces.
====================
*/
bool Sys_IsLANAddress(netadr_t adr)
{
    int i;

    if (Sys_IsLANAddress_IgnoreSubnet(adr))
        return true;

    for (i = 0; i < numIP; ++i)
    {
        if (adr.ip[0] == localIP[i][0] && adr.ip[1] == localIP[i][1] && adr.ip[2] == localIP[i][2])
            return true;
    }

    return false;
}

/*
====================
Sys_ShowIP
====================
*/
void Sys_ShowIP()
{
    int i;

    for (i = 0; i < numIP; i++)
        Com_Printf(CON_CHANNEL_SYSTEM, "IP: %i.%i.%i.%i\n", localIP[i][0], localIP[i][1], localIP[i][2], localIP[i][3]);
}

//=============================================================================
// Loopback local-auth test guard
//=============================================================================
//
// sv_loopbackTestAuth skips Steam ticket verification for local testing.
// Protocol, address/token challenge matching, passwords and capacity checks
// remain in the server handshake.  Every rule here exists so that a
// mistaken configuration fails closed instead of exposing an unauthenticated
// server to a network:
//
//   * The cvar is startup-only (DVAR_INIT) and its value is captured exactly
//     once, before any socket is bound.  The captured bool is frozen for the
//     life of the process: a NET_Shutdown()/NET_Init() cycle or a net_restart
//     keeps the original decision, so neither a map change nor an internal
//     caller can upgrade a running process into bypass mode.
//   * The cvar is never trusted as permission on its own.  Dvar_Set() can
//     rewrite a DVAR_INIT dvar from engine code, so the gate requires the cvar
//     to AGREE with the frozen value; an internal caller that turns it off
//     revokes the bypass immediately, and one that turns it on grants nothing.
//   * Binding must be loopback and nothing else.  net_ip has to be exactly
//     "127.0.0.1" before anything binds, and the address the kernel actually
//     gave us has to be 127.0.0.1 as well - checked with getsockname() after
//     bind, so a mismatched or wildcard bind is closed and fatal rather than
//     quietly serving the network.  "localhost" is deliberately rejected even
//     though it looks like loopback: NET_IPSocket() treats it as "no interface"
//     and binds INADDR_ANY, which is every interface on the machine.  So are
//     0.0.0.0, LAN addresses, host names and 127.0.0.2.
//   * The server has to be dedicated (com_dedicated non-zero), and the client
//     has to be 127.0.0.1 with type NA_IP on the local socket.  LAN, public
//     and broadcast senders never qualify, and neither do the internal
//     NA_LOOPBACK/NA_BOT pseudo-types or IPX.  The sender's port is free: a
//     test client picks an ephemeral port.
//
// This is a test harness switch, not a server option.  The warning printed
// below says so.

static const char *NET_LOOPBACK_TEST_ADDRESS = "127.0.0.1";

/*
====================
NET_LoopbackTestAddress

Exactly 127.0.0.1 and nothing else.  127.0.0.0/8 as a whole is not enough:
127.0.0.2 and friends are ordinary addresses on a loopback interface and are
not what a local test client uses.
====================
*/
static bool NET_LoopbackTestAddress(const byte *ip)
{
    return ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
}

/*
====================
NET_SocketIsLoopbackBoundFd

Asks the kernel what `sock` is bound to, rather than trusting net_ip.  This is
what makes the guard independent of the configuration path: a socket that ended
up on INADDR_ANY, on a LAN address, or that no longer exists (NET_INVALID_SOCKET)
reports false.
====================
*/
static bool NET_SocketIsLoopbackBoundFd(int sock)
{
    struct sockaddr_in bound;
    socklen_t boundLength = sizeof(bound);

    if (sock == NET_INVALID_SOCKET)
        return false;

    memset(&bound, 0, sizeof(bound));
    if (getsockname(sock, (struct sockaddr *)&bound, &boundLength) == -1)
        return false;
    if (bound.sin_family != AF_INET)
        return false;

    return NET_LoopbackTestAddress((const byte *)&bound.sin_addr);
}

/*
====================
NET_LoopbackTestConfigSafe

The start-up half of the contract: net_ip names loopback exactly and this is a
dedicated server.  Anything else - including the empty string and "localhost",
which NET_IPSocket() turns into a wildcard bind - is unsafe.
====================
*/
static bool NET_LoopbackTestConfigSafe(const char *bindIP)
{
    if (!bindIP || strcmp(bindIP, NET_LOOPBACK_TEST_ADDRESS) != 0)
        return false;

    if (!com_dedicated || com_dedicated->current.integer == 0)
        return false;

    return true;
}

/*
====================
NET_CheckLoopbackTestConfig

Gate for every path that is about to bring a socket up.  Returns true when the
open may proceed, false when the frozen permission is on and the configuration
is unsafe - in which case the diagnostic has already been raised as fatal, and
the caller must not bind anything.

Com_Error(ERR_FATAL) does not return in the engine, so in production the false
path is unreachable; the return value exists so that a test harness with a
non-fatal Com_Error still cannot fall through to bind(2).
====================
*/
static bool NET_CheckLoopbackTestConfig(const char *bindIP, const char *stage)
{
    if (!loopbackAuthTestEnabled)
        return true;

    if (NET_LoopbackTestConfigSafe(bindIP))
        return true;

    Com_Error(ERR_FATAL,
              "sv_loopbackTestAuth is enabled, but this start-up (%s) would not be limited to loopback.\n"
              "net_ip is \"%s\"; with sv_loopbackTestAuth it must be exactly \"%s\", and this must be a dedicated server (dedicated must be set to 1 or 2).\n"
              "\"localhost\" is refused because the engine binds every interface for it, and 0.0.0.0, LAN addresses and host names would expose the unauthenticated test logins to the network.\n"
              "Set net_ip %s and dedicated 1, or turn sv_loopbackTestAuth off.\n",
              stage ? stage : "start-up",
              bindIP ? bindIP : "(unset)",
              NET_LOOPBACK_TEST_ADDRESS,
              NET_LOOPBACK_TEST_ADDRESS);

    return false;
}

/*
====================
NET_InitLoopbackAuthTest

Registers sv_loopbackTestAuth and captures it for the rest of the process.
NET_Init() calls this after the engine's start-up dvars exist and before the
first bind, so the server handshake path never observes a half-settled answer.

The capture happens once.  A later call (a second NET_Init after NET_Shutdown,
or anything else) only re-registers the dvar; it cannot change the frozen bool,
so a map reset or restart cannot grant a bypass the process did not start with.
====================
*/
void NET_InitLoopbackAuthTest()
{
    sv_loopbackTestAuth = Dvar_RegisterBool("sv_loopbackTestAuth", false, DVAR_INIT,
                                            "Allow unauthenticated local test clients (127.0.0.1 only; startup only; local testing only)");

    if (loopbackAuthTestFrozen)
        return;

    loopbackAuthTestEnabled = sv_loopbackTestAuth && sv_loopbackTestAuth->current.enabled;
    loopbackAuthTestFrozen = true;

    if (loopbackAuthTestEnabled && !warnedLoopbackAuthTest)
    {
        warnedLoopbackAuthTest = true;
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
                         "\n"
                         "=========================================================================\n"
                         "WARNING: sv_loopbackTestAuth is enabled.\n"
                         "This server will accept UNAUTHENTICATED connections and give them\n"
                         "synthetic non-Steam identities. Local processes are trusted; do not\n"
                         "forward this port. Protocol, challenge and password checks remain.\n"
                         "This is a LOCAL TESTING switch. Never enable it on a server that\n"
                         "other machines, a LAN, or the internet can reach.\n"
                         "The bypass lasts until this process exits; restarts and map\n"
                         "changes cannot turn it on or off.\n"
                         "=========================================================================\n");
    }
}

/*
====================
NET_LoopbackAuthTestEnabled

The captured start-up answer only.  It deliberately says nothing about the
current socket or the current cvar: callers must not treat this as permission,
because a socket may not be bound at all.  NET_CanBypassLoopbackAuth() answers
the authorisation question.
====================
*/
bool NET_LoopbackAuthTestEnabled()
{
    return loopbackAuthTestFrozen && loopbackAuthTestEnabled;
}

/*
====================
NET_CanBypassLoopbackAuth

The per-packet authorisation question.  Everything is re-checked here rather
than cached, because each of these can change under a running process:
the cvar can be rewritten by engine code, `dedicated` is a latched dvar, and
the socket is reopened by net_restart.

Note what is NOT used: Sys_IsLANAddress() and friends.  They answer "is this
address local-ish", which includes private ranges this guard must refuse, and
they treat every 127.x.x.x address as LAN.
====================
*/
bool NET_CanBypassLoopbackAuth(netadr_t from)
{
    if (!loopbackAuthTestFrozen || !loopbackAuthTestEnabled)
        return false;

    // The cvar must still agree with the start-up capture.  It is read here as
    // a veto only: a dvar that has been turned off revokes the bypass at once,
    // while a dvar turned on after start-up still grants nothing, because the
    // frozen bool stays false.  Reading it cannot create permission.
    if (!sv_loopbackTestAuth || !sv_loopbackTestAuth->current.enabled)
    {
        if (!warnedLoopbackAuthTestRevoked)
        {
            warnedLoopbackAuthTestRevoked = true;
            Com_PrintWarning(CON_CHANNEL_SYSTEM,
                             "WARNING: sv_loopbackTestAuth no longer agrees with its start-up value;"
                             " the local test bypass is refused while it stays changed.\n");
        }
        return false;
    }

    const dvar_t* net_ip = Dvar_FindVar("net_ip");
    if (!net_ip || !NET_LoopbackTestConfigSafe(net_ip->current.string))
        return false;

    if (!NET_SocketIsLoopbackBoundFd(ip_socket))
        return false;

    if (from.type != NA_IP)
        return false;

    return NET_LoopbackTestAddress(from.ip);
}

//=============================================================================
// Socket setup
//=============================================================================

/*
====================
NET_IPSocket

Non-blocking broadcast-capable UDP socket, bound to `port` on `net_interface`.
Returns the descriptor, or -1 after closing whatever it opened.
====================
*/
static int NET_IPSocket(const char *net_interface, int port)
{
    struct sockaddr_in address;
    struct sockaddr resolved;
    int broadcast = 1;
    int newsocket;

    if (net_interface && *net_interface)
        Com_Printf(CON_CHANNEL_SYSTEM, "Opening IP socket: %s:%i\n", net_interface, port);
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "Opening IP socket: localhost:%i\n", port);

    newsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (newsocket == NET_INVALID_SOCKET)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: UDP_OpenSocket: socket: %s\n", NET_ErrorString());
        return NET_INVALID_SOCKET;
    }

    if (!NET_SetNonBlocking(newsocket))
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: UDP_OpenSocket: fcntl O_NONBLOCK: %s\n", NET_ErrorString());
        close(newsocket);
        return NET_INVALID_SOCKET;
    }

    if (setsockopt(newsocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: UDP_OpenSocket: setsockopt SO_BROADCAST: %s\n", NET_ErrorString());
        close(newsocket);
        return NET_INVALID_SOCKET;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;

    if (net_interface && *net_interface && I_stricmp(net_interface, "localhost"))
    {
        if (!Sys_StringToSockaddr(net_interface, &resolved))
        {
            Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: UDP_OpenSocket: cannot resolve bind address '%s'\n", net_interface);
            close(newsocket);
            return NET_INVALID_SOCKET;
        }
        address.sin_addr = ((struct sockaddr_in *)&resolved)->sin_addr;
    }
    else
    {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    // Host-order port -> sockaddr field: one of the two places the contract
    // requires an explicit htons() (netadr_t itself stays network order).
    address.sin_port = (port == -1) ? 0 : htons((uint16_t)port);

    if (bind(newsocket, (struct sockaddr *)&address, sizeof(address)) == -1)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: UDP_OpenSocket: bind: %s\n", NET_ErrorString());
        close(newsocket);
        return NET_INVALID_SOCKET;
    }

    // With the local test bypass on, the configuration check that got us here is
    // only a promise: confirm with the kernel what this socket is really bound
    // to before it can receive a single packet.  A socket that landed on a
    // wildcard or LAN address is closed instead of used, and the mismatch is
    // fatal - continuing would serve unauthenticated test logins to the network.
    if (loopbackAuthTestEnabled && !NET_SocketIsLoopbackBoundFd(newsocket))
    {
        close(newsocket);
        Com_Error(ERR_FATAL,
                  "sv_loopbackTestAuth is enabled but the UDP socket bound to something other than %s.\n"
                  "Refusing to run an unauthenticated test server on a non-loopback address.\n"
                  "Set net_ip %s and dedicated 1, or turn sv_loopbackTestAuth off.\n",
                  NET_LOOPBACK_TEST_ADDRESS, NET_LOOPBACK_TEST_ADDRESS);
        return NET_INVALID_SOCKET;
    }

    return newsocket;
}

/*
====================
NET_GetLocalAddress

Enumerates live AF_INET interfaces.  Every address is recorded (loopback
included) because Sys_IsLANAddress() compares against the /24 of each one;
nothing here decides LAN-ness by itself.
====================
*/
int NET_GetLocalAddress()
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    char hostname[256];
    int result = 0;

    numIP = 0;

    if (gethostname(hostname, sizeof(hostname)) == 0)
    {
        hostname[sizeof(hostname) - 1] = '\0';
        Com_Printf(CON_CHANNEL_SYSTEM, "Hostname: %s\n", hostname);
    }

    if (getifaddrs(&ifaddr) == -1)
    {
        result = errno;
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_GetLocalAddress: getifaddrs: %s\n", NET_ErrorString());
        return result;
    }

    for (ifa = ifaddr; ifa && numIP < MAX_IPS; ifa = ifa->ifa_next)
    {
        const struct sockaddr_in *in;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        in = (const struct sockaddr_in *)ifa->ifa_addr;
        memcpy(localIP[numIP], &in->sin_addr, sizeof(localIP[numIP]));

        Com_Printf(CON_CHANNEL_SYSTEM, "IP: %i.%i.%i.%i\n",
                   localIP[numIP][0], localIP[numIP][1], localIP[numIP][2], localIP[numIP][3]);
        numIP++;
    }

    freeifaddrs(ifaddr);
    return result;
}

/*
====================
NET_OpenIP

Registers net_ip/net_port and binds, walking up to NET_MAX_PORT_ATTEMPTS ports
past net_port exactly as win_net.cpp did.  On success net_port is updated to
the port that actually bound, so the running server reports the truth.
====================
*/
static void NET_OpenIP()
{
    const dvar_t *net_ip = Dvar_RegisterString("net_ip", "localhost", DVAR_LATCH, "Network IP Address");
    const dvar_t *net_port = Dvar_RegisterInt("net_port", 28960, DvarLimits(0, 0xFFFF), DVAR_LATCH, "Network port");
    int basePort = net_port->current.integer;
    const char *bindIP = net_ip->current.string;
    int i;

    // Last thing before a descriptor exists: with the local test bypass on, the
    // address about to be bound has to be loopback exactly.  Com_Error is fatal,
    // so nothing below runs in production; returning keeps a non-fatal test
    // harness from opening the very socket the check refused.
    if (!NET_CheckLoopbackTestConfig(bindIP, "UDP bind"))
        return;

    for (i = 0; i < NET_MAX_PORT_ATTEMPTS; ++i)
    {
        ip_socket = NET_IPSocket(bindIP, i + basePort);
        if (ip_socket != NET_INVALID_SOCKET)
            break;
    }

    if (ip_socket == NET_INVALID_SOCKET)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: Couldn't allocate IP port\n");
        return;
    }

    if (i + basePort != basePort)
        Dvar_SetInt(net_port, i + basePort);

    // Remember what bound, so NET_GetDvars() only reports "modified" once per
    // actual net_ip/net_port change.
    if (bindIP)
    {
        I_strncpyz(lastNetIP, bindIP, sizeof(lastNetIP));
    }
    else
    {
        lastNetIP[0] = '\0';
    }
    lastNetPort = net_port->current.integer;
}

//=============================================================================
// CVars / configuration
//=============================================================================

/*
====================
NET_GetDvars

Returns true when a latched network setting changed and the sockets have to be
rebuilt.  win_net.cpp reported this for net_noudp/net_noipx/socks only, which
meant a latched net_ip/net_port change never re-bound; comparing against the
values that actually bound fixes that without any dvar-flag bookkeeping.
====================
*/
static bool NET_GetDvars()
{
    bool modified = false;
    const dvar_t *net_ip;
    const dvar_t *net_port;

    if (net_noudp && net_noudp->modified)
        modified = true;
    net_noudp = Dvar_RegisterBool("net_noudp", false, DVAR_ARCHIVE | DVAR_LATCH, "Disable UDP");

    if (net_noipx && net_noipx->modified)
        modified = true;
    net_noipx = Dvar_RegisterBool("net_noipx", false, DVAR_ARCHIVE | DVAR_LATCH, "Disable IPX");

    net_ip = Dvar_RegisterString("net_ip", "localhost", DVAR_LATCH, "Network IP Address");
    net_port = Dvar_RegisterInt("net_port", 28960, DvarLimits(0, 0xFFFF), DVAR_LATCH, "Network port");

    // The local test bypass demands loopback exactly.  Checked on every dvar
    // read - which is every NET_Config()/net_restart, and the boot path as
    // well, including when net_noudp stops the sockets from opening at all, so
    // an unsafe configuration is rejected even if it would not bind yet.  The
    // return value is not consulted here: the diagnostic is fatal, and
    // NET_OpenIP() repeats the check immediately before opening a socket.
    (void)NET_CheckLoopbackTestConfig(net_ip->current.string, "start-up");

    if (net_ip->current.string)
    {
        if (I_stricmp(net_ip->current.string, lastNetIP))
            modified = true;
    }
    if (net_port->current.integer != lastNetPort)
        modified = true;

    // Registered so existing config files keep loading.  SOCKS itself is not
    // implemented on this platform; see NET_OpenIP's caller.
    net_socksEnabled = Dvar_RegisterBool("net_socksEnabled", false, DVAR_ARCHIVE | DVAR_LATCH, "Enable network sockets (not supported on this platform)");
    net_socksServer = Dvar_RegisterString("net_socksServer", "", DVAR_ARCHIVE | DVAR_LATCH, "Network socket server (not supported on this platform)");
    net_socksPort = Dvar_RegisterInt("net_socksPort", 1080, DvarLimits(0, 0xFFFF), DVAR_ARCHIVE | DVAR_LATCH, "Network socket port (not supported on this platform)");
    net_socksUsername = Dvar_RegisterString("net_socksUsername", "", DVAR_ARCHIVE | DVAR_LATCH, "Network socket username (not supported on this platform)");
    net_socksPassword = Dvar_RegisterString("net_socksPassword", "", DVAR_ARCHIVE | DVAR_LATCH, "Network socket password (not supported on this platform)");

    return modified;
}

/*
====================
NET_WarnSocksUnsupported

win_net.cpp had a full SOCKS5 client.  It is not ported: report the request once
and carry on with a direct socket instead of silently pretending it worked.
====================
*/
static void NET_WarnSocksUnsupported()
{
    if (!warnedSocksUnsupported && net_socksEnabled && net_socksEnabled->current.enabled)
    {
        warnedSocksUnsupported = true;
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
                         "WARNING: net_socksEnabled is not supported on this platform; using a direct UDP socket\n");
    }
}

/*
====================
NET_Config

Starts/stops the sockets so that `enableNetworking` ends up effective.  Both
transitions are idempotent: closing already-closed sockets is a no-op and a
restart never leaves the previous descriptor behind.
====================
*/
void NET_Config(bool enableNetworking)
{
    int start;
    int stop;
    bool modified = NET_GetDvars();

    // No IPX on this platform, so "everything off" is expressed by net_noudp
    // alone; net_noipx is still honoured for config/command parity.
    if (net_noudp->current.enabled)
        enableNetworking = false;

    if (enableNetworking != networkingEnabled || modified)
    {
        if (enableNetworking == networkingEnabled)
        {
            if (enableNetworking)
            {
                stop = 1;
                start = 1;
            }
            else
            {
                stop = 0;
                start = 0;
            }
        }
        else
        {
            if (enableNetworking)
            {
                stop = 0;
                start = 1;
            }
            else
            {
                stop = 1;
                start = 0;
            }
            networkingEnabled = enableNetworking;
        }

        if (stop)
            NET_CloseSocket(&ip_socket);

        if (start)
        {
            NET_OpenIP();
            NET_WarnSocksUnsupported();
        }
    }
}

//=============================================================================
// Lifetime
//=============================================================================

/*
====================
NET_Init
====================
*/
void NET_Init()
{
    if (netInitialized)
        return;

    // No WSAStartup() equivalent exists, or is needed, on POSIX.
    netInitialized = true;
    Com_Printf(CON_CHANNEL_SYSTEM, "UDP network initialized\n");

    // Capture sv_loopbackTestAuth before anything binds, so the server
    // handshake path has a settled answer from its first challenge on.  This is
    // the only place the decision is taken; NET_Shutdown()/NET_Init() cycles
    // keep it.  See the loopback local-auth test guard section.
    NET_InitLoopbackAuthTest();

    NET_GetDvars();
    NET_Config(true);
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown()
{
    if (!netInitialized)
        return;

    NET_Config(false);
    NET_CloseSocket(&ip_socket);
    networkingEnabled = false;
    netInitialized = false;

    // The loopback local-auth test guard is deliberately not reset here: its
    // start-up decision outlives the sockets, so a shutdown/init cycle inside
    // one process can neither grant nor revoke the bypass.  With no socket
    // bound NET_CanBypassLoopbackAuth() refuses anyway.
}

/*
====================
NET_Restart

Re-binds with the current (possibly latched) net_ip/net_port.

Passing true rather than the cached networkingEnabled is deliberate: the caller
of NET_Restart (the `net_restart` command) means "bring the sockets back up with
the settings as they are now".  net_noudp still vetoes, so this honours the
cvar in both directions instead of letting a disable stick until process exit.
NET_Init()/NET_Shutdown() own the lifetime, so a shut-down subsystem stays down.
====================
*/
void NET_Restart()
{
    if (!netInitialized)
        return;

    NET_Config(true);
}

/*
====================
NET_Sleep

Sleeps msec, or less if the UDP socket becomes readable first.  Finite in all
cases: with no socket it is a plain EINTR-safe nanosleep, and with a socket the
select() is bounded by the deadline so an interrupted wait cannot hot-spin.
====================
*/
void NET_Sleep(int msec)
{
    struct timespec deadline;
    fd_set readfds;
    int err;

    if (ip_socket == NET_INVALID_SOCKET)
    {
        NET_SleepNs(msec);
        return;
    }

    if (ip_socket >= FD_SETSIZE)
    {
        // Defensive: select() cannot represent this descriptor.
        NET_SleepNs(msec);
        return;
    }

    NET_DeadlineFromNow(msec, &deadline);

    for (;;)
    {
        struct timeval timeout;
        int remainingMs = NET_RemainingMs(&deadline);

        FD_ZERO(&readfds);
        FD_SET(ip_socket, &readfds);

        timeout.tv_sec = remainingMs / 1000;
        timeout.tv_usec = (remainingMs % 1000) * 1000;

        err = select(ip_socket + 1, &readfds, NULL, NULL, &timeout);
        if (err > 0)
            return;                 // readable: let the caller pump packets
        if (err == 0)
            return;                 // deadline reached
        if (errno != EINTR)
            return;                 // real error: fall through to the caller

        if (NET_RemainingMs(&deadline) == 0)
            return;                 // interrupted at the deadline: stop
    }
}

//=============================================================================
// Debug socket
//=============================================================================

/*
====================
NET_InitDebug / NET_ShutdownDebug / NET_RestartDebug

qcommon/common.cpp drives these unconditionally from Com_Init and Com_Close, and
their only implementation (src/win32/win_net_debug.cpp) is built on Winsock and
Win32 threads.  The remote debug socket is out of scope for this port, so the
Linux build gets no-ops rather than a link error.  Nothing else references the
debug socket: Sys_IsRemoteDebugClient/Sys_*DebugSocket* are only used from the
Windows console and network code that this platform replaces.
====================
*/
void NET_InitDebug()
{
}

void NET_ShutdownDebug()
{
}

void NET_RestartDebug()
{
}

//=============================================================================
// Stream sockets
//=============================================================================

/*
====================
NET_Select

Waits up to 5s for the descriptor.  Only used by NET_TCPIPSocket's connect.
====================
*/
static bool NET_Select(int sock)
{
    struct timeval timeout;
    fd_set readfds;
    fd_set writefds;
    int err;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    err = select(sock + 1, &readfds, &writefds, NULL, &timeout);
    if (err == -1)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_Select: select: %s\n", NET_ErrorString());
        return false;
    }
    if (err == 0)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "NET_Select: timeout\n");
        return false;
    }
    return true;
}

/*
====================
NET_TCPIPSocket

type 0 binds, type 1 connects.  Returns the descriptor or -1.  NOTE: POSIX uses
-1 as the invalid socket, not win_net.cpp's 0, because 0 is a valid descriptor.
====================
*/
uint32_t NET_TCPIPSocket(const char *net_interface, int port, int type)
{
    struct sockaddr_in address;
    struct sockaddr resolved;
    int newsocket;
    int socketError;
    socklen_t socketErrorLength;

    if (net_interface)
        Com_Printf(CON_CHANNEL_SYSTEM, "Opening IP socket: %s:%i\n", net_interface, port);
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "Opening IP socket: localhost:%i\n", port);

    newsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (newsocket == NET_INVALID_SOCKET)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: socket: %s\n", NET_ErrorString());
        return (uint32_t)NET_INVALID_SOCKET;
    }

    if (!NET_SetNonBlocking(newsocket))
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: fcntl O_NONBLOCK: %s\n", NET_ErrorString());
        close(newsocket);
        return (uint32_t)NET_INVALID_SOCKET;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;

    if (net_interface && *net_interface && I_stricmp(net_interface, "localhost"))
    {
        if (!Sys_StringToSockaddr(net_interface, &resolved))
        {
            Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: cannot resolve '%s'\n", net_interface);
            close(newsocket);
            return (uint32_t)NET_INVALID_SOCKET;
        }
        address.sin_addr = ((struct sockaddr_in *)&resolved)->sin_addr;
    }
    else
    {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    address.sin_port = (port == -1) ? 0 : htons((uint16_t)port);

    if (type)
    {
        if (type == 1)
        {
            if (connect(newsocket, (struct sockaddr *)&address, sizeof(address)) == -1)
            {
                if (errno != EINPROGRESS && errno != EALREADY)
                {
                    Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: connect: %s\n", NET_ErrorString());
                    close(newsocket);
                    return (uint32_t)NET_INVALID_SOCKET;
                }

                // Non-blocking connect: wait, then read the real outcome from
                // SO_ERROR instead of trusting select() alone.
                if (!NET_Select(newsocket))
                {
                    Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: connect failed\n");
                    close(newsocket);
                    return (uint32_t)NET_INVALID_SOCKET;
                }

                socketError = 0;
                socketErrorLength = sizeof(socketError);
                if (getsockopt(newsocket, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) == -1 || socketError)
                {
                    Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: connect: %s\n",
                                     socketError ? strerror(socketError) : NET_ErrorString());
                    close(newsocket);
                    return (uint32_t)NET_INVALID_SOCKET;
                }
            }
        }
    }
    else if (bind(newsocket, (struct sockaddr *)&address, sizeof(address)) == -1)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "WARNING: NET_TCPIPSocket: bind: %s\n", NET_ErrorString());
        close(newsocket);
        return (uint32_t)NET_INVALID_SOCKET;
    }

    return (uint32_t)newsocket;
}
