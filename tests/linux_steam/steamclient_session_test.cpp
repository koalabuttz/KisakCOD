// Asset-free test of src/linux/steam/steamclient_session.cpp, the Linux Steam
// backend's connection to the running Steam client or, for a dedicated server,
// to Steam as an anonymous game server. It loads the stand-in
// steamclient.so built from fake_steamclient.cpp (path in argv[1]) through the
// real Session code: no Steam install is looked for or used.
//
// Usage: KisakCOD-steam-session-test <path/to/fake_steamclient.so>

#include <linux/steam/steamclient_session.h>

#include "fake_steamclient.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

FakeSteamControl *g_control = nullptr;
int g_validateId = 0;
const char *g_library = nullptr;

void Reset()
{
    *g_control = FakeSteamControl{};
    for (uint32_t i = 0; i < 40; ++i)
        g_control->ticket[i] = static_cast<unsigned char>(0xA0 + i);
    g_control->ticketSize = 40;
}

void TestDefaultLibraryPath()
{
    char path[256];
    setenv("KISAK_STEAMCLIENT_LIBRARY", "/opt/steam/steamclient.so", 1);
    Check(linux_steam::DefaultLibraryPath(path, sizeof(path)) && !std::strcmp(path, "/opt/steam/steamclient.so"),
        "KISAK_STEAMCLIENT_LIBRARY overrides the path");
    Check(!linux_steam::DefaultLibraryPath(path, 8) && path[0] == '\0', "an override that does not fit is refused");
    unsetenv("KISAK_STEAMCLIENT_LIBRARY");
    setenv("HOME", "/home/player", 1);
    const std::string expected = sizeof(void *) == 8 ? "/home/player/.steam/sdk64/steamclient.so"
                                                     : "/home/player/.steam/sdk32/steamclient.so";
    Check(linux_steam::DefaultLibraryPath(path, sizeof(path)) && expected == path,
        "HOME gives ~/.steam/sdk32 (i386) or sdk64 (x86_64)");
    setenv("KISAK_STEAMCLIENT_LIBRARY", "", 1);
    Check(linux_steam::DefaultLibraryPath(path, sizeof(path)) && expected == path, "an empty override is ignored");
    unsetenv("KISAK_STEAMCLIENT_LIBRARY");
    unsetenv("HOME");
    Check(!linux_steam::DefaultLibraryPath(path, sizeof(path)) && path[0] == '\0', "no HOME and no override: no path");
    Check(!linux_steam::DefaultLibraryPath(nullptr, 10), "no buffer: no path");
}

void TestOpenFailures()
{
    linux_steam::Session session;
    Check(session.Open(nullptr, 7940) == linux_steam::OpenResult::noLibraryPath, "no path: noLibraryPath");
    Check(session.Open("/nonexistent/kisak/steamclient.so", 7940) == linux_steam::OpenResult::libraryMissing,
        "missing library: libraryMissing");
    Check(session.LoaderError()[0] != '\0', "missing library: the loader's error is kept");
    Check(!session.IsOpen() && !session.LoggedOn() && session.SteamId() == 0, "missing library: nothing open");

    // A library that is not steamclient.so (this test's own C library has
    // none of the three exports).
    Check(session.Open("libc.so.6", 7940) == linux_steam::OpenResult::symbolMissing,
        "a library without the Steam exports: symbolMissing");

    Reset();
    g_control->serveClient = false;
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::noClientInterface,
        "no ISteamClient for this SDK's version: noClientInterface");
    Check(g_control->pipesCreated == 0, "noClientInterface: no pipe was made");

    Reset();
    g_control->pipe = 0;
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::noPipe, "Steam not running: noPipe");
    Check(g_control->pipesReleased == 0, "noPipe: nothing to release");

    Reset();
    g_control->user = 0;
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::noUser, "no logged-in user: noUser");
    Check(g_control->pipesReleased == 1 && g_control->releasedPipe == 7 && g_control->usersReleased == 0,
        "noUser: the pipe is released");

    Reset();
    g_control->serveUser = false;
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::noUserInterface,
        "no ISteamUser: noUserInterface");
    Check(g_control->usersReleased == 1 && g_control->releasedUser == 3 && g_control->pipesReleased == 1,
        "noUserInterface: the user and pipe are released");
    Check(!session.IsOpen(), "noUserInterface: nothing open");
    Check(session.BeginAuth(g_control->ticket, 40, 1) == -1, "closed session: BeginAuth answers -1");
    unsigned char buffer[64];
    uint32_t size = 99;
    Check(session.RequestTicket(1, 2, buffer, sizeof(buffer), &size) == 0 && size == 0,
        "closed session: no ticket");
    Check(g_control->beginCalls == 0 && g_control->ticketRequests == 0, "closed session: Steam is not asked");
    Check(session.Pump(nullptr, nullptr) == 0 && g_control->getCallbackCalls == 0, "closed session: no callbacks read");
}

