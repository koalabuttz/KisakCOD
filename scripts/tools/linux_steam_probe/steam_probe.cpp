// Feasibility probe: can a native Linux process log in to the running Steam
// client as Call of Duty 4 (app 7940) and get an auth session ticket?
//
// Nothing here links against Steam. The probe dlopen()s either the Steamworks
// redistributable (libsteam_api.so) or the Steam client's own steamclient.so,
// so it builds without any Steam files and only touches Steam when run.
//
// Build (32-bit, from the repository root):
//   g++ -m32 -std=c++17 -O1 -Ideps/steamsdk -o steam_probe
//     scripts/tools/linux_steam_probe/steam_probe.cpp -ldl
// (drop -m32 for x86_64.)
//
// Run from an empty private directory (it writes steam_appid.txt there):
//   steam_probe api <path/to/libsteam_api.so>
//   steam_probe client <path/to/steamclient.so>
//   steam_probe gameserver <path/to/steamclient.so>
//
// gameserver mode asks whether Steam lets an anonymous game server for app
// 7940 log on, as a dedicated server would to check players' tickets without
// anyone's Steam account. It needs Steam installed for steamclient.so but not
// running or logged in, and it never logs in as a user.
//
// The probe never prints ticket bytes, and only the last four digits of the
// SteamID, so its output can be shared.

#include <steam/steam_api.h>
#include <steam/isteamclient.h>
#include <steam/isteamgameserver.h>

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
constexpr AppId_t kCod4AppId = 7940;

// Interface versions this repository's headers describe. steamclient.so serves
// old interface versions, so asking by name keeps the vtables in step with the
// headers even if the libsteam_api.so on disk is older or newer.
const char kVersions[] =
    STEAMCLIENT_INTERFACE_VERSION "\0"
    STEAMUSER_INTERFACE_VERSION "\0"
    STEAMFRIENDS_INTERFACE_VERSION "\0"
    STEAMUTILS_INTERFACE_VERSION "\0"
    STEAMAPPS_INTERFACE_VERSION "\0";

using InternalInitFn = ESteamAPIInitResult (*)(const char *, SteamErrMsg *);
using BoolFn = bool (*)();
using VoidFn = void (*)();
using HandleFn = int32 (*)();
using CreateInterfaceFn = void *(*)(const char *);
using CreateInterfaceRetFn = void *(*)(const char *, int *);
using PipeFn = void (*)(HSteamPipe);
using NextCallbackFn = bool (*)(HSteamPipe, CallbackMsg_t *);
// steamclient.so's own exports. Some versions take a third out-parameter; a
// caller-cleaned cdecl call with an extra valid pointer is safe either way.
using ClientNextCallbackFn = bool (*)(HSteamPipe, CallbackMsg_t *, int32 *);

struct Session
{
    void *lib = nullptr;
    ISteamClient *client = nullptr;
    HSteamPipe pipe = 0;
    HSteamUser user = 0;
    bool manualDispatch = false;
    NextCallbackFn nextCallback = nullptr;
    PipeFn freeCallback = nullptr;
    PipeFn runFrame = nullptr;
    VoidFn shutdown = nullptr;
    ClientNextCallbackFn clientNextCallback = nullptr;
};

void *Sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    std::printf("  symbol %-36s %s\n", name, p ? "present" : "missing");
    return p;
}

void WriteAppIdFile()
{
    FILE *fp = std::fopen("steam_appid.txt", "w");
    if (fp)
    {
        std::fprintf(fp, "%u", kCod4AppId);
        std::fclose(fp);
    }
    setenv("SteamAppId", "7940", 1);
    setenv("SteamGameId", "7940", 1);
}

