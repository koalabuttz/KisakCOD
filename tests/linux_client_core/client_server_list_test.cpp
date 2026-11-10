// Asset-free loopback test of how the client and server find each other.
//
// Links the unmodified production units that own the server list:
//   src/client_mp/cl_main_pc_mp.cpp   globalservers, infoResponse handling
//   src/client_mp/cl_main_mp.cpp      getserversResponse handling, localservers,
//                                     the master dvar guard
//   src/server_mp/sv_main_pc_mp.cpp   master address, heartbeat, end-of-game status
// on top of the shared standalone runtime (real dvars, commands and the engine's
// own UDP socket). The "master server", a rogue sender and a LAN game server are
// plain UDP sockets this test binds on 127.0.0.1, so every packet asserted here
// really went through the engine's send or receive path. Nothing outside the
// loopback interface is contacted: the only other send is the LAN scan's own
// broadcast, which stays on the local network segment by definition.
//
// What it pins down:
//   * With masterServerName empty (the registered default) the client sends no
//     list request and a dedicated server sends no heartbeat.
//   * A master that does not resolve, or "localhost", gets no packet either.
//   * With a master set, `globalservers` sends retail's `getservers` request to
//     it, and a `dedicated 2` server sends retail's heartbeats.
//   * A server list is accepted only from the master that was asked, only soon
//     after asking, and addresses a master must not send are dropped.
//   * A LAN server's infoResponse is listed; one for another protocol is not.
//   * Only an answer to the last LAN scan is listed: the reply has to echo that
//     scan's random token and arrive soon after it.
//   * A remote game server cannot change the master settings.
#include <client_mp/client_mp.h>
#include <client_mp/cl_master.h>
#include <client_mp/cl_connection_mp.h>
#include <server_mp/server_mp.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <linux/client/diagnostic_host.h>
#include <linux/net_internal.h>
#include <universal/assertive.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

// ---- storage whose owners are not linked ----------------------------------
const dvar_t *com_masterServerName;
const dvar_t *com_masterPort;
serverStatic_t svs;

// ---- recording doubles ----------------------------------------------------
static int gameCompleteStatusCalls;
static netadr_t gameCompleteStatusTo;
void SVC_GameCompleteStatus(netadr_t from)
{
    ++gameCompleteStatusCalls;
    gameCompleteStatusTo = from;
}

// ---- harness ---------------------------------------------------------------
static int checks, failures;
static void check(bool value, const char *name)
{
    ++checks;
    if (!value)
        ++failures;
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
}

// The runtime's console goes to stderr; swap it for a file to read what a
// command printed.
static std::string printedBy(const std::function<void()> &action)
{
    std::FILE *saved = stderr;
    std::FILE *capture = std::tmpfile();
    if (!capture)
        std::abort();
    stderr = capture;
    action();
    std::fflush(capture);
    stderr = saved;
    std::string text;
    char buffer[512];
    std::rewind(capture);
    for (size_t got; (got = std::fread(buffer, 1, sizeof(buffer), capture)) > 0;)
        text.append(buffer, got);
    std::fclose(capture);
    std::fputs(text.c_str(), stderr);
    return text;
}

struct Udp
{
    int fd = -1;
    sockaddr_in address{};

    Udp()
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in bindTo{};
        bindTo.sin_family = AF_INET;
        bindTo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&bindTo), sizeof(bindTo))
            || getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length))
        {
            std::perror("loopback UDP socket");
            std::exit(2);
        }
    }
    ~Udp() { close(fd); }
    int port() const { return ntohs(address.sin_port); }

    // One datagram, or false when none arrives within the timeout.
    bool receive(std::string &payload, sockaddr_in &from, int timeoutMs)
    {
        pollfd waiting{fd, POLLIN, 0};
        if (poll(&waiting, 1, timeoutMs) <= 0)
            return false;
        char buffer[2048];
        socklen_t length = sizeof(from);
        const ssize_t got = recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &length);
        if (got < 0)
            return false;
        payload.assign(buffer, static_cast<size_t>(got));
        return true;
    }
    bool silent(int timeoutMs = 150)
    {
        std::string payload;
        sockaddr_in from{};
        return !receive(payload, from, timeoutMs);
    }
    void send(const sockaddr_in &to, const std::string &payload)
    {
        sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    }
};

