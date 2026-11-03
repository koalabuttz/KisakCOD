// Asset-free test of the production `connect` command on native Linux.
//
// Links the unmodified src/client_mp/cl_main_pc_mp.cpp (CL_Connect_f and
// CL_CDKeyValidate), the real NET_StringToAdr/NET_IsLocalAddress from
// src/qcommon/net_chan_mp.cpp and the real Sys_StringToAdr from src/linux/net.cpp.
// With --gc-sections only what CL_Connect_f reaches is kept; the client systems it
// calls around the address check are recording doubles below.
//
// What it pins down:
//   * "connect localhost" names the in-process server (NA_LOOPBACK) and goes
//     straight to CA_CHALLENGING without asking for authentication, which is the
//     retail path a listen server's own client takes.
//   * Every other address, including 127.0.0.1 over UDP, asks the platform for
//     authentication; on Linux that fails closed: the command drops with
//     EXE_ERR_INVALID_CD_KEY and the connection is never started.
#include <client_mp/client_mp.h>
#include <qcommon/qcommon.h>
#include <universal/assertive.h>
#include <sound/snd_public.h>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- storage whose owners are not linked ----------------------------------
clientActive_t clients[1];
clientConnection_t clientConnections[1];
clientUIActive_t clientUIActives[1];
clientStatic_t cls;
int cl_serverLoadingMap;
int g_waitingForServer;

clientConnection_t *CL_GetLocalClientConnection(int localClientNum)
{
    iassert(localClientNum == 0);
    return &clientConnections[0];
}

// ---- recording doubles ----------------------------------------------------
static const char *argv_[4];
static int argc_;
static int killLocalServer, svFrames, disconnects, profileQueries, closeAlls, loadScreens;
static int authErrors, otherErrors;
static char lastDrop[128];
static std::jmp_buf dropJump;
static bool dropArmed;

int Cmd_Argc() { return argc_; }
const char *Cmd_Argv(int index) { return index < argc_ ? argv_[index] : ""; }
void SND_StopSounds(snd_stopsounds_arg_t) {}
void SV_KillLocalServer() { ++killLocalServer; }
void SV_Frame(int) { ++svFrames; }
void CL_Disconnect(int) { ++disconnects; }
void Con_Close(int) {}
bool Com_HasPlayerProfile() { ++profileQueries; return true; }
void Cbuf_ExecuteBuffer(int, int, const char *) {}
void UI_CloseAll(int) { ++closeAlls; }
void SCR_UpdateLoadScreen() { ++loadScreens; }
uint32_t Sys_Milliseconds() { return 4242; }

void MyAssertHandler(const char *file, int line, int, const char *format, ...)
{
    std::fprintf(stderr, "assert %s:%d %s\n", file, line, format);
    std::abort();
}
void Com_Printf(int, const char *, ...) {}
void Com_DPrintf(int, const char *, ...) {}
void Com_PrintWarning(int, const char *, ...) {}
void Com_PrintError(int, const char *format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (std::strstr(text, "Client authentication unavailable"))
        ++authErrors;
    else
        ++otherErrors;
}
void Com_Error(errorParm_t code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(lastDrop, sizeof(lastDrop), format, args);
    va_end(args);
    if (!dropArmed || code != ERR_DROP)
    {
        std::fprintf(stderr, "unexpected Com_Error(%d, %s)\n", code, lastDrop);
        std::abort();
    }
    std::longjmp(dropJump, 1);
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

static void reset()
{
    std::memset(clientConnections, 0, sizeof(clientConnections));
    std::memset(clientUIActives, 0, sizeof(clientUIActives));
    std::memset(&cls, 0, sizeof(cls));
    clientUIActives[0].connectionState = CA_DISCONNECTED;
    killLocalServer = svFrames = disconnects = profileQueries = closeAlls = loadScreens = 0;
    authErrors = otherErrors = 0;
    lastDrop[0] = 0;
}

// Runs "connect <server>" and reports whether it dropped through Com_Error.
static bool connect(const char *server)
{
    argv_[0] = "connect";
    argv_[1] = server;
    argc_ = 2;
    dropArmed = true;
    if (setjmp(dropJump))
    {
        dropArmed = false;
        return true;
    }
    CL_Connect_f();
    dropArmed = false;
    return false;
}

static void refusedRemote(const char *server, const char *label)
{
    char name[160];
    reset();
    const bool dropped = connect(server);
    std::snprintf(name, sizeof(name), "%s: drops with EXE_ERR_INVALID_CD_KEY", label);
    check(dropped && !std::strcmp(lastDrop, "EXE_ERR_INVALID_CD_KEY"), name);
    std::snprintf(name, sizeof(name), "%s: address resolves as NA_IP, not local", label);
    check(clientConnections[0].serverAddress.type == NA_IP
        && !NET_IsLocalAddress(clientConnections[0].serverAddress), name);
    std::snprintf(name, sizeof(name), "%s: Linux reports authentication unavailable once", label);
    check(authErrors == 1 && otherErrors == 0, name);
    std::snprintf(name, sizeof(name), "%s: connection never starts", label);
    check(clientUIActives[0].connectionState == CA_DISCONNECTED && !profileQueries && !closeAlls
        && !loadScreens && !clientConnections[0].qport, name);
    std::snprintf(name, sizeof(name), "%s: local server is left running", label);
    check(!killLocalServer, name);
}

int main()
{
    g_qport = 0x1234; // net_chan_mp.cpp owns it; NET_Init would pick it.
    reset();
    check(!connect("localhost"), "localhost: no drop");
    check(clientConnections[0].serverAddress.type == NA_LOOPBACK, "localhost: resolves to NA_LOOPBACK");
    check(NET_IsLocalAddress(clientConnections[0].serverAddress), "localhost: counts as a local address");
    check(clientUIActives[0].connectionState == CA_CHALLENGING, "localhost: goes straight to CA_CHALLENGING");
    check(authErrors == 0 && otherErrors == 0, "localhost: never asks for platform authentication");
    check(clientConnections[0].qport == g_qport && clientConnections[0].connectTime == -99999
        && clientConnections[0].connectPacketCount == 0, "localhost: connection fields set as in retail");
    check(killLocalServer == 1 && svFrames == 1 && disconnects == 1 && profileQueries == 1
        && closeAlls == 1 && loadScreens == 1, "localhost: runs the retail connect sequence once");
    check(!std::strcmp(cls.servername, "localhost"), "localhost: server name kept");

    refusedRemote("192.0.2.10:28960", "remote IPv4");
    refusedRemote("127.0.0.1:28960", "UDP loopback 127.0.0.1");
    refusedRemote("127.0.0.1", "UDP loopback without port");

    // A bare CL_CDKeyValidate call on Linux refuses as well, whatever the address.
    reset();
    netadr_t address{};
    address.type = NA_IP;
    check(!CL_CDKeyValidate(address) && authErrors == 1, "CL_CDKeyValidate refuses NA_IP");

    // An already connected client is not touched (retail behaviour).
    reset();
    clientUIActives[0].connectionState = CA_ACTIVE;
    check(!connect("192.0.2.10:28960") && authErrors == 0 && !killLocalServer
        && clientUIActives[0].connectionState == CA_ACTIVE, "connected client: connect is refused before any lookup");

    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
