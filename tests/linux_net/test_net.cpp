// TEST-ONLY driver for src/linux/net.cpp (POSIX UDP network layer).
//
// Exercises the real implementation against real kernel sockets on loopback:
// binary payload round-trip, network byte order of netadr_t.port, non-blocking
// empty reads, oversize-datagram rejection without buffer overflow, zero-length
// datagrams, address parsing, LAN classification, NET_Sleep timing (including
// that a 1 ms wait does not degenerate into a poll), the
// net_port fallback walk, rebind/shutdown idempotency and descriptor leaks.
//
// Not part of any build target.  See tests/linux_net/README.md.

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <linux/net_internal.h>
#include "shim_engine.h"
#include "test_loopback_auth.h"

// ------------------------------------------------------------------ harness --

static int g_failures;
static int g_checks;

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

static double NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int CountOpenFds()
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] != '.')
            count++;
    }
    closedir(dir);
    return count - 1;   // the DIR's own descriptor
}

// Binds port 0 to learn a free UDP port, then releases it.
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

static int ServerPortFromDvar()
{
    const dvar_t *port = Shim_DvarFind("net_port");
    return port ? port->current.integer : -1;
}

static void SetDvarInt(const char *name, int value)
{
    Shim_DvarSeedInt(name, value);
    const dvar_t *found = Shim_DvarFind(name);
    Dvar_SetInt(found, value);
}

static uint16_t SendToLoopback(int sock, int port, const void *data, int length)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (sendto(sock, data, (size_t)length, 0, (struct sockaddr *)&addr, sizeof(addr)) != length)
        return (uint16_t)-1;
    return 0;
}

// ---------------------------------------------------------------- the tests --

static void TestLayout()
{
    Section("engine type layout");
    Check(sizeof(netadr_t) == 20, "netadr_t is 20 bytes");
    Check(sizeof(msg_t) == 40, "msg_t is 40 bytes");
    Check(offsetof(netadr_t, ip) == 4, "netadr_t.ip offset 4");
    Check(offsetof(netadr_t, port) == 8, "netadr_t.port offset 8");
}

static void TestPayloadRoundTrip(int peer, int peerPort, int serverPort)
{
    uint8_t payload[256];
    uint8_t buffer[1024 + 64];
    netadr_t from;
    msg_t msg;
    int i;

    Section("binary payload round-trip + byte order");

    for (i = 0; i < 256; ++i)
        payload[i] = (uint8_t)i;

    memset(buffer, 0, sizeof(buffer));
    memset(&msg, 0, sizeof(msg));
    memset(&from, 0, sizeof(from));
    msg.data = buffer;
    msg.maxsize = 1024;
    msg.cursize = -12345;

    Check(SendToLoopback(peer, serverPort, payload, sizeof(payload)) == 0, "peer sendto succeeded");

    NET_Sleep(100);

    Check(Sys_GetPacket(&from, &msg) == qtrue, "Sys_GetPacket returned a packet");
    Check(msg.cursize == (int)sizeof(payload), "cursize matches the 256-byte payload");
    Check(memcmp(buffer, payload, sizeof(payload)) == 0, "payload bytes are unmodified (binary safe)");
    Check(msg.readcount == 0, "readcount reset to 0");
    Check(from.type == NA_IP, "source address is NA_IP");
    Check(from.ip[0] == 127 && from.ip[1] == 0 && from.ip[2] == 0 && from.ip[3] == 1,
          "source ip octets are 127.0.0.1 in network order");
    Check(from.port == htons((uint16_t)peerPort),
          "netadr_t.port keeps network byte order (== htons(peer port))");

    // Non-blocking semantics: nothing left to read.
    {
        double t0 = NowMs();
        msg.cursize = -12345;
        Check(Sys_GetPacket(&from, &msg) == qfalse, "empty queue returns qfalse");
        Check(NowMs() - t0 < 5.0, "empty read did not block");
        Check(msg.cursize == -12345, "failed read leaves cursize untouched");
    }
}