static uint8_t packetBuffer[65507];

// Waits for the engine's socket to hold a datagram and reads it the way the
// client frame does.
static bool enginePacket(netadr_t &from, msg_t &message)
{
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        std::memset(&message, 0, sizeof(message));
        message.data = packetBuffer;
        message.maxsize = sizeof(packetBuffer);
        std::memset(&from, 0, sizeof(from));
        if (NET_GetClientPacket(&from, &message))
            return true;
        NET_Sleep(100);
    }
    return false;
}

static void command(kisak::client::Runtime &runtime, const char *text)
{
    runtime.Queue(text);
    runtime.Tick();
}

static std::string oob(const std::string &text) { return std::string("\xff\xff\xff\xff", 4) + text; }

static std::string listEntry(int a, int b, int c, int d, int port)
{
    const char bytes[7] = {'\\', char(a), char(b), char(c), char(d), char(port >> 8), char(port & 0xff)};
    return std::string(bytes, sizeof(bytes));
}

static bool listed(int index, int a, int b, int c, int d, int port)
{
    const netadr_t &adr = cls.globalServers[index].adr;
    return adr.type == NA_IP && adr.ip[0] == a && adr.ip[1] == b && adr.ip[2] == c && adr.ip[3] == d
        && ntohs(adr.port) == port;
}

static cmd_function_s globalServersCommand, localServersCommand;