void TestOpen(linux_steam::Session &session)
{
    Reset();
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::ok, "Open succeeds");
    Check(session.IsOpen(), "the session is open");
    Check(g_control->appIdEnvAtPipe == 7940 && g_control->gameIdEnvAtPipe == 7940,
        "SteamAppId and SteamGameId are 7940 when the pipe is made");
    Check(g_control->connectPipe == 7, "the global user is connected on the new pipe");
    Check(g_control->userVersionMatches && g_control->userInterfaceHandlesMatch,
        "ISteamUser is asked for by this SDK's version, with the pipe and user");
    Check(session.LoggedOn(), "LoggedOn reads BLoggedOn");
    Check(session.SteamId() == 76561197960287930ull, "SteamId reads GetSteamID");
    Check(session.AppId() == 0, "no ISteamUtils: the app id is unknown (0)");
    g_control->loggedOn = false;
    Check(!session.LoggedOn(), "a logged-off user reads as such");
    g_control->loggedOn = true;
}

void TestTickets(linux_steam::Session &session)
{
    unsigned char buffer[1024];
    uint32_t size = 0;
    const uint32_t handle = session.RequestTicket(0xC0A80102, 28960, buffer, sizeof(buffer), &size);
    Check(handle == 55 && size == 40 && !std::memcmp(buffer, g_control->ticket, 40),
        "RequestTicket returns Steam's handle and ticket bytes");
    Check(g_control->identityIsIp && g_control->identityIPv4 == 0xC0A80102 && g_control->identityPort == 28960,
        "the ticket is bound to the server's IPv4 address and port");
    Check(g_control->ticketCapacity == 1024, "Steam is told the buffer's size");

    g_control->ticketHandle = 0;
    Check(session.RequestTicket(1, 1, buffer, sizeof(buffer), &size) == 0 && size == 0,
        "an invalid handle from Steam: no ticket");
    Check(g_control->cancelCalls == 0, "an invalid handle is not cancelled");

    g_control->ticketHandle = 56;
    g_control->ticketSize = 0;
    Check(session.RequestTicket(1, 1, buffer, sizeof(buffer), &size) == 0 && size == 0,
        "an empty ticket: no ticket");
    Check(g_control->cancelCalls == 1 && g_control->cancelledHandle == 56, "an empty ticket's handle is cancelled");

    Reset();
    g_control->ticketHandle = 57;
    g_control->reportedTicketSize = 2048;
    Check(session.RequestTicket(1, 1, buffer, sizeof(buffer), &size) == 0 && size == 0,
        "a ticket larger than the buffer: no ticket");
    Check(g_control->cancelCalls == 1 && g_control->cancelledHandle == 57, "the oversized ticket is cancelled");

    Reset();
    Check(session.RequestTicket(1, 1, nullptr, 10, &size) == 0 && session.RequestTicket(1, 1, buffer, 0, &size) == 0 &&
              session.RequestTicket(1, 1, buffer, sizeof(buffer), nullptr) == 0,
        "no buffer, no capacity or no size: no ticket");
    Check(g_control->ticketRequests == 0, "bad arguments never reach Steam");

    session.CancelTicket(55);
    Check(g_control->cancelCalls == 1 && g_control->cancelledHandle == 55, "CancelTicket cancels the handle");
    session.CancelTicket(0);
    Check(g_control->cancelCalls == 1, "CancelTicket(0) does nothing");
}

void TestBeginAuth(linux_steam::Session &session)
{
    Reset();
    Check(session.BeginAuth(g_control->ticket, 40, 76561197960287931ull) == linux_steam::kBeginAuthOK,
        "BeginAuth returns Steam's OK");
    Check(g_control->beginCalls == 1 && g_control->beginSteamId == 76561197960287931ull &&
              g_control->beginSize == 40 && g_control->beginTicketMatches,
        "BeginAuthSession gets the ticket, its size and the claimed SteamID");
    g_control->beginResult = 2; // k_EBeginAuthSessionResultDuplicateRequest
    Check(session.BeginAuth(g_control->ticket, 40, 5) == 2, "BeginAuth passes Steam's refusal through");
    const int calls = g_control->beginCalls;
    Check(session.BeginAuth(nullptr, 40, 5) == 1 && session.BeginAuth(g_control->ticket, 0, 5) == 1 &&
              session.BeginAuth(g_control->ticket, 40, 0) == 1,
        "no ticket, no size or no SteamID: invalid ticket (1)");
    Check(g_control->beginCalls == calls, "invalid arguments never reach Steam");
    session.EndAuth(76561197960287931ull);
    Check(g_control->endCalls == 1 && g_control->endSteamId == 76561197960287931ull, "EndAuth ends that session");
    session.EndAuth(0);
    Check(g_control->endCalls == 1, "EndAuth(0) does nothing");
}