static void TestOversize(int peer, int serverPort)
{
    static uint8_t big[300];
    uint8_t buffer[32 + 64];
    netadr_t from;
    msg_t msg;
    int i;

    Section("oversize datagram (MSG_TRUNC) rejection without overflow");

    memset(big, 0x5A, sizeof(big));
    memset(buffer, 0xAA, sizeof(buffer));
    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = 32;
    msg.cursize = -777;

    Check(SendToLoopback(peer, serverPort, big, sizeof(big)) == 0, "peer sent a 300-byte datagram");

    NET_Sleep(100);

    Check(Sys_GetPacket(&from, &msg) == qfalse, "oversize datagram is rejected");
    Check(msg.cursize == -777, "rejected packet leaves cursize untouched");

    for (i = 32; i < (int)sizeof(buffer); ++i)
    {
        if (buffer[i] != 0xAA)
            break;
    }
    Check(i == (int)sizeof(buffer), "guard bytes past maxsize were not written");

    // Socket must be drained, not stuck on the rejected datagram.
    {
        double t0 = NowMs();
        Check(Sys_GetPacket(&from, &msg) == qfalse, "socket is empty after the oversize drop");
        Check(NowMs() - t0 < 5.0, "post-drop read did not block");
    }

    // A legal datagram after the oversize one is still accepted.
    Check(SendToLoopback(peer, serverPort, "12345678", 8) == 0, "peer sent an 8-byte datagram");
    NET_Sleep(100);
    Check(Sys_GetPacket(&from, &msg) == qtrue, "legal datagram after the oversize drop is received");
    Check(msg.cursize == 8, "8-byte datagram has cursize 8");
}

static void TestZeroLength(int peer, int serverPort)
{
    uint8_t buffer[64];
    netadr_t from;
    msg_t msg;

    Section("zero-length datagram");

    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = 64;
    msg.cursize = -1;

    Check(SendToLoopback(peer, serverPort, "", 0) == 0, "peer sent a zero-length datagram");
    NET_Sleep(100);

    Check(Sys_GetPacket(&from, &msg) == qtrue, "zero-length datagram accepted");
    Check(msg.cursize == 0, "zero-length datagram has cursize 0");
}

static void TestSendPacket(int peer, int peerPort)
{
    static uint8_t payload[64];
    uint8_t received[128];
    netadr_t to;
    ssize_t got;
    int i;

    Section("Sys_SendPacket");

    for (i = 0; i < (int)sizeof(payload); ++i)
        payload[i] = (uint8_t)(0xFF - i);   // includes 0x00 and 0xFF

    memset(&to, 0, sizeof(to));
    to.type = NA_IP;
    to.ip[0] = 127;
    to.ip[1] = 0;
    to.ip[2] = 0;
    to.ip[3] = 1;
    to.port = htons((uint16_t)peerPort);

    Check(Sys_SendPacket((int)sizeof(payload), payload, to) != 0, "Sys_SendPacket reports success");

    got = recvfrom(peer, received, sizeof(received), 0, NULL, NULL);
    Check(got == (ssize_t)sizeof(payload), "peer received the full datagram");
    Check(got > 0 && memcmp(received, payload, sizeof(payload)) == 0, "sent bytes match received bytes");

    // Bad address types are a fatal engine error; only exercise the good pair.
    to.type = NA_IP;
    Check(Sys_SendPacket(1, payload, to) != 0, "1-byte send succeeds");
    recvfrom(peer, received, sizeof(received), 0, NULL, NULL);
}

static void TestAddressParsing()
{
    netadr_t a;

    Section("Sys_StringToAdr / address parsing");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("127.0.0.1", &a) == qtrue, "127.0.0.1 resolves");
    Check(a.type == NA_IP, "127.0.0.1 is NA_IP");
    Check(a.ip[0] == 127 && a.ip[1] == 0 && a.ip[2] == 0 && a.ip[3] == 1, "127.0.0.1 octets correct");
    Check(a.port == 0, "Sys_StringToAdr leaves the port 0 (host:port is the caller's job)");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("localhost", &a) == qtrue, "localhost resolves");
    Check(a.ip[0] == 127 && a.ip[3] == 1, "localhost resolves to an IPv4 loopback address");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("255.255.255.255", &a) == qtrue, "255.255.255.255 resolves numerically");
    Check(a.ip[0] == 255 && a.ip[1] == 255 && a.ip[2] == 255 && a.ip[3] == 255,
          "broadcast address survives so NET_StringToAdr can reject it");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("127.1", &a) == qtrue, "legacy short form 127.1 resolves");
    Check(a.ip[0] == 127 && a.ip[3] == 1, "127.1 maps to 127.0.0.1");

    memset(&a, 0, sizeof(a));
    a.type = NA_IP;
    Check(Sys_StringToAdr("no-such-host.invalid", &a) == qfalse, "unknown host name fails");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("", &a) == qfalse, "empty string fails");

    memset(&a, 0, sizeof(a));
    Check(Sys_StringToAdr("28960", &a) == qtrue, "bare integer parses as a numeric address (inet_aton contract)");
}

