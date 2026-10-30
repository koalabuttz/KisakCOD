// TEST-ONLY driver for the loopback local-auth test guard in src/linux/net.cpp:
// sv_loopbackTestAuth, NET_InitLoopbackAuthTest(), NET_LoopbackAuthTestEnabled(),
// NET_CanBypassLoopbackAuth() and the configuration gate in front of every bind.
//
// Two kinds of claim are asserted separately and must not be confused:
//
//   * what the guard answers (the two net functions), and
//   * what the kernel actually has bound, observed through a UDP round-trip on
//     127.0.0.1 and through bind() probes from a second loopback address.
//
// The second kind is what makes the negative cases real: this harness's
// Com_Error() only logs and returns where the engine aborts, so a printed
// "FATAL" proves nothing by itself.  A refused configuration has to show up as
// "the port is free and no datagram is read", never as "an error was printed".
//
// Every start-up decision needs its own process.  The guard captures the cvar
// once per process (NET_Init -> NET_InitLoopbackAuthTest) and NET_Shutdown()
// deliberately does not clear it, so scenarios are forked before the parent's
// first NET_Init() and report their check counts back over a pipe.
//
// Not part of any build target.  See tests/linux_net/README.md.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <linux/net_internal.h>

#include "shim_engine.h"
#include "test_loopback_auth.h"

// ------------------------------------------------------------------ harness --

static int g_checks;
static int g_failures;

static void Check(bool condition, const char *what)
{
    g_checks++;
    if (!condition)
    {
        g_failures++;
        printf("  FAIL  %s\n", what);
    }
    else
    {
        printf("  ok    %s\n", what);
    }
}

static void Section(const char *name)
{
    printf("\n== %s ==\n", name);
}

static void Skip(const char *what)
{
    printf("  SKIP  %s\n", what);
}

int LoopbackAuthCheckCount()
{
    return g_checks;
}

int LoopbackAuthFailureCount()
{
    return g_failures;
}

// ------------------------------------------------------------------ sockets --

// Binds port 0 on 127.0.0.1 to learn a free UDP port, then releases it.
static int PickFreeUdpPort()
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int port = -1;

    if (sock == -1)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(sock, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        port = ntohs(addr.sin_port);
    }

    close(sock);
    return port;
}

// A peer socket bound to 127.0.0.1 with an ephemeral port, standing in for a
// local test client.
static int MakePeerSocket(int *outPort)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == -1)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1 ||
        getsockname(sock, (struct sockaddr *)&addr, &addrlen) == -1)
    {
        close(sock);
        return -1;
    }

    *outPort = ntohs(addr.sin_port);
    return sock;
}

static int SendToLoopback(int sock, int port, const void *data, int length)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (sendto(sock, data, (size_t)length, 0, (struct sockaddr *)&addr, sizeof(addr)) != length)
        return -1;
    return 0;
}

// Returns 0 when a fresh UDP socket could bind ipString:port, else the errno
// the kernel reported (EINVAL when this helper itself could not build the
// address).  Deliberately no SO_REUSEADDR: without it, a port already taken on
// any address is reported as taken.
static int TryBindUdp(const char *ipString, int port)
{
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int result = 0;

    if (sock == -1)
        return errno;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ipString, &addr.sin_addr) != 1)
    {
        close(sock);
        return EINVAL;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        result = errno;

    close(sock);
    return result;
}

// True when nothing at all holds `port`: a wildcard bind succeeds only then, so
// this is the direct kernel-level answer to "was an unsafe socket opened?".
static bool PortIsFree(int port)
{
    return port > 0 && TryBindUdp("0.0.0.0", port) == 0;
}

// True when the engine's socket is bound to 127.0.0.1 specifically rather than
// to every interface: a wildcard holder excludes every other bind on the port,
// while a socket on 127.0.0.1 leaves 127.0.0.2 free.  Only meaningful together
// with a received datagram (an unused port would answer "free" as well).
static bool ServerSocketIsLoopbackSpecific(int port)
{
    return port > 0 && TryBindUdp("127.0.0.2", port) == 0 && !PortIsFree(port);
}

