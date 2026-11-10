#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <win32/win_steam.h>

#ifndef KISAK_LINUX_STEAM

// The default native build has no Steam backend. Keep ticket verification
// explicitly unavailable rather than accepting unverified Steam IDs or loading
// a Windows DLL. KISAK_LINUX_STEAM builds the backend below instead.
bool g_steamInitialized = false;
uint64_t g_steamID = 0;
char g_steamName[64] = {};

void Steam_Init()
{
    Com_PrintWarning(CON_CHANNEL_SYSTEM,
        "Steam authentication is unavailable in the native Linux build; "
        "client tickets will be rejected. Do not use this build for gameplay.\n");
}

void Steam_Shutdown()
{
    g_steamInitialized = false;
}

bool Steam_CheckClientTicket(const void*, uint32, uint64_t)
{
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        Com_PrintError(CON_CHANNEL_SYSTEM,
            "Rejecting client: native Linux Steam authentication is not implemented.\n");
    }
    return false;
}

void Steam_CheckClients() {}
void Steam_OnClientDropped(uint64_t) {}
void Steam_SV_AddTestCommands() {}
void Steam_Frame() {}

#else // KISAK_LINUX_STEAM

// The Windows backend (src/win32/win_steam.cpp) on Linux. It talks to the
// Steam client already running on this machine through its steamclient.so
// (src/linux/steam/steamclient_session.h) and makes the same calls
// steam_api.dll makes for Windows: the client sends a ticket in place of a CD
// key, and the server checks it with BeginAuthSession, then drops the player if
// Steam's verdict comes back negative.
//
// Without a running, logged-in Steam client, Steam_Init leaves the backend off:
// the client cannot join remote servers and the server rejects every Steam
// ticket, exactly as the default build does.
//
// A dedicated server can instead log on to Steam as an anonymous game server
// (KISAK_STEAM_SERVER_LOGIN=anonymous, see Steam_Init): it then checks tickets
// without a Steam client or anyone's account on its machine. Until that logon
// completes it rejects every ticket.

#include <linux/steam/steamclient_session.h>
#include <qcommon/cmd.h>

#ifdef KISAK_MP
#include <server_mp/server_mp.h>
#include <server_mp/sv_handshake_auth.h>
#endif

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// 7940 = COD4 APPID
static constexpr uint32_t kSteamAppId = 7940;

bool g_steamInitialized = false;
uint64_t g_steamID = 0;
char g_steamName[64] = {};

static linux_steam::Session s_steam;
// Game-server sessions only: the logon outcome was printed once.
static bool s_serverLogonReported = false;
static uint32_t steamClientAuthTicket = 0;
static unsigned char steamClientTicket[1024] = {};
static uint32 steamClientTicketSize = 0;

static std::mutex kickPlayerMutex;
static std::vector<uint64_t> steamIDsToKick;
// SteamIDs this process started an auth session for (BeginAuthSession OK and
// not yet ended). Steam can deliver verdicts to every process logged in to the
// same account, so verdicts for anyone else are not ours to act on.
static std::vector<uint64_t> steamAuthSessions;

static bool Steam_HasAuthSession(uint64_t steamID64)
{
    for (uint64_t id : steamAuthSessions)
        if (id == steamID64)
            return true;
    return false;
}

static void Steam_ForgetAuthSession(uint64_t steamID64)
{
    for (size_t i = 0; i < steamAuthSessions.size(); ++i)
    {
        if (steamAuthSessions[i] == steamID64)
        {
            steamAuthSessions[i] = steamAuthSessions.back();
            steamAuthSessions.pop_back();
            return;
        }
    }
}

uint64_t Steam_GetClientSteamID64()
{
    iassert(g_steamID != 0);
    return g_steamID;
}

bool Steam_GetRawClientTicket(unsigned char **pBuffer, uint32 *pSize)
{
    if (!steamClientTicketSize)
        return false;
    *pSize = steamClientTicketSize;
    *pBuffer = steamClientTicket;
    return true;
}

void Steam_CancelClientTicket()
{
    if (steamClientAuthTicket)
    {
        s_steam.CancelTicket(steamClientAuthTicket);
        steamClientAuthTicket = 0;
    }
    steamClientTicketSize = 0;
}

static void Steam_OnVerdict(const linux_steam::AuthVerdict &verdict, void *)
{
    if (!Steam_HasAuthSession(verdict.steamId))
        return;
    if (verdict.response != linux_steam::kAuthResponseOK)
    {
        // Logged without the SteamID: the console log is often shared.
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] A player failed the Steam auth check (response %i)\n",
            verdict.response);
        std::lock_guard<std::mutex> _(kickPlayerMutex);
        steamIDsToKick.push_back(verdict.steamId);
    }
    else
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] A player passed the Steam auth check\n");
    }
}