static netadr_t MakeAdr(int a, int b, int c, int d)
{
    netadr_t adr;

    memset(&adr, 0, sizeof(adr));
    adr.type = NA_IP;
    adr.ip[0] = (unsigned char)a;
    adr.ip[1] = (unsigned char)b;
    adr.ip[2] = (unsigned char)c;
    adr.ip[3] = (unsigned char)d;
    return adr;
}

static void TestLANClassification()
{
    netadr_t adr;

    Section("Sys_IsLANAddress");

    adr = MakeAdr(0, 0, 0, 0);
    adr.type = NA_LOOPBACK;
    Check(Sys_IsLANAddress(adr) == true, "NA_LOOPBACK is LAN");

    adr.type = NA_BOT;
    Check(Sys_IsLANAddress(adr) == true, "NA_BOT is LAN");

    Check(Sys_IsLANAddress(MakeAdr(10, 1, 2, 3)) == true, "10.0.0.0/8 is LAN");
    Check(Sys_IsLANAddress(MakeAdr(127, 0, 0, 1)) == true, "127.0.0.0/8 is LAN");
    Check(Sys_IsLANAddress(MakeAdr(169, 254, 1, 1)) == true, "169.254.0.0/16 is LAN");
    Check(Sys_IsLANAddress(MakeAdr(172, 16, 0, 1)) == true, "172.16.0.0/12 lower bound is LAN");
    Check(Sys_IsLANAddress(MakeAdr(172, 31, 255, 254)) == true, "172.31.255.254 is LAN");
    Check(Sys_IsLANAddress(MakeAdr(172, 32, 0, 1)) == false, "172.32.0.1 is outside 172.16/12");
    Check(Sys_IsLANAddress(MakeAdr(192, 168, 1, 1)) == true, "192.168.0.0/16 is LAN");
    Check(Sys_IsLANAddress(MakeAdr(8, 8, 8, 8)) == false, "8.8.8.8 is NOT LAN");
    Check(Sys_IsLANAddress(MakeAdr(203, 0, 113, 7)) == false, "203.0.113.7 is NOT LAN (no blanket true)");

    adr = MakeAdr(8, 8, 8, 8);
    adr.type = NA_BAD;
    Check(Sys_IsLANAddress(adr) == false, "NA_BAD is NOT LAN");
}

static void TestSleepTiming(int peer, int serverPort)
{
    double t0;
    double elapsed;

    Section("NET_Sleep");

    t0 = NowMs();
    NET_Sleep(0);
    elapsed = NowMs() - t0;
    Check(elapsed < 20.0, "NET_Sleep(0) returns immediately");

    t0 = NowMs();
    NET_Sleep(120);
    elapsed = NowMs() - t0;
    Check(elapsed >= 100.0 && elapsed < 1000.0, "NET_Sleep(120) sleeps about 120ms when idle");

    // Readable socket must cut the sleep short.
    Check(SendToLoopback(peer, serverPort, "wake", 4) == 0, "peer queued a wake-up datagram");
    NET_Sleep(50);
    t0 = NowMs();
    NET_Sleep(2000);
    elapsed = NowMs() - t0;
    Check(elapsed < 500.0, "NET_Sleep returns early once the socket is readable");
}