// Self-test for the probe above.  A kernel that refused 127.0.0.2:port even
// while 127.0.0.1:port was held would make ServerSocketIsLoopbackSpecific() a
// no-op, so the probe is skipped instead of silently passing.
static bool LoopbackProbeIsMeaningful()
{
    int port = PickFreeUdpPort();
    struct sockaddr_in addr;
    int loopSock;
    int otherErr;
    int wildcardErr;

    if (port <= 0)
        return false;

    loopSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (loopSock == -1)
        return false;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(loopSock, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        close(loopSock);
        return false;
    }

    otherErr = TryBindUdp("127.0.0.2", port);
    wildcardErr = TryBindUdp("0.0.0.0", port);
    close(loopSock);

    return otherErr == 0 && wildcardErr == EADDRINUSE;
}

// --------------------------------------------------- engine state + packets --

static int PortFromDvar()
{
    const dvar_t *port = Shim_DvarFind("net_port");
    return port ? port->current.integer : -1;
}

static netadr_t AdrIP(const char *dotted, unsigned short port)
{
    netadr_t adr;

    memset(&adr, 0, sizeof(adr));
    adr.type = NA_IP;
    inet_pton(AF_INET, dotted, adr.ip);
    adr.port = htons(port);
    return adr;
}

// The pseudo-types the engine also uses internally.  The ip bytes are set to
// 127.0.0.1 on purpose: the type check, not the address, has to reject them.
static netadr_t AdrPseudo(netadrtype_t type)
{
    netadr_t adr;

    memset(&adr, 0, sizeof(adr));
    adr.type = type;
    adr.ip[0] = 127;
    adr.ip[3] = 1;
    adr.port = htons(5555);
    return adr;
}

static void SetDvarString(const char *name, const char *value)
{
    Shim_DvarSeedString(name, value);
    ((dvar_s *)Shim_DvarFind(name))->current.string = value;
}

static void SetDvarBool(const char *name, bool value)
{
    Shim_DvarSeedBool(name, value);
}

static void SetDvarInt(const char *name, int value)
{
    Shim_DvarSeedInt(name, value);
}

static void DrainPackets()
{
    static uint8_t buffer[512];
    netadr_t from;
    msg_t msg;
    int guard;

    for (guard = 0; guard < 1000; ++guard)
    {
        memset(&msg, 0, sizeof(msg));
        msg.data = buffer;
        msg.maxsize = sizeof(buffer);
        if (Sys_GetPacket(&from, &msg) != qtrue)
            return;
    }
}

// Sends one datagram from a fresh 127.0.0.1 peer to `port` and reports whether
// the engine read back exactly those bytes, filling `fromOut` with the sender
// the engine recorded.
static bool EngineReadsLoopbackDatagram(int port, netadr_t *fromOut)
{
    static const char payload[] = "loopback-guard";
    static uint8_t buffer[512];
    const int payloadLength = (int)sizeof(payload) - 1;
    netadr_t from;
    msg_t msg;
    int peerPort = 0;
    int peer;
    bool received = false;

    memset(fromOut, 0, sizeof(*fromOut));
    if (port <= 0)
        return false;

    peer = MakePeerSocket(&peerPort);
    if (peer == -1)
        return false;

    DrainPackets();
    if (SendToLoopback(peer, port, payload, payloadLength) == 0)
    {
        NET_Sleep(40);
        memset(&msg, 0, sizeof(msg));
        msg.data = buffer;
        msg.maxsize = sizeof(buffer);
        if (Sys_GetPacket(&from, &msg) == qtrue && msg.cursize == payloadLength)
        {
            *fromOut = from;
            received = true;
        }
    }

    close(peer);
    return received;
}