bool OpenApi(const char *path, Session *s)
{
    s->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s->lib)
    {
        std::printf("dlopen failed: %s\n", dlerror());
        return false;
    }
    auto internalInit = reinterpret_cast<InternalInitFn>(Sym(s->lib, "SteamInternal_SteamAPI_Init"));
    auto initFlat = reinterpret_cast<BoolFn>(Sym(s->lib, "SteamAPI_InitFlat"));
    auto initOld = reinterpret_cast<BoolFn>(Sym(s->lib, "SteamAPI_Init"));
    auto manualInit = reinterpret_cast<VoidFn>(Sym(s->lib, "SteamAPI_ManualDispatch_Init"));
    auto getPipe = reinterpret_cast<HandleFn>(Sym(s->lib, "SteamAPI_GetHSteamPipe"));
    auto getUser = reinterpret_cast<HandleFn>(Sym(s->lib, "SteamAPI_GetHSteamUser"));
    auto create = reinterpret_cast<CreateInterfaceFn>(Sym(s->lib, "SteamInternal_CreateInterface"));
    s->nextCallback = reinterpret_cast<NextCallbackFn>(Sym(s->lib, "SteamAPI_ManualDispatch_GetNextCallback"));
    s->freeCallback = reinterpret_cast<PipeFn>(Sym(s->lib, "SteamAPI_ManualDispatch_FreeLastCallback"));
    s->runFrame = reinterpret_cast<PipeFn>(Sym(s->lib, "SteamAPI_ManualDispatch_RunFrame"));
    s->shutdown = reinterpret_cast<VoidFn>(Sym(s->lib, "SteamAPI_Shutdown"));
    if (!getPipe || !getUser || !create)
        return false;

    if (manualInit && s->nextCallback && s->freeCallback && s->runFrame)
    {
        manualInit();
        s->manualDispatch = true;
    }

    bool ok = false;
    if (internalInit)
    {
        SteamErrMsg err = {};
        ESteamAPIInitResult r = internalInit(kVersions, &err);
        std::printf("SteamInternal_SteamAPI_Init -> %d (%s)\n", static_cast<int>(r), err);
        ok = r == k_ESteamAPIInitResult_OK;
    }
    else if (initFlat)
    {
        ok = initFlat();
        std::printf("SteamAPI_InitFlat -> %d\n", ok);
    }
    else if (initOld)
    {
        ok = initOld();
        std::printf("SteamAPI_Init -> %d\n", ok);
    }
    if (!ok)
        return false;
    s->pipe = getPipe();
    s->user = getUser();
    s->client = static_cast<ISteamClient *>(create(STEAMCLIENT_INTERFACE_VERSION));
    std::printf("pipe %d user %d client %p\n", s->pipe, s->user, static_cast<void *>(s->client));
    return s->client && s->pipe && s->user;
}

bool OpenClient(const char *path, Session *s)
{
    s->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s->lib)
    {
        std::printf("dlopen failed: %s\n", dlerror());
        return false;
    }
    auto create = reinterpret_cast<CreateInterfaceRetFn>(Sym(s->lib, "CreateInterface"));
    s->clientNextCallback = reinterpret_cast<ClientNextCallbackFn>(Sym(s->lib, "Steam_BGetCallback"));
    s->freeCallback = reinterpret_cast<PipeFn>(Sym(s->lib, "Steam_FreeLastCallback"));
    Sym(s->lib, "Steam_GetAPICallResult");
    if (!create)
        return false;
    int rc = 0;
    s->client = static_cast<ISteamClient *>(create(STEAMCLIENT_INTERFACE_VERSION, &rc));
    std::printf("CreateInterface(%s) -> %p rc %d\n", STEAMCLIENT_INTERFACE_VERSION,
        static_cast<void *>(s->client), rc);
    if (!s->client)
        return false;
    s->pipe = s->client->CreateSteamPipe();
    s->user = s->pipe ? s->client->ConnectToGlobalUser(s->pipe) : 0;
    std::printf("pipe %d user %d\n", s->pipe, s->user);
    return s->pipe && s->user;
}

bool NextCallback(Session *s, CallbackMsg_t *msg)
{
    if (s->manualDispatch)
        return s->nextCallback(s->pipe, msg);
    int32 call = 0;
    return s->clientNextCallback(s->pipe, msg, &call);
}