// The frame loop calls NET_Sleep(1) to wait for the next packet. A sleep that
// returns without waiting is a poll: the caller loops straight back into it and
// an idle server burns a core with no players connected. 100 iterations must
// therefore consume measurable wall time.
static void TestSleepNoHotSpin()
{
    uint8_t buffer[64];
    netadr_t from;
    msg_t msg;
    double t0;
    double elapsed;
    int iteration;

    Section("NET_Sleep(1) waits instead of polling");

    // Drain first: a readable socket legitimately cuts a sleep short, and an
    // earlier test leaves one datagram queued.
    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = sizeof(buffer);
    for (iteration = 0; iteration < 1000 && Sys_GetPacket(&from, &msg) == qtrue; ++iteration)
        ;

    t0 = NowMs();
    for (iteration = 0; iteration < 100; ++iteration)
        NET_Sleep(1);
    elapsed = NowMs() - t0;

    printf("  (100 x NET_Sleep(1) took %.2f ms)\n", elapsed);
    Check(elapsed >= 50.0, "100 x NET_Sleep(1) on an idle socket waited at least 50ms");
    Check(elapsed <= 2000.0, "100 x NET_Sleep(1) on an idle socket did not oversleep");
}

// The same wait with no socket open, where NET_Sleep falls through to
// nanosleep. That path never rounded its timeout away; this keeps both paths
// honest about actually waiting.
static void TestSleepNoHotSpinWithoutSocket()
{
    double t0;
    double elapsed;
    int iteration;

    Section("NET_Sleep(1) waits with no socket open");

    NET_Shutdown();

    t0 = NowMs();
    for (iteration = 0; iteration < 100; ++iteration)
        NET_Sleep(1);
    elapsed = NowMs() - t0;

    printf("  (100 x NET_Sleep(1) took %.2f ms)\n", elapsed);
    Check(elapsed >= 50.0, "100 x NET_Sleep(1) without a socket waited at least 50ms");
    Check(elapsed <= 2000.0, "100 x NET_Sleep(1) without a socket did not oversleep");
}

static void TestPortFallback(int peer)
{
    struct sockaddr_in addr;
    int blocker = -1;
    int blockerPort = -1;
    int newPort;
    uint8_t buffer[64];
    netadr_t from;
    msg_t msg;

    Section("net_port fallback walk");

    // Occupy a port so the first bind attempt must fail.
    for (int attempt = 0; attempt < 8 && blocker == -1; ++attempt)
    {
        int candidate = PickFreeUdpPort();
        int sock;

        if (candidate <= 0)
            break;

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == -1)
            break;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)candidate);

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            blocker = sock;
            blockerPort = candidate;
        }
        else
        {
            close(sock);
        }
    }

    if (blocker == -1)
    {
        Check(false, "could not reserve a port for the fallback test");
        return;
    }

    SetDvarInt("net_port", blockerPort);
    NET_Restart();

    newPort = ServerPortFromDvar();
    Check(newPort > blockerPort && newPort <= blockerPort + 10,
          "net_port advanced within 10 ports past the busy one");
    Check(newPort != blockerPort, "net_port no longer points at the busy port");

    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = sizeof(buffer);
    Check(SendToLoopback(peer, newPort, "fallback", 8) == 0, "peer sent to the fallback port");
    NET_Sleep(100);
    Check(Sys_GetPacket(&from, &msg) == qtrue, "engine receives on the fallback port");
    Check(msg.cursize == 8, "fallback-port datagram has the right size");

    close(blocker);
}

static void TestRebindAndShutdown(int peer)
{
    int fdBefore;
    int fdAfter;
    int iteration;

    Section("rebind / shutdown idempotency and descriptor leaks");

    fdBefore = CountOpenFds();

    for (iteration = 0; iteration < 5; ++iteration)
    {
        int port = PickFreeUdpPort();

        if (port <= 0)
            break;
        SetDvarInt("net_port", port);
        NET_Restart();
        Check(ServerPortFromDvar() == port, "rebind latched the requested free port");
    }

    NET_Restart();
    NET_Restart();
    fdAfter = CountOpenFds();
    Check(fdAfter == fdBefore, "repeated rebinds leak no descriptors");

    // Shutdown really stops receiving.
    {
        int port = ServerPortFromDvar();
        uint8_t buffer[64];
        netadr_t from;
        msg_t msg;

        NET_Shutdown();
        NET_Shutdown();     // idempotent

        memset(&msg, 0, sizeof(msg));
        msg.data = buffer;
        msg.maxsize = sizeof(buffer);

        if (SendToLoopback(peer, port, "after-shutdown", 14) == 0)
        {
            NET_Sleep(30);
            Check(Sys_GetPacket(&from, &msg) == qfalse, "no packets are read after NET_Shutdown");
        }

        NET_Init();
        NET_Init();         // idempotent
        Check(CountOpenFds() == fdBefore, "init/shutdown cycles leak no descriptors");
    }
}