// First live non-loopback IPv4 interface, or false when the host has none.
static bool FindLocalHostAddress(char *out, size_t outSize)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    bool found = false;

    if (getifaddrs(&ifaddr) == -1)
        return false;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next)
    {
        const struct sockaddr_in *in;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;

        in = (const struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)outSize))
        {
            found = true;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

// ------------------------------------------------------------ configuration --

// Models the engine start-up this harness cannot reproduce offline: the value
// the switch was given before the network layer initialised (on a real server
// that is "+set sv_loopbackTestAuth 1", which the engine keeps when the dvar is
// registered) plus the other dvars the guard reads.
static void SeedAuthStartup(bool authEnabled, int dedicated, const char *netIp, bool noudp, int port)
{
    Shim_DvarSeedBool("sv_loopbackTestAuth", authEnabled);

    if (dedicated < 0)
        Shim_ClearComDedicated();
    else
        Shim_DvarSeedInt("dedicated", dedicated);

    SetDvarString("net_ip", netIp);
    Shim_DvarSeedBool("net_noudp", noudp);
    Shim_DvarSeedInt("net_port", port);
}

// True only when every condition other than the start-up history holds: a
// dedicated server, net_ip exactly 127.0.0.1, and a socket really bound to it.
// A refusal under these conditions can only come from the captured start-up
// answer or from the cvar veto - which is the point of the checks that use it.
static bool GuardPreconditionsOtherwiseMet(int port)
{
    return com_dedicated && com_dedicated->current.integer != 0 && PortFromDvar() == port &&
           ServerSocketIsLoopbackSpecific(port);
}

// ---------------------------------------------------------------- scenarios --
//
// Each of these runs in its own process, before any NET_Init() in the parent.

static void ScenarioValidStartup()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("start-up: sv_loopbackTestAuth 1, net_ip 127.0.0.1, dedicated 1");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);

    // Nothing has been captured or bound yet: even with the cvar already on, the
    // guard must fail closed until NET_Init() takes the decision.
    Check(!NET_LoopbackAuthTestEnabled(), "before NET_Init nothing is captured: the answer is 'disabled'");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "before NET_Init a 127.0.0.1 client is refused");

    NET_Init();

    Check(NET_LoopbackAuthTestEnabled(), "the captured start-up answer is 'enabled'");
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a 127.0.0.1 client may bypass the handshake auth");
    Check(EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "the UDP socket is bound and really receiving on 127.0.0.1");
    Check(from.type == NA_IP && from.ip[0] == 127 && from.ip[1] == 0 && from.ip[2] == 0 &&
              from.ip[3] == 1,
          "the recorded sender is exactly 127.0.0.1 with type NA_IP");

    if (LoopbackProbeIsMeaningful())
    {
        Check(ServerSocketIsLoopbackSpecific(PortFromDvar()),
              "the kernel bind is 127.0.0.1 only, not INADDR_ANY");
    }
    else
    {
        Skip("this kernel cannot tell a 127.0.0.1 bind apart from a wildcard one");
    }

    NET_Shutdown();
}

static void ScenarioNetNoudp()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("start-up: enabled, safe, but net_noudp 1 leaves no socket to authorise");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", true, port);
    Shim_ComErrorReset();
    NET_Init();

    Check(NET_LoopbackAuthTestEnabled(), "the captured start-up answer is still 'enabled'");
    Check(Shim_ComErrorCount() == 0, "a safe configuration raises no diagnostic even when nothing binds");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "with no socket bound the bypass is refused");
    Check(PortIsFree(PortFromDvar()), "net_noudp 1 bound no port at all");
    Check(!EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "nothing is read, because no socket exists to read it");

    NET_Shutdown();
}