static void Steam_OnServerEvent(linux_steam::ServerEvent event, int result, void *)
{
    switch (event)
    {
    case linux_steam::ServerEvent::connected:
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] Steam: logged on as an anonymous game server for app %u\n",
            kSteamAppId);
        s_serverLogonReported = true;
        break;
    case linux_steam::ServerEvent::connectRetrying:
        if (!s_serverLogonReported)
            Com_PrintWarning(CON_CHANNEL_SYSTEM,
                "[Kisak] Steam: game server logon failed (result %i), retrying. Tickets are rejected until it "
                "logs on.\n",
                result);
        s_serverLogonReported = true;
        break;
    case linux_steam::ServerEvent::connectFailed:
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: game server logon failed (result %i). Tickets are rejected.\n", result);
        s_serverLogonReported = true;
        break;
    case linux_steam::ServerEvent::disconnected:
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: game server lost its Steam connection (result %i). New tickets are rejected until "
            "it reconnects.\n",
            result);
        break;
    }
}

void Steam_CheckClients()
{
    if (!g_steamInitialized)
        return;
    s_steam.Pump(&Steam_OnVerdict, nullptr, &Steam_OnServerEvent);

    // Taken out under the lock and acted on outside it: dropping a player ends
    // their auth session, which must not run inside the verdict list's lock.
    std::vector<uint64_t> rejected;
    {
        std::lock_guard<std::mutex> _(kickPlayerMutex);
        rejected.swap(steamIDsToKick);
    }
#ifdef KISAK_MP
    if (com_sv_running && com_sv_running->current.enabled && sv_maxclients)
    {
        // Drops the players holding the SteamID64 and forgets its challenges,
        // so the verdict also holds for a connect that comes after it.
        for (uint64_t steamID64 : rejected)
            SV_OnSteamAuthRejected(steamID64);
    }
#endif
}

void Steam_Frame()
{
    // A dedicated server checks from SV_Frame, as on Windows. Everywhere else
    // (a listen server, or a client reading its own ticket callbacks) the
    // Linux main loop calls here once a frame so verdicts are not left queued.
    if (!g_steamInitialized || (com_dedicated && com_dedicated->current.integer))
        return;
    Steam_CheckClients();
}

#ifdef KISAK_DEDICATED
// KISAK_STEAM_SERVER_LOGIN=anonymous: log on as an anonymous Steam game server
// rather than as the Steam client's signed-in user. It has to be known before
// Com_Init (Steam starts first, as in WinMain), so it is an environment
// variable rather than a dvar. Any other value turns the backend off.
static bool Steam_InitGameServer(const char *path, const char *login)
{
    if (strcmp(login, "anonymous"))
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: KISAK_STEAM_SERVER_LOGIN must be \"anonymous\" or unset, not \"%s\". "
            "Steam authentication is off.\n",
            login);
        return false;
    }
    // The game port only describes the server to Steam; with the query port
    // shared, Steam opens no socket of its own and the server is not listed.
    const linux_steam::OpenResult result = s_steam.OpenGameServer(path, kSteamAppId, 28960, "1.7.0.0");
    if (result != linux_steam::OpenResult::ok)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: could not start an anonymous game server through %s (%s%s%s). "
            "Steam authentication is off.\n",
            path, linux_steam::OpenResultName(result), s_steam.LoaderError()[0] ? ": " : "",
            s_steam.LoaderError());
        return false;
    }
    s_serverLogonReported = false;
    g_steamInitialized = true;
    Com_Printf(CON_CHANNEL_SYSTEM,
        "[Kisak] Steam: logging on as an anonymous game server for app %u through %s\n", kSteamAppId, path);
    return true;
}
#endif

void Steam_Init()
{
    char path[512];
    if (!linux_steam::DefaultLibraryPath(path, sizeof(path)))
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: no steamclient.so path (set HOME or KISAK_STEAMCLIENT_LIBRARY). "
            "Steam authentication is off.\n");
        return;
    }
#ifdef KISAK_DEDICATED
    const char *serverLogin = getenv("KISAK_STEAM_SERVER_LOGIN");
    if (serverLogin && *serverLogin)
    {
        Steam_InitGameServer(path, serverLogin);
        return;
    }
#endif
    const linux_steam::OpenResult result = s_steam.Open(path, kSteamAppId);
    if (result != linux_steam::OpenResult::ok)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam: could not connect through %s (%s%s%s). Is the Steam client running? "
            "Steam authentication is off.\n",
            path, linux_steam::OpenResultName(result), s_steam.LoaderError()[0] ? ": " : "",
            s_steam.LoaderError());
        return;
    }
    if (!s_steam.LoggedOn())
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "[Kisak] Steam User isn't Logged On. Steam authentication is off.\n");
        s_steam.Close();
        return;
    }
    g_steamID = s_steam.SteamId();
    if (!g_steamID)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM, "[Kisak] Steam gave no SteamID. Steam authentication is off.\n");
        s_steam.Close();
        return;
    }
    const uint32_t appId = s_steam.AppId();
    if (appId && appId != kSteamAppId)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam runs this process as app %u, not %u. Steam authentication is off.\n", appId, kSteamAppId);
        s_steam.Close();
        g_steamID = 0;
        return;
    }
    g_steamInitialized = true;
    if (appId)
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] Steam: logged on as app %u through %s\n", appId, path);
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] Steam: logged on through %s (app id not reported)\n", path);
}