void PumpCallbacks(Session *s, int ms, HAuthTicket ticket, CSteamID self)
{
    const bool canRead = s->manualDispatch || (s->clientNextCallback && s->freeCallback);
    if (!canRead)
    {
        std::printf("(no manual dispatch: waiting %d ms without reading callbacks)\n", ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end)
    {
        if (s->manualDispatch)
            s->runFrame(s->pipe);
        CallbackMsg_t msg;
        while (NextCallback(s, &msg))
        {
            if (msg.m_iCallback == GetAuthSessionTicketResponse_t::k_iCallback)
            {
                auto *r = reinterpret_cast<GetAuthSessionTicketResponse_t *>(msg.m_pubParam);
                std::printf("callback GetAuthSessionTicketResponse: ticket %s, result %d\n",
                    r->m_hAuthTicket == ticket ? "ours" : "other", static_cast<int>(r->m_eResult));
            }
            else if (msg.m_iCallback == ValidateAuthTicketResponse_t::k_iCallback)
            {
                auto *r = reinterpret_cast<ValidateAuthTicketResponse_t *>(msg.m_pubParam);
                std::printf("callback ValidateAuthTicketResponse: %s, response %d, owner %s\n",
                    r->m_SteamID == self ? "self" : "other", static_cast<int>(r->m_eAuthSessionResponse),
                    r->m_OwnerSteamID == r->m_SteamID ? "same" : "different");
            }
            else
            {
                std::printf("callback id %d (%d bytes)\n", msg.m_iCallback, msg.m_cubParam);
            }
            s->freeCallback(s->pipe);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int Run(Session *s)
{
    ISteamUser *user = s->client->GetISteamUser(s->user, s->pipe, STEAMUSER_INTERFACE_VERSION);
    ISteamFriends *friends = s->client->GetISteamFriends(s->user, s->pipe, STEAMFRIENDS_INTERFACE_VERSION);
    ISteamUtils *utils = s->client->GetISteamUtils(s->pipe, STEAMUTILS_INTERFACE_VERSION);
    ISteamApps *apps = s->client->GetISteamApps(s->user, s->pipe, STEAMAPPS_INTERFACE_VERSION);
    std::printf("interfaces: user %s friends %s utils %s apps %s\n", user ? "ok" : "null",
        friends ? "ok" : "null", utils ? "ok" : "null", apps ? "ok" : "null");
    if (!user)
        return 3;

    const bool loggedOn = user->BLoggedOn();
    const CSteamID id = user->GetSteamID();
    const uint64 id64 = id.ConvertToUint64();
    std::printf("BLoggedOn %d, SteamID valid %d, individual %d, ends ...%04u\n", loggedOn,
        id.IsValid(), id.BIndividualAccount(), static_cast<unsigned>(id64 % 10000));
    if (friends)
    {
        const char *name = friends->GetPersonaName();
        std::printf("persona name: %zu chars\n", name ? std::strlen(name) : 0);
    }
    if (utils)
        std::printf("GetAppID %u, IP country set %d\n", utils->GetAppID(), utils->GetIPCountry() != nullptr);
    if (apps)
    {
        std::printf("BIsSubscribedApp(7940) %d, BIsSubscribed %d, language %s\n",
            apps->BIsSubscribedApp(kCod4AppId), apps->BIsSubscribed(), apps->GetCurrentGameLanguage());
    }
    if (!loggedOn)
        return 4;

    unsigned char ticket[1024];
    uint32 size = 0;
    SteamNetworkingIdentity serverIdentity;
    serverIdentity.Clear();
    serverIdentity.SetIPv4Addr(0x7F000001, 28960);
    const HAuthTicket handle = user->GetAuthSessionTicket(ticket, sizeof(ticket), &size, &serverIdentity);
    std::printf("GetAuthSessionTicket -> handle %s, %u bytes\n",
        handle != k_HAuthTicketInvalid ? "valid" : "INVALID", size);
    if (handle == k_HAuthTicketInvalid || !size)
        return 5;
    PumpCallbacks(s, 3000, handle, id);

    // What a KisakCOD server does with the ticket: BeginAuthSession on the
    // claimed SteamID, then wait for ValidateAuthTicketResponse.
    const EBeginAuthSessionResult begin = user->BeginAuthSession(ticket, static_cast<int>(size), id);
    std::printf("BeginAuthSession(own ticket) -> %d\n", static_cast<int>(begin));
    PumpCallbacks(s, 5000, handle, id);
    if (begin == k_EBeginAuthSessionResultOK)
        user->EndAuthSession(id);
    user->CancelAuthTicket(handle);
    std::printf("ticket cancelled\n");
    return 0;
}

// InitGameServer is protected in the SDK headers (steam_api calls it from
// SteamGameServer_Init). A derived class may name it; the pointer to member is
// ISteamGameServer's own, so calling through it needs no downcast.
struct GameServerAccess : ISteamGameServer
{
    static auto InitGameServerMember() { return &GameServerAccess::InitGameServer; }
};

const char *CallbackName(int id)
{
    switch (id)
    {
    case SteamServersConnected_t::k_iCallback: return "SteamServersConnected";
    case SteamServerConnectFailure_t::k_iCallback: return "SteamServerConnectFailure";
    case SteamServersDisconnected_t::k_iCallback: return "SteamServersDisconnected";
    case ValidateAuthTicketResponse_t::k_iCallback: return "ValidateAuthTicketResponse";
    case GSPolicyResponse_t::k_iCallback: return "GSPolicyResponse";
    default: return nullptr;
    }
}

// Reads game-server callbacks for up to ms milliseconds; returns early once the
// logon outcome is known. Sets *connected to 1 (connected), 0 (failed, no
// retry) or leaves -1 (still unknown).
void PumpServerCallbacks(Session *s, int ms, int *connected)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end)
    {
        CallbackMsg_t msg;
        int32 call = 0;
        while (s->clientNextCallback(s->pipe, &msg, &call))
        {
            const char *name = CallbackName(msg.m_iCallback);
            if (msg.m_iCallback == SteamServerConnectFailure_t::k_iCallback &&
                msg.m_cubParam >= static_cast<int>(sizeof(SteamServerConnectFailure_t)))
            {
                SteamServerConnectFailure_t r;
                std::memcpy(&r, msg.m_pubParam, sizeof(r));
                std::printf("callback %s: result %d, still retrying %d\n", name,
                    static_cast<int>(r.m_eResult), r.m_bStillRetrying);
                if (!r.m_bStillRetrying)
                    *connected = 0;
            }
            else if (msg.m_iCallback == SteamServersDisconnected_t::k_iCallback &&
                msg.m_cubParam >= static_cast<int>(sizeof(SteamServersDisconnected_t)))
            {
                SteamServersDisconnected_t r;
                std::memcpy(&r, msg.m_pubParam, sizeof(r));
                std::printf("callback %s: result %d\n", name, static_cast<int>(r.m_eResult));
            }
            else if (msg.m_iCallback == ValidateAuthTicketResponse_t::k_iCallback &&
                msg.m_cubParam >= static_cast<int>(sizeof(ValidateAuthTicketResponse_t)))
            {
                ValidateAuthTicketResponse_t r;
                std::memcpy(&r, msg.m_pubParam, sizeof(r));
                std::printf("callback %s: response %d\n", name, static_cast<int>(r.m_eAuthSessionResponse));
            }
            else if (name)
            {
                std::printf("callback %s\n", name);
                if (msg.m_iCallback == SteamServersConnected_t::k_iCallback)
                    *connected = 1;
            }
            else
            {
                std::printf("callback id %d (%d bytes)\n", msg.m_iCallback, msg.m_cubParam);
            }
            s->freeCallback(s->pipe);
        }
        if (*connected >= 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int RunGameServer(const char *path)
{
    Session s;
    s.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s.lib)
    {
        std::printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto create = reinterpret_cast<CreateInterfaceRetFn>(Sym(s.lib, "CreateInterface"));
    s.clientNextCallback = reinterpret_cast<ClientNextCallbackFn>(Sym(s.lib, "Steam_BGetCallback"));
    s.freeCallback = reinterpret_cast<PipeFn>(Sym(s.lib, "Steam_FreeLastCallback"));
    if (!create || !s.clientNextCallback || !s.freeCallback)
        return 1;
    int rc = 0;
    s.client = static_cast<ISteamClient *>(create(STEAMCLIENT_INTERFACE_VERSION, &rc));
    std::printf("CreateInterface(%s) -> %s rc %d\n", STEAMCLIENT_INTERFACE_VERSION,
        s.client ? "ok" : "null", rc);
    if (!s.client)
        return 1;

    // A game-server user gets its own pipe; this does not touch the desktop
    // Steam client's logged-in user.
    s.user = s.client->CreateLocalUser(&s.pipe, k_EAccountTypeGameServer);
    std::printf("CreateLocalUser(game server) -> pipe %d user %d\n", s.pipe, s.user);
    if (!s.pipe || !s.user)
        return 1;
    ISteamGameServer *server = s.client->GetISteamGameServer(s.user, s.pipe, STEAMGAMESERVER_INTERFACE_VERSION);
    std::printf("GetISteamGameServer(%s) -> %s\n", STEAMGAMESERVER_INTERFACE_VERSION, server ? "ok" : "null");
    int result = 1;
    if (server)
    {
        // Same arguments SteamGameServer_Init would pass for a dedicated,
        // non-VAC server on a probe port (nothing listens on it here).
        constexpr uint32 kFlagDedicated = 4;
        constexpr uint16 kQueryPortShared = 0xffff; // STEAMGAMESERVER_QUERY_PORT_SHARED
        const auto initGameServer = GameServerAccess::InitGameServerMember();
        const bool init = (server->*initGameServer)(0, 28999, kQueryPortShared, kFlagDedicated, kCod4AppId,
            "1.7.0.0");
        std::printf("InitGameServer(app 7940) -> %d\n", init);
        if (init)
        {
            server->SetProduct("cod4");
            server->SetGameDescription("Call of Duty 4");
            server->SetModDir("main");
            server->SetDedicatedServer(true);
            server->LogOnAnonymous();
            std::printf("LogOnAnonymous called\n");
            int connected = -1;
            PumpServerCallbacks(&s, 20000, &connected);
            const CSteamID id = server->GetSteamID();
            std::printf("connected %d, BLoggedOn %d, BSecure %d, SteamID valid %d, game server %d, anonymous %d\n",
                connected, server->BLoggedOn(), server->BSecure(), id.IsValid(), id.BGameServerAccount(),
                id.BAnonGameServerAccount());
            if (server->BLoggedOn())
            {
                // A ticket that cannot be valid: BeginAuthSession must reject
                // it straight away (k_EBeginAuthSessionResultInvalidTicket = 1).
                unsigned char junk[64] = {};
                const CSteamID fake(1u, k_EUniversePublic, k_EAccountTypeIndividual);
                const EBeginAuthSessionResult begin = server->BeginAuthSession(junk, sizeof(junk), fake);
                std::printf("BeginAuthSession(junk ticket) -> %d\n", static_cast<int>(begin));
                if (begin == k_EBeginAuthSessionResultOK)
                    server->EndAuthSession(fake);
                result = 0;
            }
            server->LogOff();
            int ignored = -1;
            PumpServerCallbacks(&s, 2000, &ignored);
        }
    }
    s.client->ReleaseUser(s.pipe, s.user);
    s.client->BReleaseSteamPipe(s.pipe);
    std::printf("BShutdownIfAllPipesClosed -> %d\n", s.client->BShutdownIfAllPipesClosed());
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3 || (std::strcmp(argv[1], "api") && std::strcmp(argv[1], "client") &&
        std::strcmp(argv[1], "gameserver")))
    {
        std::fprintf(stderr, "usage: %s api|client|gameserver <library>\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("probe: %zu-bit, mode %s\n", sizeof(void *) * 8, argv[1]);
    if (!std::strcmp(argv[1], "gameserver"))
    {
        const int rc = RunGameServer(argv[2]);
        std::printf("RESULT: %s (code %d)\n", rc == 0 ? "anonymous game server logged on" : "failed", rc);
        return rc;
    }
    WriteAppIdFile();
    Session s;
    const bool open = !std::strcmp(argv[1], "api") ? OpenApi(argv[2], &s) : OpenClient(argv[2], &s);
    if (!open)
    {
        std::printf("RESULT: could not connect to Steam\n");
        return 1;
    }
    const int rc = Run(&s);
    if (!std::strcmp(argv[1], "api") && s.shutdown)
        s.shutdown();
    else if (s.client && s.pipe)
    {
        if (s.user)
            s.client->ReleaseUser(s.pipe, s.user);
        s.client->BReleaseSteamPipe(s.pipe);
    }
    std::printf("RESULT: %s (code %d)\n", rc == 0 ? "ticket issued" : "failed", rc);
    return rc;
}