static void ScenarioPredicate()
{
    struct FromCase
    {
        const char *ip;
        netadrtype_t type;
        unsigned short port;
        bool expected;
        const char *what;
    };
    static const FromCase cases[] = {
        { "127.0.0.1", NA_IP, 0, true, "127.0.0.1 may bypass (port 0: the sender's port is free)" },
        { "127.0.0.1", NA_IP, 1, true, "127.0.0.1 may bypass from a privileged source port" },
        { "127.0.0.1", NA_IP, 54321, true, "127.0.0.1 may bypass from an ephemeral port" },
        { "127.0.0.1", NA_IP, 65535, true, "127.0.0.1 may bypass from the top of the port range" },
        { "127.0.0.2", NA_IP, 5555, false, "127.0.0.2 is refused (127/8 as a whole is not enough)" },
        { "127.0.0.10", NA_IP, 5555, false, "127.0.0.10 is refused" },
        { "127.1.0.1", NA_IP, 5555, false, "127.1.0.1 is refused" },
        { "10.1.2.3", NA_IP, 5555, false, "LAN 10.1.2.3 is refused" },
        { "172.16.5.4", NA_IP, 5555, false, "LAN 172.16.5.4 is refused" },
        { "192.168.1.10", NA_IP, 5555, false, "LAN 192.168.1.10 is refused" },
        { "169.254.10.1", NA_IP, 5555, false, "link-local 169.254.10.1 is refused" },
        { "8.8.8.8", NA_IP, 5555, false, "public 8.8.8.8 is refused" },
        { "203.0.113.9", NA_IP, 5555, false, "public 203.0.113.9 is refused" },
        { "0.0.0.0", NA_IP, 5555, false, "the unspecified address 0.0.0.0 is refused" },
        { "255.255.255.255", NA_IP, 5555, false, "a broadcast sender is refused even with type NA_IP" },
        { "224.0.0.1", NA_IP, 5555, false, "a multicast sender is refused" },
    };
    static const struct
    {
        netadrtype_t type;
        const char *what;
    } pseudoCases[] = {
        { NA_LOOPBACK, "the internal NA_LOOPBACK type is refused even with 127.0.0.1 bytes" },
        { NA_BOT, "the NA_BOT type is refused" },
        { NA_BAD, "the NA_BAD type is refused" },
        { NA_BROADCAST, "the NA_BROADCAST type is refused" },
        { NA_IPX, "the NA_IPX type is refused" },
    };
    const int port = PickFreeUdpPort();
    size_t i;

    Section("per-packet predicate: exactly 127.0.0.1 over type NA_IP");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "precondition: the enabled, loopback-bound server does authorise");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        bool answer = NET_CanBypassLoopbackAuth(AdrIP(cases[i].ip, cases[i].port));

        Check(answer == cases[i].expected, cases[i].what);
    }

    for (i = 0; i < sizeof(pseudoCases) / sizeof(pseudoCases[0]); ++i)
    {
        Check(!NET_CanBypassLoopbackAuth(AdrPseudo(pseudoCases[i].type)), pseudoCases[i].what);
    }

    NET_Shutdown();
}

static void ScenarioShutdownReinit()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("shutdown / re-init: the start-up decision is not recomputed");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "precondition: the bypass is live after start-up");

    NET_Shutdown();
    Check(NET_LoopbackAuthTestEnabled(), "NET_Shutdown does not clear the captured start-up answer");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "with the sockets closed the per-packet gate is closed");

    // Engine-internal caller turns the cvar off while the sockets are down.
    SetDvarBool("sv_loopbackTestAuth", false);
    NET_Init();
    Check(NET_LoopbackAuthTestEnabled(),
          "re-init keeps the captured mode instead of re-reading the cvar");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "the disagreeing cvar still vetoes after re-init");

    SetDvarBool("sv_loopbackTestAuth", true);
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "putting the cvar back to its start-up value restores the bypass (it adds no authority)");
    Check(EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "the socket opened by the re-init is bound and receiving on 127.0.0.1");

    NET_Shutdown();
}

static void ScenarioRuntimeRevoke()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("runtime cvar disagreement: revoke immediately, never upgrade");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "precondition: the bypass is live");

    SetDvarBool("sv_loopbackTestAuth", false);
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a cvar turned off by engine code revokes the bypass immediately");
    Check(EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "the socket is still up and loopback-bound, so the refusal came from the cvar");

    NET_Restart();
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_restart does not resurrect a revoked bypass");

    SetDvarBool("sv_loopbackTestAuth", true);
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "restoring the start-up value restores the bypass");

    NET_Shutdown();
}