struct Seen
{
    std::vector<linux_steam::AuthVerdict> verdicts;
};

void OnVerdict(const linux_steam::AuthVerdict &verdict, void *context)
{
    static_cast<Seen *>(context)->verdicts.push_back(verdict);
}

void TestPump(linux_steam::Session &session)
{
    Reset();
    g_control->callbacks[0] = {304, -1, 0, 0, 0};
    g_control->callbacks[1] = {g_validateId, -1, 11, 11, 0};
    g_control->callbacks[2] = {163, -1, 0, 0, 0}; // GetAuthSessionTicketResponse_t
    g_control->callbacks[3] = {g_validateId, -1, 12, 13, 2};
    g_control->callbacks[4] = {g_validateId, 8, 14, 14, 3}; // truncated payload
    g_control->callbackCount = 5;
    Seen seen;
    Check(session.Pump(&OnVerdict, &seen) == 5, "Pump reads every pending callback");
    Check(g_control->freeCallbackCalls == 5 && g_control->freeWithoutCallback == 0 &&
              g_control->callbackReadWithoutFree == 0,
        "each callback is released once, before the next is read");
    Check(seen.verdicts.size() == 2, "only whole ValidateAuthTicketResponse callbacks are verdicts");
    if (seen.verdicts.size() == 2)
    {
        Check(seen.verdicts[0].steamId == 11 && seen.verdicts[0].ownerSteamId == 11 &&
                  seen.verdicts[0].response == linux_steam::kAuthResponseOK,
            "the first verdict is OK for SteamID 11");
        Check(seen.verdicts[1].steamId == 12 && seen.verdicts[1].ownerSteamId == 13 && seen.verdicts[1].response == 2,
            "the second verdict is response 2 for SteamID 12, owner 13");
    }
    Check(session.Pump(&OnVerdict, &seen) == 0 && seen.verdicts.size() == 2, "an empty queue reads nothing");
    g_control->callbacks[5] = {g_validateId, -1, 21, 21, 0};
    g_control->callbackCount = 6;
    Check(session.Pump(nullptr, nullptr) == 1 && g_control->freeCallbackCalls == 6,
        "without a verdict handler callbacks are still released");
}

void TestCloseAndReopen(linux_steam::Session &session)
{
    Reset();
    session.Close();
    Check(g_control->usersReleased == 1 && g_control->releasedUser == 3 && g_control->releasedUserPipe == 7 &&
              g_control->pipesReleased == 1 && g_control->releasedPipe == 7,
        "Close releases the user and the pipe");
    Check(!session.IsOpen() && session.SteamId() == 0 && !session.LoggedOn(), "a closed session answers nothing");
    session.Close();
    Check(g_control->pipesReleased == 1, "a second Close does nothing");
    Reset();
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::ok && session.IsOpen(), "the session reopens");
    Reset();
    {
        linux_steam::Session scoped;
        Check(scoped.Open(g_library, 7940) == linux_steam::OpenResult::ok, "a second session opens");
    }
    // A static session is destroyed at exit, after steamclient.so's own
    // teardown: calling into the library then crashed the game on quit.
    Check(g_control->pipesReleased == 0 && g_control->usersReleased == 0 && g_control->cancelCalls == 0,
        "the destructor does not call into the library");
}

struct ServerEvents
{
    std::vector<std::pair<linux_steam::ServerEvent, int>> events;
    std::vector<linux_steam::AuthVerdict> verdicts;
};

void OnServerEvent(linux_steam::ServerEvent event, int result, void *context)
{
    static_cast<ServerEvents *>(context)->events.emplace_back(event, result);
}

void OnServerVerdict(const linux_steam::AuthVerdict &verdict, void *context)
{
    static_cast<ServerEvents *>(context)->verdicts.push_back(verdict);
}