void Steam_Shutdown()
{
    if (!g_steamInitialized)
        return;
    Steam_CancelClientTicket();
    for (uint64_t id : steamAuthSessions)
        s_steam.EndAuth(id);
    steamAuthSessions.clear();
    s_steam.Close();
    g_steamInitialized = false;
    memset(g_steamName, 0, sizeof(g_steamName));
    g_steamID = 0;
}

bool Steam_UpdateClientAuthTicket(netadr_t serverIpv4)
{
    if (!g_steamInitialized)
    {
        Com_PrintError(CON_CHANNEL_CLIENT,
            "Client authentication unavailable: Steam is not running or not logged on\n");
        return false;
    }
    // A new connection gets a new ticket; the old one is no longer needed.
    Steam_CancelClientTicket();

    uint32_t ip32 = 0;
    ip32 |= (serverIpv4.ip[0] << 24);
    ip32 |= (serverIpv4.ip[1] << 16);
    ip32 |= (serverIpv4.ip[2] << 8);
    ip32 |= (serverIpv4.ip[3]);
    steamClientAuthTicket = s_steam.RequestTicket(ip32, serverIpv4.port, steamClientTicket,
        sizeof(steamClientTicket), &steamClientTicketSize);
    return steamClientAuthTicket != 0;
}

bool Steam_CheckClientTicket(const void *pAuthTicket, uint32 authTicketLen, uint64_t steamID64)
{
    if (!g_steamInitialized)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "[Kisak] Steam_CheckClientTicket() - Steam is not Initialized! Returning False\n");
        return false;
    }
    if (!steamID64)
        return false;
    if (s_steam.IsGameServer() && !s_steam.LoggedOn())
    {
        Com_Printf(CON_CHANNEL_SYSTEM,
            "[Kisak] Steam_CheckClientTicket() - the game server is not logged on to Steam yet. Returning False\n");
        return false;
    }
    int result = s_steam.BeginAuth(pAuthTicket, authTicketLen, steamID64);
    if (result == linux_steam::kBeginAuthDuplicateRequest && Steam_HasAuthSession(steamID64))
    {
        // A player reconnecting before this process ended their last session
        // (a listen server's SV_DropClient does not end it): end it and check
        // the new ticket instead. The new ticket still has to pass.
        s_steam.EndAuth(steamID64);
        Steam_ForgetAuthSession(steamID64);
        result = s_steam.BeginAuth(pAuthTicket, authTicketLen, steamID64);
#ifdef KISAK_MP
        // The old session is gone. If the new ticket did not start another,
        // nothing stands behind this SteamID64 any more: nobody keeps playing
        // or connects on the strength of the old one. (SV_GetChallenge lets a
        // request get here only when no connected player holds the SteamID64,
        // or the one who does has gone silent at the same address.)
        if (result != linux_steam::kBeginAuthOK)
            SV_OnSteamAuthRejected(steamID64);
#endif
    }
    if (result != linux_steam::kBeginAuthOK)
        return false;
    if (!Steam_HasAuthSession(steamID64))
        steamAuthSessions.push_back(steamID64);
    // Good for now: Steam's verdict arrives later, and Steam_CheckClients drops
    // the player if it is negative.
    return true;
}

void Steam_OnClientDropped(uint64_t steamID64)
{
    if (g_steamInitialized && steamID64)
    {
        s_steam.EndAuth(steamID64);
        Steam_ForgetAuthSession(steamID64);
    }
}

#ifdef KISAK_MP
static void Steam_SV_TestKick()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "Server is not running.\n");
        return;
    }
    if (SV_Cmd_Argc() != 2)
    {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "Usage: %s <SteamID64>\n", SV_Cmd_Argv(0));
        return;
    }
    uint64_t steamID64 = 0;
    if (!SV_ParseSteamIdentity(SV_Cmd_Argv(1), &steamID64))
    {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "Not a SteamID64.\n");
        return;
    }
    std::lock_guard<std::mutex> _(kickPlayerMutex);
    steamIDsToKick.push_back(steamID64);
}

static cmd_function_s Steam_SV_TestSteamKick_SERVER;
static cmd_function_s Steam_SV_TestSteamKick;
void Steam_SV_AddTestCommands()
{
    Cmd_AddCommandInternal("steam_testkick", Cbuf_AddServerText_f, &Steam_SV_TestSteamKick);
    Cmd_AddServerCommandInternal("steam_testkick", Steam_SV_TestKick, &Steam_SV_TestSteamKick_SERVER);
}
#else
void Steam_SV_AddTestCommands() {}
#endif

#endif // KISAK_LINUX_STEAM