static void ScenarioNetIpVeto()
{
    netadr_t from;
    char lanAddress[64];
    const int port = PickFreeUdpPort();

    Section("runtime net_ip / dedicated changes veto, whatever the socket is doing");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    if (!GuardPreconditionsOtherwiseMet(PortFromDvar()))
    {
        Skip("this kernel cannot confirm the loopback-specific bind; net_ip veto not asserted");
    }
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "precondition: the bypass is live");

    // net_ip is read per packet: a change now must veto even though the bound
    // socket is untouched and still on 127.0.0.1.
    SetDvarString("net_ip", "localhost");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_ip 'localhost' vetoes the bypass although the socket is still 127.0.0.1");
    Check(EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "the actual socket is still receiving on 127.0.0.1 (the veto is net_ip's)");

    SetDvarString("net_ip", "");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "an empty net_ip vetoes the bypass (it would bind every interface)");

    SetDvarString("net_ip", "0.0.0.0");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "net_ip 0.0.0.0 vetoes the bypass");

    SetDvarString("net_ip", "127.0.0.2");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_ip 127.0.0.2 vetoes the bypass (only 127.0.0.1 counts)");

    if (FindLocalHostAddress(lanAddress, sizeof(lanAddress)))
    {
        SetDvarString("net_ip", lanAddress);
        Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "a LAN net_ip vetoes the bypass");
    }
    else
    {
        Skip("no non-loopback IPv4 interface: LAN net_ip veto not asserted here");
    }

    SetDvarString("net_ip", "127.0.0.1");
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "restoring net_ip 127.0.0.1 restores the bypass");

    SetDvarInt("dedicated", 0);
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "dedicated 0 vetoes the bypass (listen server)");
    SetDvarInt("dedicated", 2);
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "dedicated 2 still counts as dedicated");

    Shim_ClearComDedicated();
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a process with no com_dedicated pointer vetoes the bypass");
    SetDvarInt("dedicated", 1);
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "dedicated 1 restores the bypass");

    NET_Shutdown();
}

static void ScenarioRestartSafe()
{
    netadr_t from;
    int bound;
    const int port = PickFreeUdpPort();

    Section("net_restart with a safe configuration stays local");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    bound = PortFromDvar();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "precondition: the bypass is live");

    NET_Restart();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_restart with a safe configuration keeps the bypass");
    Check(PortFromDvar() == bound, "net_restart re-bound the same loopback port");
    Check(EngineReadsLoopbackDatagram(bound, &from),
          "the re-bound socket is still receiving on 127.0.0.1");
    if (LoopbackProbeIsMeaningful())
    {
        Check(ServerSocketIsLoopbackSpecific(bound), "the re-bind stayed loopback-specific");
    }

    SetDvarBool("net_noudp", true);
    NET_Restart();
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_noudp 1 through net_restart closes the gate (no socket to authorise)");
    Check(PortIsFree(bound), "net_noudp 1 through net_restart closed the socket");

    SetDvarBool("net_noudp", false);
    NET_Restart();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "clearing net_noudp brings the loopback socket and the bypass back");

    NET_Shutdown();
}

static void ScenarioPendingLatched()
{
    netadr_t from;
    int bound;
    const int port = PickFreeUdpPort();

    Section("a pending latched net_ip change authorises nothing, and applying it fails closed");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    bound = PortFromDvar();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "precondition: the bypass is live");

    // net_ip is DVAR_LATCH on a real server: "net_ip 203.0.113.9" queues the new
    // value and the engine applies it on the next dvar read, not at once.
    ((dvar_s *)Shim_DvarFind("net_ip"))->latched.string = "203.0.113.9";
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a pending latched net_ip change neither revokes nor widens the bypass");
    if (LoopbackProbeIsMeaningful())
    {
        Check(ServerSocketIsLoopbackSpecific(bound),
              "the socket is still bound to 127.0.0.1 while the change is pending");
    }

    // The latched value becomes current on the next read - still without a rebind.
    SetDvarString("net_ip", "203.0.113.9");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "the applied change vetoes the bypass before any net_restart");
    Check(EngineReadsLoopbackDatagram(bound, &from),
          "the socket itself is still loopback-bound: the veto precedes the rebind");

    // And the rebind it asks for must be refused before an unsafe socket exists.
    Shim_ComErrorReset();
    NET_Restart();
    Check(Shim_ComErrorCount() > 0, "net_restart with an unsafe net_ip reports the configuration error");
    Check(PortIsFree(bound), "no socket was opened for the unsafe address");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "no bypass survives the refused restart");

    NET_Shutdown();
}