void TestGameServerFailures()
{
    linux_steam::Session session;
    Check(session.OpenGameServer(nullptr, 7940, 28960, "1.7") == linux_steam::OpenResult::noLibraryPath,
        "game server, no path: noLibraryPath");
    Check(session.OpenGameServer("/nonexistent/kisak/steamclient.so", 7940, 28960, "1.7") ==
              linux_steam::OpenResult::libraryMissing,
        "game server, missing library: libraryMissing");

    Reset();
    g_control->serverPipe = 0;
    Check(session.OpenGameServer(g_library, 7940, 28960, "1.7") == linux_steam::OpenResult::noServerUser,
        "no game-server pipe: noServerUser");
    Check(g_control->pipesReleased == 0 && g_control->usersReleased == 0, "noServerUser without a pipe: nothing released");

    Reset();
    g_control->serverUser = 0;
    Check(session.OpenGameServer(g_library, 7940, 28960, "1.7") == linux_steam::OpenResult::noServerUser,
        "no game-server user: noServerUser");
    Check(g_control->pipesReleased == 1 && g_control->releasedPipe == 9 && g_control->usersReleased == 0,
        "noServerUser: the game-server pipe is released");

    Reset();
    g_control->serveServer = false;
    Check(session.OpenGameServer(g_library, 7940, 28960, "1.7") == linux_steam::OpenResult::noServerInterface,
        "no ISteamGameServer: noServerInterface");
    Check(g_control->usersReleased == 1 && g_control->releasedUser == 4 && g_control->pipesReleased == 1 &&
              g_control->serverLogOffCalls == 0,
        "noServerInterface: user and pipe released, no LogOff");

    Reset();
    g_control->serverInitResult = false;
    Check(session.OpenGameServer(g_library, 7940, 28960, "1.7") == linux_steam::OpenResult::serverInitFailed,
        "InitGameServer refused: serverInitFailed");
    Check(g_control->serverLogOnAnonymousCalls == 0 && g_control->usersReleased == 1 && g_control->pipesReleased == 1,
        "serverInitFailed: no logon, user and pipe released");
    Check(!session.IsOpen() && !session.IsGameServer(), "serverInitFailed: nothing open");
}