int main()
{
    kisak::client::Runtime runtime;
    // Same names, defaults and flags as Com_InitDvars; test_master_server.py
    // checks the production registration itself has the empty default.
    com_masterServerName = Dvar_RegisterString("masterServerName", "", DVAR_ARCHIVE, "Master server");
    com_masterPort = Dvar_RegisterInt("masterPort", 20810, DvarLimits(0, 0xFFFF), DVAR_ARCHIVE, "Master server port");
    Cmd_AddCommandInternal("globalservers", CL_GlobalServers_f, &globalServersCommand);
    Cmd_AddCommandInternal("localservers", CL_LocalServers_f, &localServersCommand);
    CL_ProtectMasterDvars();
    clientUIActives[0].connectionState = CA_DISCONNECTED;

    Udp master, rogue, lanServer;
    std::string payload, printed;
    sockaddr_in clientAddress{};
    char text[128];

    // ---- no master set: nobody is asked -------------------------------------
    cls.waitglobalserverresponse = 1;
    printed = printedBy([&] { command(runtime, "globalservers 0 1 full empty\n"); });
    check(!std::strcmp(com_masterServerName->current.string, ""), "no master: masterServerName is empty by default");
    check(master.silent(), "no master: globalservers sends nothing");
    check(printed.find("No master server is set") != std::string::npos, "no master: the console says why the list is empty");
    check(cls.waitglobalserverresponse == 0 && !cl_masterQuery.asked, "no master: the client does not wait for a reply");

    for (const char *unusable : {"localhost", "255.255.255.255"})
    {
        std::snprintf(text, sizeof(text), "set masterServerName %s\n", unusable);
        command(runtime, text);
        cls.waitglobalserverresponse = 1;
        printed = printedBy([&] { command(runtime, "globalservers 0 1 full empty\n"); });
        std::snprintf(text, sizeof(text), "master %s: refused with a message, nothing asked", unusable);
        check(printed.find("Couldn't resolve master server address") != std::string::npos
            && cls.waitglobalserverresponse == 0 && !cl_masterQuery.asked, text);
    }
    check(master.silent(), "unusable master names: nothing was sent");

    // ---- a master is set: retail's request goes to it -----------------------
    std::snprintf(text, sizeof(text), "set masterServerName 127.0.0.1:%d\n", master.port());
    command(runtime, text);
    command(runtime, "globalservers 0 1 full empty\n");
    check(master.receive(payload, clientAddress, 2000) && payload == oob("getservers 1 full empty"),
        "master host:port: globalservers sends retail's getservers request to it");
    check(cls.waitglobalserverresponse == 1 && cl_masterQuery.asked, "master host:port: the client waits for the reply");

    command(runtime, "set masterServerName 127.0.0.1\n");
    std::snprintf(text, sizeof(text), "set masterPort %d\n", master.port());
    command(runtime, text);
    command(runtime, "globalservers 0 6 full empty\n");
    check(master.receive(payload, clientAddress, 2000) && payload == oob("getservers 6 full empty"),
        "master host + masterPort: the request uses masterPort and the asked protocol");

    printed = printedBy([&] { command(runtime, "globalservers 0\n"); });
    check(printed.find("usage: globalservers") != std::string::npos && master.silent(),
        "globalservers without a protocol prints usage and sends nothing");

    // ---- replies -------------------------------------------------------------
    netadr_t from;
    msg_t message;
    const std::string list = oob("getserversResponse")
        + listEntry(192, 0, 2, 20, 28960)
        + listEntry(69, 79, 84, 7, 28961)   // "EOT" as address bytes is still an address
        + listEntry(0, 0, 0, 0, 28960)       // not listable
        + listEntry(239, 1, 2, 3, 28960)     // multicast: not listable
        + listEntry(192, 0, 2, 21, 0)        // port 0: not listable
        + listEntry(127, 0, 0, 1, 28962)
        + std::string("\\EOT\0\0\0", 7);

    rogue.send(clientAddress, list);
    check(enginePacket(from, message), "rogue sender: its packet reaches the engine socket");
    CL_ServersResponsePacket(from, &message);
    check(cls.numglobalservers == 0 && cls.waitglobalserverresponse == 1,
        "rogue sender: a server list from an address that was not asked is ignored");

    master.send(clientAddress, list);
    check(enginePacket(from, message), "master reply: reaches the engine socket");
    CL_ServersResponsePacket(from, &message);
    check(cls.numglobalservers == 3 && cls.waitglobalserverresponse == 0,
        "master reply: three usable servers listed, three unusable addresses dropped");
    // The engine keeps the list sorted by port, then address, for its binary search.
    check(listed(0, 192, 0, 2, 20, 28960) && listed(1, 69, 79, 84, 7, 28961) && listed(2, 127, 0, 0, 1, 28962),
        "master reply: addresses and ports are right and in the engine's sorted order");
    check(cls.globalServers[0].dirty == 1 && cls.globalServers[0].ping == -1,
        "master reply: new entries are marked to be pinged");

    master.send(clientAddress, list);
    check(enginePacket(from, message), "repeated reply: reaches the engine socket");
    CL_ServersResponsePacket(from, &message);
    check(cls.numglobalservers == 3, "repeated reply: no server is listed twice");

    const std::string hostile[] = {
        oob("getserversResponse"),
        oob("getserversResponse\\"),
        oob("getserversResponse\\EOT"),
        oob("getserversResponse") + listEntry(192, 0, 2, 30, 28960),               // no separator after it
        oob("getserversResponse") + listEntry(192, 0, 2, 31, 28960).substr(0, 5),   // cut short
        std::string(1400, 'A'),
    };
    for (const std::string &packet : hostile)
    {
        master.send(clientAddress, packet);
        if (!enginePacket(from, message))
        {
            check(false, "malformed reply: reaches the engine socket");
            continue;
        }
        CL_ServersResponsePacket(from, &message);
    }
    check(cls.numglobalservers == 3, "malformed replies: nothing is listed from them and nothing breaks");

    cl_masterQuery.askedTime = Sys_Milliseconds() - (MASTER_RESPONSE_WINDOW_MS + 1000);
    master.send(clientAddress, oob("getserversResponse") + listEntry(192, 0, 2, 40, 28960) + std::string("\\EOT\0\0\0", 7));
    check(enginePacket(from, message), "late reply: reaches the engine socket");
    CL_ServersResponsePacket(from, &message);
    check(cls.numglobalservers == 3, "late reply: a list long after the request is ignored");

    // ---- LAN scan ------------------------------------------------------------
    // Delivers one infoResponse from `sender` to the production handler.
    const auto lanReply = [&](Udp &sender, const std::string &info, const char *name) {
        sender.send(clientAddress, oob("infoResponse\n" + info));
        check(enginePacket(from, message), name);
        MSG_BeginReading(&message);
        MSG_ReadLong(&message);
        MSG_ReadStringLine(&message);
        CL_ServerInfoPacket(from, &message, Sys_Milliseconds());
    };
    const std::string serverInfo = "\\hostname\\KisakCOD test\\mapname\\mp_crash\\sv_maxclients\\24\\gametype\\war";

    // The browser starts on Local (source 0) before anyone has scanned.
    cls.pingUpdateSource = 0;
    cls.numlocalservers = 0;
    check(!cl_lanScan.asked, "no scan yet: no token exists");
    lanReply(rogue, "\\protocol\\1\\challenge\\xxx" + serverInfo, "no scan yet: an unasked reply reaches the engine socket");
    lanReply(rogue, "\\protocol\\1\\challenge\\" + serverInfo, "no scan yet: a reply with an empty challenge reaches the engine socket");
    check(cls.numlocalservers == 0 && !cls.localServers[0].adr.port, "no scan yet: nobody is listed");

    cls.pingUpdateSource = 1;
    cls.numlocalservers = 5;
    command(runtime, "localservers\n");
    check(cls.numlocalservers == 0 && cls.pingUpdateSource == 0, "localservers: the local list is cleared for a new scan");
    const std::string token = cl_lanScan.token;
    check(cl_lanScan.asked && token.size() == 16 && token.find_first_not_of("0123456789abcdef") == std::string::npos,
        "localservers: the scan has a 16 digit token");
    command(runtime, "localservers\n");
    check(token != cl_lanScan.token, "localservers: each scan has its own token");
    const std::string challenge = std::string("\\challenge\\") + cl_lanScan.token;

    lanReply(lanServer, "\\protocol\\6" + challenge + serverInfo, "LAN reply, other protocol: reaches the engine socket");
    check(cls.numlocalservers == 0, "LAN reply, other protocol: not listed");

    lanReply(rogue, "\\protocol\\1" + serverInfo, "unasked reply, no challenge: reaches the engine socket");
    lanReply(rogue, "\\protocol\\1\\challenge\\xxx" + serverInfo, "unasked reply, retail's fixed challenge: reaches the engine socket");
    lanReply(rogue, "\\protocol\\1\\challenge\\" + token + serverInfo, "unasked reply, the previous scan's token: reaches the engine socket");
    lanReply(rogue, "\\protocol\\1" + challenge + "0" + serverInfo, "unasked reply, token with a digit added: reaches the engine socket");
    lanReply(rogue, "\\protocol\\1" + challenge.substr(0, challenge.size() - 1) + serverInfo,
        "unasked reply, token cut short: reaches the engine socket");
    check(cls.numlocalservers == 0 && !cls.localServers[0].adr.port, "unasked replies without this scan's token: nobody is listed");

    for (int repeat = 0; repeat < 2; ++repeat)
        lanReply(lanServer, "\\protocol\\1" + challenge + serverInfo, "LAN reply: reaches the engine socket");
    check(cls.numlocalservers == 1 && cls.localServers[0].adr.type == NA_IP
        && ntohs(cls.localServers[0].adr.port) == lanServer.port() && cls.localServers[0].adr.ip[0] == 127,
        "LAN reply: the server is listed once, at the address it answered from");

    cl_lanScan.askedTime = Sys_Milliseconds() - (LAN_SCAN_RESPONSE_WINDOW_MS + 1000);
    lanReply(rogue, "\\protocol\\1" + challenge + serverInfo, "late reply with the token: reaches the engine socket");
    check(cls.numlocalservers == 1, "late reply with the token: not listed long after the scan");

    // ---- a game server cannot choose the master ------------------------------
    clientUIActives[0].connectionState = CA_ACTIVE;
    printed = printedBy([&] {
        Dvar_SetFromStringByName("masterServerName", "192.0.2.66");
        Dvar_SetFromStringByName("masterPort", "1");
    });
    check(!std::strcmp(com_masterServerName->current.string, "127.0.0.1")
        && com_masterPort->current.integer == master.port(),
        "connected to a remote server: masterServerName and masterPort cannot be changed");
    check(printed.find("cannot be changed while connected") != std::string::npos,
        "connected to a remote server: the refusal is printed");
    Dvar_SetBool(com_sv_running, true);
    Dvar_SetFromStringByName("masterPort", "20811");
    check(com_masterPort->current.integer == 20811, "own listen server: the player can still change the settings");
    Dvar_SetBool(com_sv_running, false);
    clientUIActives[0].connectionState = CA_DISCONNECTED;
    std::snprintf(text, sizeof(text), "%d", master.port());
    Dvar_SetFromStringByName("masterPort", text);
    check(com_masterPort->current.integer == master.port(), "disconnected: the settings can be changed");

    // ---- server side ----------------------------------------------------------
    sockaddr_in serverAddress{};
    svs.time = 5000;
    svs.nextHeartbeatTime = INT_MIN;
    Dvar_SetInt(com_dedicated, 2);
    command(runtime, "set masterServerName \"\"\n");
    check(!SV_UsesMasterServer(), "server, no master: reports to nobody");
    SV_MasterHeartbeat("COD-4");
    SV_MasterGameCompleteStatus();
    SV_MasterShutdown();
    check(master.silent() && gameCompleteStatusCalls == 0 && SV_MasterAddress()->type == NA_BAD,
        "server, no master: no heartbeat, flatline or end-of-game status is sent");

    command(runtime, "set masterServerName 127.0.0.1\n");
    for (int mode = 0; mode < 2; ++mode)
    {
        Dvar_SetInt(com_dedicated, mode);
        svs.nextHeartbeatTime = INT_MIN;
        SV_MasterHeartbeat("COD-4");
        SV_MasterGameCompleteStatus();
    }
    check(master.silent() && gameCompleteStatusCalls == 0,
        "listen and LAN-only servers: a set master is still not contacted");

    Dvar_SetInt(com_dedicated, 2);
    svs.nextHeartbeatTime = INT_MIN;
    check(SV_UsesMasterServer(), "internet server with a master: reports to it");
    SV_MasterHeartbeat("COD-4");
    check(master.receive(payload, serverAddress, 2000) && payload == oob("heartbeat COD-4\n"),
        "internet server: retail's heartbeat goes to the master");
    check(svs.nextHeartbeatTime == svs.time + MASTER_HEARTBEAT_MSEC, "internet server: the next heartbeat is three minutes later");
    SV_MasterHeartbeat("COD-4");
    svs.time += MASTER_HEARTBEAT_MSEC - 1;
    SV_MasterHeartbeat("COD-4");
    check(master.silent(), "internet server: no heartbeat before it is due");
    svs.time += 1;
    SV_MasterHeartbeat("COD-4");
    check(master.receive(payload, serverAddress, 2000) && payload == oob("heartbeat COD-4\n"),
        "internet server: the heartbeat repeats when due");
    SV_MasterGameCompleteStatus();
    check(gameCompleteStatusCalls == 1 && gameCompleteStatusTo.type == NA_IP
        && ntohs(gameCompleteStatusTo.port) == master.port(), "internet server: end-of-game status goes to the master");
    SV_MasterShutdown();
    check(master.receive(payload, serverAddress, 2000) && payload == oob("heartbeat flatline\n"),
        "internet server: shutdown sends retail's flatline heartbeat");

    std::snprintf(text, sizeof(text), "set masterServerName 127.0.0.1:%d\n", rogue.port());
    command(runtime, text);
    svs.nextHeartbeatTime = INT_MIN;
    SV_MasterHeartbeat("COD-4");
    check(rogue.receive(payload, serverAddress, 2000) && payload == oob("heartbeat COD-4\n") && master.silent(),
        "internet server: a changed master setting is looked up again and used");

    command(runtime, "set masterServerName localhost\n");
    svs.nextHeartbeatTime = INT_MIN;
    SV_MasterHeartbeat("COD-4");
    check(SV_MasterAddress()->type == NA_BAD && master.silent() && rogue.silent(),
        "internet server: a master that does not resolve gets nothing");

    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