// The bind matrix: every unsafe net_ip has to be refused, with and without
// net_noudp, and every refusal has to leave no socket behind.
static void ScenarioUnsafeRestartMatrix()
{
    struct BindCase
    {
        char value[64];
        char label[96];
    };
    static BindCase cases[10];
    static char hostName[128];
    static char lanAddress[64];
    int caseCount = 0;
    int bound;
    int i;
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("unsafe net_ip matrix: refused, and no socket opened");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, "127.0.0.1", false, port);
    NET_Init();
    bound = PortFromDvar();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "precondition: a safe start-up is authorised");
    Check(EngineReadsLoopbackDatagram(bound, &from), "precondition: the loopback socket is receiving");

    // Build the matrix once: the literal traps first, then host-dependent ones.
    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "empty net_ip");
    caseCount++;

    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "localhost");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip localhost");
    caseCount++;

    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "0.0.0.0");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip 0.0.0.0");
    caseCount++;

    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "127.0.0.2");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip 127.0.0.2");
    caseCount++;

    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "127.1.0.1");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip 127.1.0.1");
    caseCount++;

    snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", "127.0.0.1 ");
    snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip '127.0.0.1 ' (trailing space)");
    caseCount++;

    if (FindLocalHostAddress(lanAddress, sizeof(lanAddress)))
    {
        snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", lanAddress);
        snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip %s (LAN)", lanAddress);
        caseCount++;
    }
    else
    {
        Skip("no non-loopback IPv4 interface: LAN bind case not run");
    }

    if (gethostname(hostName, sizeof(hostName)) == 0)
    {
        hostName[sizeof(hostName) - 1] = '\0';
        if (strcmp(hostName, "127.0.0.1") != 0)
        {
            snprintf(cases[caseCount].value, sizeof(cases[caseCount].value), "%s", hostName);
            snprintf(cases[caseCount].label, sizeof(cases[caseCount].label), "net_ip %s (host name)", hostName);
            caseCount++;
        }
    }

    for (i = 0; i < caseCount; ++i)
    {
        char what[192];

        SetDvarString("net_ip", cases[i].value);
        Shim_ComErrorReset();
        NET_Restart();

        snprintf(what, sizeof(what), "%s: refused with a diagnostic", cases[i].label);
        Check(Shim_ComErrorCount() > 0, what);

        snprintf(what, sizeof(what), "%s: no bypass afterwards", cases[i].label);
        Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), what);

        snprintf(what, sizeof(what), "%s: no socket exists on the port", cases[i].label);
        Check(PortIsFree(bound), what);

        snprintf(what, sizeof(what), "%s: nothing is received on 127.0.0.1", cases[i].label);
        Check(!EngineReadsLoopbackDatagram(bound, &from), what);
    }

    // The same matrix with net_noudp 1: the configuration is still inspected,
    // so an unsafe net_ip is refused even when it would not have bound today.
    // Note the fresh port: after a refused bind the layer keeps its previous
    // "last bound" values, and NET_Restart() only re-binds when something
    // differs, which a fatal Com_Error makes unreachable in production.
    bound = PickFreeUdpPort();
    SetDvarInt("net_port", bound);
    SetDvarString("net_ip", "127.0.0.1");
    SetDvarBool("net_noudp", false);
    NET_Restart();
    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a safe net_ip on a fresh port binds again after the refusals");

    SetDvarBool("net_noudp", true);
    for (i = 0; i < caseCount; ++i)
    {
        char what[192];

        SetDvarString("net_ip", cases[i].value);
        Shim_ComErrorReset();
        NET_Restart();

        snprintf(what, sizeof(what), "%s with net_noudp 1: still refused with a diagnostic",
                 cases[i].label);
        Check(Shim_ComErrorCount() > 0, what);

        snprintf(what, sizeof(what), "%s with net_noudp 1: nothing is bound", cases[i].label);
        Check(PortIsFree(bound), what);
    }

    NET_Shutdown();
}

// One unsafe value at real start-up: this is the first bind of the process, so
// nothing was ever bound safely before it.
static void ScenarioUnsafeStartup(const char *value, const char *what)
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section(what);
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 1, value, false, port);
    Shim_ComErrorReset();
    NET_Init();

    Check(NET_LoopbackAuthTestEnabled(),
          "the cvar capture is 'enabled' even though the configuration is unsafe");
    Check(Shim_ComErrorCount() > 0, "start-up refused with a diagnostic");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "no bypass is granted");
    Check(PortIsFree(PortFromDvar()), "start-up opened no socket");
    Check(!EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "nothing is read on 127.0.0.1, because no socket was opened");

    NET_Shutdown();
}

static void ScenarioUnsafeStartupEmpty()
{
    ScenarioUnsafeStartup("", "start-up with an empty net_ip (would bind every interface)");
}