void TestGameServer()
{
    Reset();
    linux_steam::Session session;
    Check(session.OpenGameServer(g_library, 7940, 28961, "1.7.0.0") == linux_steam::OpenResult::ok,
        "OpenGameServer succeeds");
    Check(session.IsOpen() && session.IsGameServer(), "the game-server session is open");
    Check(g_control->localUsersCreated == 1 && g_control->localUserAccountType == 3 /* k_EAccountTypeGameServer */,
        "a local game-server user is created");
    Check(g_control->pipesCreated == 0 && g_control->connectPipe == 0,
        "no pipe to the desktop Steam client and no global user");
    Check(g_control->serverVersionMatches && g_control->serverInterfaceHandlesMatch,
        "ISteamGameServer is asked for by this SDK's version, with the game-server pipe and user");
    Check(g_control->serverInits == 1 && g_control->serverInitAppId == 7940 && g_control->serverInitIp == 0 &&
              g_control->serverInitGamePort == 28961 && g_control->serverInitQueryPort == 0xffff &&
              g_control->serverInitFlags == 4 && !std::strcmp(g_control->serverInitVersion, "1.7.0.0"),
        "InitGameServer: app 7940, any address, the game port, shared query port, dedicated flag, version");
    Check(!std::strcmp(g_control->serverProduct, "cod4") && !std::strcmp(g_control->serverModDir, "main") &&
              g_control->serverDedicated,
        "product, mod dir and dedicated are set before logon");
    Check(g_control->serverLogOnAnonymousCalls == 1 && g_control->serverLogOnCalls == 0,
        "the server logs on anonymously, never with a token");
    Check(!session.LoggedOn() && session.SteamId() == 0, "until Steam answers, the server is not logged on");

    unsigned char buffer[64];
    uint32_t size = 99;
    Check(session.RequestTicket(1, 2, buffer, sizeof(buffer), &size) == 0 && size == 0 &&
              g_control->ticketRequests == 0 && g_control->serverTicketRequests == 0,
        "a game server issues no tickets");

    ServerEvents seen;
    g_control->callbacks[0] = {102, -1, 0, 0, 3, true};  // SteamServerConnectFailure_t, retrying
    g_control->callbacks[1] = {101, -1, 0, 0, 0};        // SteamServersConnected_t
    g_control->callbacks[2] = {103, -1, 0, 0, 6};        // SteamServersDisconnected_t
    g_control->callbacks[3] = {102, -1, 0, 0, 15, false}; // failure, no retry
    g_control->callbacks[4] = {102, 4, 0, 0, 2, false};   // truncated: ignored
    g_control->callbacks[5] = {g_validateId, -1, 31, 31, 0};
    g_control->callbackCount = 6;
    Check(session.Pump(&OnServerVerdict, &seen, &OnServerEvent) == 6, "Pump reads the game server's callbacks");
    Check(g_control->freeCallbackCalls == 6 && g_control->freeWithoutCallback == 0, "each is released once");
    const bool eventsOk = seen.events.size() == 4 &&
        seen.events[0] == std::make_pair(linux_steam::ServerEvent::connectRetrying, 3) &&
        seen.events[1] == std::make_pair(linux_steam::ServerEvent::connected, 0) &&
        seen.events[2] == std::make_pair(linux_steam::ServerEvent::disconnected, 6) &&
        seen.events[3] == std::make_pair(linux_steam::ServerEvent::connectFailed, 15);
    Check(eventsOk, "logon callbacks become retrying(3), connected, disconnected(6), failed(15); truncated ones are dropped");
    Check(seen.verdicts.size() == 1 && seen.verdicts[0].steamId == 31, "verdicts still reach onVerdict");

    g_control->serverLoggedOn = true;
    Check(session.LoggedOn() && session.SteamId() == 90071992547409920ull,
        "once logged on, LoggedOn and SteamId read the game server");

    Check(session.BeginAuth(g_control->ticket, 40, 76561197960287932ull) == linux_steam::kBeginAuthOK,
        "BeginAuth returns the game server's OK");
    Check(g_control->serverBeginCalls == 1 && g_control->beginCalls == 0 &&
              g_control->serverBeginSteamId == 76561197960287932ull && g_control->serverBeginTicketMatches,
        "BeginAuthSession goes to ISteamGameServer with the ticket and claimed SteamID");
    g_control->serverBeginResult = 1;
    Check(session.BeginAuth(g_control->ticket, 40, 5) == 1, "the game server's refusal is passed through");
    session.EndAuth(76561197960287932ull);
    Check(g_control->serverEndCalls == 1 && g_control->endCalls == 0 &&
              g_control->serverEndSteamId == 76561197960287932ull,
        "EndAuth goes to ISteamGameServer");

    session.Close();
    Check(g_control->serverLogOffCalls == 1 && g_control->usersReleased == 1 && g_control->releasedUser == 4 &&
              g_control->releasedUserPipe == 9 && g_control->pipesReleased == 1 && g_control->releasedPipe == 9,
        "Close logs off and releases the game-server user and pipe");
    Check(!session.IsOpen() && !session.IsGameServer() && !session.LoggedOn(), "a closed game server answers nothing");
    session.Close();
    Check(g_control->serverLogOffCalls == 1, "a second Close does nothing");

    Reset();
    Check(session.Open(g_library, 7940) == linux_steam::OpenResult::ok && !session.IsGameServer(),
        "the same session can reopen as a client");
    Check(session.BeginAuth(g_control->ticket, 40, 7) == 0 && g_control->beginCalls == 1 &&
              g_control->serverBeginCalls == 0,
        "a client session's BeginAuth goes to ISteamUser");
    ServerEvents clientSeen;
    g_control->callbacks[0] = {101, -1, 0, 0, 0};
    g_control->callbackCount = 1;
    session.Pump(nullptr, &clientSeen, &OnServerEvent);
    Check(clientSeen.events.empty(), "a client session reports no server events");
    session.Close();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <fake_steamclient.so>\n", argv[0]);
        return 2;
    }
    g_library = argv[1];
    void *fake = dlopen(g_library, RTLD_NOW | RTLD_LOCAL);
    if (!fake)
    {
        std::fprintf(stderr, "cannot load %s: %s\n", g_library, dlerror());
        return 2;
    }
    auto control = reinterpret_cast<FakeSteamControl *(*)()>(dlsym(fake, "FakeSteam_Control"));
    auto validateId = reinterpret_cast<int (*)()>(dlsym(fake, "FakeSteam_ValidateCallbackId"));
    if (!control || !validateId)
    {
        std::fprintf(stderr, "%s is not the stand-in steamclient.so\n", g_library);
        return 2;
    }
    g_control = control();
    g_validateId = validateId();

    TestDefaultLibraryPath();
    TestOpenFailures();
    linux_steam::Session session;
    TestOpen(session);
    TestTickets(session);
    TestBeginAuth(session);
    TestPump(session);
    TestCloseAndReopen(session);
    session.Close();
    TestGameServerFailures();
    TestGameServer();

    std::printf("%s: %d checks, %d failures (%zu-bit)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures,
        sizeof(void *) * 8);
    return g_failures ? 1 : 0;
}