// NET_TCPIPSocket only binds/connects; putting the descriptor into listen state
// is the caller's job, exactly as with win_net.cpp.
static void StartListening(int sock)
{
    listen(sock, 1);
}

static void TestTcpSocket()
{
    uint32_t listener;
    uint32_t client;
    int port;

    Section("NET_TCPIPSocket");

    port = PickFreeUdpPort();
    if (port <= 0)
    {
        Check(false, "could not pick a port for the TCP socket test");
        return;
    }

    listener = NET_TCPIPSocket("127.0.0.1", port, 0);
    Check(listener != (uint32_t)-1, "bind-style socket created (failure is -1, never 0)");

    if (listener != (uint32_t)-1)
        StartListening((int)listener);

    client = NET_TCPIPSocket("127.0.0.1", port, 1);
    Check(client != (uint32_t)-1, "connect-style socket created");

    if (listener != (uint32_t)-1)
        close((int)listener);
    if (client != (uint32_t)-1)
        close((int)client);
}

// Sends to an explicit dotted-quad address.
static int SendToAddr(int sock, const char *ipString, int port, const void *data, int length)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ipString, &addr.sin_addr) != 1)
        return -1;

    return sendto(sock, data, (size_t)length, 0, (struct sockaddr *)&addr, sizeof(addr)) == length ? 0 : -1;
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

static void TestNetIPBind(int peer)
{
    char hostAddress[64];
    uint8_t buffer[64];
    netadr_t from;
    msg_t msg;
    int port;

    Section("net_ip drives the bind address");

    if (!FindLocalHostAddress(hostAddress, sizeof(hostAddress)))
    {
        printf("  SKIP  no non-loopback IPv4 interface on this host\n");
        return;
    }

    port = PickFreeUdpPort();
    if (port <= 0)
    {
        Check(false, "could not pick a port for the net_ip test");
        return;
    }

    Shim_DvarSeedString("net_ip", hostAddress);
    SetDvarInt("net_port", port);
    NET_Restart();

    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = sizeof(buffer);

    Check(SendToLoopback(peer, port, "loopback", 8) == 0, "peer sent to 127.0.0.1");
    NET_Sleep(80);
    Check(Sys_GetPacket(&from, &msg) == qfalse,
          "a socket bound to a specific address ignores 127.0.0.1 (net_ip took effect)");

    Check(SendToAddr(peer, hostAddress, port, "direct", 6) == 0, "peer sent to the bound address");
    NET_Sleep(80);
    Check(Sys_GetPacket(&from, &msg) == qtrue, "engine receives on the address net_ip selected");
    Check(msg.cursize == 6, "datagram on the bound address has the right size");

    Shim_DvarSeedString("net_ip", "localhost");
    SetDvarInt("net_port", PickFreeUdpPort());
    NET_Restart();
    Check(ServerPortFromDvar() > 0, "reverted to localhost binding");
}

static void TestSocksUnsupported(int peer)
{
    int port;
    uint8_t buffer[64];
    netadr_t from;
    msg_t msg;

    Section("net_socksEnabled reports explicit unsupported");

    ((dvar_s *)Shim_DvarFind("net_socksEnabled"))->current.enabled = true;
    port = PickFreeUdpPort();
    if (port <= 0)
    {
        Check(false, "could not pick a port for the SOCKS test");
        return;
    }
    SetDvarInt("net_port", port);
    printf("  (expect one 'not supported on this platform' warning below)\n");
    NET_Restart();

    // Networking must keep working over a direct socket.
    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = sizeof(buffer);
    Check(SendToLoopback(peer, ServerPortFromDvar(), "direct", 6) == 0, "peer sent over the direct socket");
    NET_Sleep(80);
    Check(Sys_GetPacket(&from, &msg) == qtrue, "direct UDP still works with net_socksEnabled set");

    ((dvar_s *)Shim_DvarFind("net_socksEnabled"))->current.enabled = false;
}

// -------------------------------------------------------------------- main ---