static void ScenarioUnsafeStartupLocalhost()
{
    ScenarioUnsafeStartup("localhost", "start-up with net_ip localhost (the engine binds INADDR_ANY for it)");
}

static void ScenarioUnsafeStartupWildcard()
{
    ScenarioUnsafeStartup("0.0.0.0", "start-up with net_ip 0.0.0.0");
}

static void ScenarioDedicatedZero()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("start-up: enabled and loopback, but a listen server (dedicated 0)");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, 0, "127.0.0.1", false, port);
    Shim_ComErrorReset();
    NET_Init();

    Check(NET_LoopbackAuthTestEnabled(), "the cvar capture is 'enabled'");
    Check(Shim_ComErrorCount() > 0, "a listen server is refused at start-up");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)), "no bypass on a listen server");
    Check(PortIsFree(PortFromDvar()), "no socket was opened");
    Check(!EngineReadsLoopbackDatagram(PortFromDvar(), &from), "nothing is read");

    NET_Shutdown();
}

static void ScenarioDedicatedUnset()
{
    netadr_t from;
    const int port = PickFreeUdpPort();

    Section("start-up: enabled and loopback, but com_dedicated was never registered");
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SeedAuthStartup(true, -1, "127.0.0.1", false, port);
    Shim_ComErrorReset();
    NET_Init();

    Check(NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)) == false,
          "a NULL com_dedicated refuses instead of dereferencing it");
    Check(Shim_ComErrorCount() > 0, "the start-up is refused with a diagnostic");
    Check(PortIsFree(PortFromDvar()), "no socket was opened");
    Check(!EngineReadsLoopbackDatagram(PortFromDvar(), &from), "nothing is read");

    NET_Shutdown();
}

// ------------------------------------------------------- scenario processes --

struct ScenarioEntry
{
    const char *name;
    void (*run)();
};

static const ScenarioEntry g_scenarios[] = {
    { "valid-startup", ScenarioValidStartup },
    { "predicate", ScenarioPredicate },
    { "net-noudp", ScenarioNetNoudp },
    { "shutdown-reinit", ScenarioShutdownReinit },
    { "runtime-revoke", ScenarioRuntimeRevoke },
    { "net-ip-veto", ScenarioNetIpVeto },
    { "restart-safe", ScenarioRestartSafe },
    { "pending-latched", ScenarioPendingLatched },
    { "unsafe-restart-matrix", ScenarioUnsafeRestartMatrix },
    { "unsafe-startup-empty", ScenarioUnsafeStartupEmpty },
    { "unsafe-startup-localhost", ScenarioUnsafeStartupLocalhost },
    { "unsafe-startup-wildcard", ScenarioUnsafeStartupWildcard },
    { "dedicated-zero", ScenarioDedicatedZero },
    { "dedicated-unset", ScenarioDedicatedUnset },
};

// Runs one scenario in a child process and folds its counts into ours.  A child
// is required even for scenarios that never enable the switch: the guard's
// frozen decision is per process, and NET_Init() may only be reached once.
static void RunScenarioProcess(const ScenarioEntry *entry)
{
    int fds[2];
    pid_t pid;
    char line[64];
    size_t length = 0;
    int childChecks = 0;
    int childFailures = 0;
    int status = 0;
    bool reported;

    if (pipe(fds) == -1)
    {
        Check(false, "could not create the scenario result pipe");
        return;
    }

    printf("\n-- scenario %s (own process) --\n", entry->name);
    fflush(stdout);

    pid = fork();
    if (pid == -1)
    {
        close(fds[0]);
        close(fds[1]);
        Check(false, "could not fork a scenario process");
        return;
    }

    if (pid == 0)
    {
        int written;

        close(fds[0]);
        // fork() copies this process's totals; the child reports only its own.
        g_checks = 0;
        g_failures = 0;
        entry->run();
        fflush(stdout);
        written = snprintf(line, sizeof(line), "%d %d\n", g_checks, g_failures);
        if (written > 0)
        {
            ssize_t ignored = write(fds[1], line, (size_t)written);
            (void)ignored;
        }
        close(fds[1]);
        _exit(g_failures == 0 ? 0 : 1);
    }

    close(fds[1]);
    while (length < sizeof(line) - 1)
    {
        ssize_t got = read(fds[0], line + length, sizeof(line) - 1 - length);

        if (got <= 0)
            break;
        length += (size_t)got;
    }
    line[length] = '\0';
    close(fds[0]);

    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
        ;

    reported = sscanf(line, "%d %d", &childChecks, &childFailures) == 2;
    if (!reported)
    {
        Check(false, "the scenario process died without reporting its results");
        return;
    }

    g_checks += childChecks;
    g_failures += childFailures;
    printf("  ..    %s: %d checks, %d failures\n", entry->name, childChecks, childFailures);

    if (!WIFEXITED(status))
    {
        Check(false, "the scenario process did not exit normally");
    }
    else if ((WEXITSTATUS(status) == 0) != (childFailures == 0))
    {
        Check(false, "the scenario's exit code disagrees with its failure count");
    }
}