int main()
{
    int peer = -1;
    int peerPort = -1;
    int serverPort;
    int seedPort;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("POSIX network layer test (src/linux/net.cpp)\n");
    printf("sizeof(void*)=%u\n", (unsigned)sizeof(void *));

    if (sizeof(void *) != 4)
    {
        printf("FATAL: this test must run as a 32-bit binary\n");
        return 2;
    }

    TestLayout();

    // Before anything calls NET_Init(): the loopback auth guard freezes its
    // start-up answer once per process, so every start-up decision gets its own
    // forked child (see test_loopback_auth.cpp).
    RunLoopbackAuthScenarioProcesses();

    // Seed a free port before init so the bound port is predictable.
    seedPort = PickFreeUdpPort();
    if (seedPort <= 0)
    {
        printf("FATAL: could not pick a free UDP port\n");
        return 2;
    }
    Shim_DvarSeedInt("net_port", seedPort);

    peer = MakePeerSocket(&peerPort);
    if (peer == -1)
    {
        printf("FATAL: could not create the peer socket\n");
        return 2;
    }

    Section("NET_Init");
    NET_Init();
    serverPort = ServerPortFromDvar();
    Check(serverPort == seedPort, "NET_Init bound the seeded net_port");
    Check(Shim_DvarFind("net_ip") != NULL, "net_ip cvar registered");
    Check(Shim_DvarFind("net_noudp") != NULL, "net_noudp cvar registered");
    Check(Shim_DvarFind("net_noipx") != NULL, "net_noipx cvar registered for config parity");
    Check(Shim_DvarFind("net_socksEnabled") != NULL, "net_socksEnabled cvar registered for config parity");

    TestPayloadRoundTrip(peer, peerPort, serverPort);
    TestOversize(peer, serverPort);
    TestZeroLength(peer, serverPort);
    TestSendPacket(peer, peerPort);
    TestAddressParsing();
    TestLANClassification();
    TestSleepTiming(peer, serverPort);
    TestSleepNoHotSpin();
    TestPortFallback(peer);
    TestRebindAndShutdown(peer);
    TestNetIPBind(peer);
    TestSocksUnsupported(peer);
    TestTcpSocket();

    // net_noudp must actually disable networking.
    Section("net_noudp disables the sockets");
    {
        int port = ServerPortFromDvar();

        Shim_DvarSeedInt("net_port", port);
        {
            const dvar_t *noudp = Shim_DvarFind("net_noudp");
            Dvar_SetInt(Shim_DvarFind("net_port"), port);
            // net_noudp is a bool dvar in the shim; flip it directly.
            ((dvar_s *)noudp)->current.enabled = true;
        }
        NET_Restart();

        {
            uint8_t buffer[64];
            netadr_t from;
            msg_t msg;

            memset(&msg, 0, sizeof(msg));
            msg.data = buffer;
            msg.maxsize = sizeof(buffer);

            SendToLoopback(peer, port, "noudp", 5);
            NET_Sleep(30);
            Check(Sys_GetPacket(&from, &msg) == qfalse, "net_noudp closes the UDP socket");
        }

        ((dvar_s *)Shim_DvarFind("net_noudp"))->current.enabled = false;

        // Clearing net_noudp must bring the sockets back on the next restart.
        NET_Restart();
        {
            uint8_t buffer[64];
            netadr_t from;
            msg_t msg;

            memset(&msg, 0, sizeof(msg));
            msg.data = buffer;
            msg.maxsize = sizeof(buffer);

            Check(SendToLoopback(peer, port, "reenable", 8) == 0, "peer sent after clearing net_noudp");
            NET_Sleep(80);
            Check(Sys_GetPacket(&from, &msg) == qtrue, "clearing net_noudp restores the UDP socket");
        }
    }

    TestSleepNoHotSpinWithoutSocket();

    // This process never enabled sv_loopbackTestAuth, so it is the default case.
    RunLoopbackAuthDefaultChecks();

    NET_Shutdown();
    close(peer);

    printf("\n%d checks, %d failures\n", g_checks + LoopbackAuthCheckCount(),
           g_failures + LoopbackAuthFailureCount());
    return (g_failures + LoopbackAuthFailureCount()) == 0 ? 0 : 1;
}