void RunLoopbackAuthScenarioProcesses()
{
    size_t i;

    Section("loopback auth guard: start-up decisions, one process each");
    printf("  (each scenario below runs in a forked child so it decides sv_loopbackTestAuth\n"
           "   before anything initialises the network layer)\n");

    for (i = 0; i < sizeof(g_scenarios) / sizeof(g_scenarios[0]); ++i)
        RunScenarioProcess(&g_scenarios[i]);
}

// ------------------------------------------------- this process's own checks --

// The parent process never seeds sv_loopbackTestAuth, so it is the "off by
// default" case: everything else is made permissive and the bypass must still
// be refused, no matter what an internal caller does to the cvar afterwards.
void RunLoopbackAuthDefaultChecks()
{
    netadr_t from;
    const dvar_t *auth;
    int port;

    Section("loopback auth guard: this process never enabled it");

    auth = Shim_DvarFind("sv_loopbackTestAuth");
    Check(auth != NULL, "net.cpp registered sv_loopbackTestAuth");
    if (auth)
    {
        Check(auth->flags == DVAR_INIT,
              "sv_loopbackTestAuth carries DVAR_INIT (0x10): start-up only, not settable later");
        Check(!auth->current.enabled, "sv_loopbackTestAuth defaults to off");
    }
    Check(!NET_LoopbackAuthTestEnabled(), "the captured start-up answer is 'disabled'");

    // Re-initialise with every other condition satisfiable, so a refusal can
    // only come from the start-up decision.
    port = PickFreeUdpPort();
    if (port <= 0)
    {
        Check(false, "could not pick a free UDP port");
        return;
    }

    SetDvarInt("dedicated", 1);
    SetDvarString("net_ip", "127.0.0.1");
    Shim_DvarSeedInt("net_port", port);
    Shim_DvarSeedBool("net_noudp", false);
    NET_Init();

    Check(PortFromDvar() == port, "the socket bound the seeded loopback port");
    Check(EngineReadsLoopbackDatagram(port, &from), "the socket is up and receiving on 127.0.0.1");
    if (LoopbackProbeIsMeaningful())
    {
        Check(ServerSocketIsLoopbackSpecific(port),
              "the socket is bound to 127.0.0.1 specifically, not to every interface");
    }
    else
    {
        Skip("this kernel cannot tell a 127.0.0.1 bind apart from a wildcard one");
    }

    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "a 127.0.0.1 client is refused: start-up did not enable the switch");

    // An engine-internal Dvar_Set (or a console user, for the record) turning the
    // cvar on after start-up must not create permission.
    SetDvarBool("sv_loopbackTestAuth", true);
    Check(!NET_LoopbackAuthTestEnabled(), "turning the cvar on afterwards does not change the captured answer");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "the per-packet guard still refuses 127.0.0.1 with the cvar on");

    NET_Restart();
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "net_restart cannot upgrade a process into bypass mode");

    NET_Shutdown();
    NET_Init();
    Check(!NET_LoopbackAuthTestEnabled(),
          "NET_Shutdown/NET_Init does not re-read the cvar into the captured answer");
    Check(!NET_CanBypassLoopbackAuth(AdrIP("127.0.0.1", 5555)),
          "the bypass is still refused after a re-init");
    Check(EngineReadsLoopbackDatagram(PortFromDvar(), &from),
          "the re-init socket is bound and receiving on 127.0.0.1");

    SetDvarBool("sv_loopbackTestAuth", false);
    NET_Shutdown();
}
