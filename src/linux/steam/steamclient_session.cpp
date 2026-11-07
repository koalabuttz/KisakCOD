#include "steamclient_session.h"

#include <steamsdk/steam/steam_api.h>
#include <steamsdk/steam/isteamclient.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace linux_steam
{
namespace
{
// steamclient.so's exports. Steam_BGetCallback has taken a third out-parameter
// (the call handle) in some versions; passing one is harmless where it is not
// read, since the caller pops cdecl arguments.
using CreateInterfaceFn = void *(*)(const char *version, int *returnCode);
using GetCallbackFn = bool (*)(HSteamPipe pipe, void *message, int32 *call);
using FreeCallbackFn = void (*)(HSteamPipe pipe);

ISteamClient *Client(void *p) { return static_cast<ISteamClient *>(p); }
ISteamUser *User(void *p) { return static_cast<ISteamUser *>(p); }
ISteamUtils *Utils(void *p) { return static_cast<ISteamUtils *>(p); }
} // namespace

const char *OpenResultName(OpenResult result)
{
    switch (result)
    {
    case OpenResult::ok: return "ok";
    case OpenResult::noLibraryPath: return "no-library-path";
    case OpenResult::libraryMissing: return "library-missing";
    case OpenResult::symbolMissing: return "symbol-missing";
    case OpenResult::noClientInterface: return "no-client-interface";
    case OpenResult::noPipe: return "steam-not-running";
    case OpenResult::noUser: return "no-logged-in-user";
    case OpenResult::noUserInterface: return "no-user-interface";
    }
    static_assert(kBeginAuthOK == k_EBeginAuthSessionResultOK, "SDK enum changed");
    static_assert(kBeginAuthDuplicateRequest == k_EBeginAuthSessionResultDuplicateRequest, "SDK enum changed");
    static_assert(kAuthResponseOK == k_EAuthSessionResponseOK, "SDK enum changed");
    return "invalid-result";
}

bool DefaultLibraryPath(char *buffer, size_t capacity)
{
    if (!buffer || !capacity)
        return false;
    buffer[0] = '\0';
    int length = 0;
    const char *override = std::getenv("KISAK_STEAMCLIENT_LIBRARY");
    if (override && *override)
    {
        length = std::snprintf(buffer, capacity, "%s", override);
    }
    else
    {
        const char *home = std::getenv("HOME");
        if (!home || !*home)
            return false;
        length = std::snprintf(buffer, capacity, "%s/.steam/sdk%d/steamclient.so", home,
            static_cast<int>(sizeof(void *) * 8));
    }
    if (length < 0 || static_cast<size_t>(length) >= capacity)
    {
        buffer[0] = '\0';
        return false;
    }
    return true;
}

Session::~Session()
{
    // No calls into steamclient.so here: a session that is still open at
    // process exit is destroyed by an atexit handler, after the library's own
    // exit-time teardown, and releasing the pipe then crashes. Close() it
    // before exit instead; the Steam client drops the pipe when the process
    // ends either way.
}

OpenResult Session::Open(const char *libraryPath, uint32_t appId)
{
    Close();
    loaderError_[0] = '\0';
    if (!libraryPath || !*libraryPath)
        return OpenResult::noLibraryPath;

    // What libsteam_api.so does with steam_appid.txt: the Steam client reads
    // the app id from the environment of the process that connects.
    char id[16];
    std::snprintf(id, sizeof(id), "%u", appId);
    setenv("SteamAppId", id, 1);
    setenv("SteamGameId", id, 1);

    library_ = dlopen(libraryPath, RTLD_NOW | RTLD_LOCAL);
    if (!library_)
    {
        const char *error = dlerror();
        std::snprintf(loaderError_, sizeof(loaderError_), "%s", error ? error : "unknown dlopen error");
        return OpenResult::libraryMissing;
    }
    auto create = reinterpret_cast<CreateInterfaceFn>(dlsym(library_, "CreateInterface"));
    auto getCallback = reinterpret_cast<GetCallbackFn>(dlsym(library_, "Steam_BGetCallback"));
    auto freeCallback = reinterpret_cast<FreeCallbackFn>(dlsym(library_, "Steam_FreeLastCallback"));
    if (!create || !getCallback || !freeCallback)
    {
        Close();
        return OpenResult::symbolMissing;
    }
    int returnCode = 0;
    ISteamClient *client = static_cast<ISteamClient *>(create(STEAMCLIENT_INTERFACE_VERSION, &returnCode));
    if (!client)
    {
        Close();
        return OpenResult::noClientInterface;
    }
    client_ = client;
    pipe_ = client->CreateSteamPipe();
    if (!pipe_)
    {
        Close();
        return OpenResult::noPipe;
    }
    steamUser_ = client->ConnectToGlobalUser(pipe_);
    if (!steamUser_)
    {
        Close();
        return OpenResult::noUser;
    }
    ISteamUser *user = client->GetISteamUser(steamUser_, pipe_, STEAMUSER_INTERFACE_VERSION);
    if (!user)
    {
        Close();
        return OpenResult::noUserInterface;
    }
    user_ = user;
    utils_ = client->GetISteamUtils(pipe_, STEAMUTILS_INTERFACE_VERSION);
    getCallback_ = reinterpret_cast<bool (*)(int32_t, void *, int32_t *)>(getCallback);
    freeCallback_ = freeCallback;
    return OpenResult::ok;
}

void Session::Close()
{
    if (client_ && pipe_)
    {
        if (steamUser_)
            Client(client_)->ReleaseUser(pipe_, steamUser_);
        Client(client_)->BReleaseSteamPipe(pipe_);
    }
    // The library stays loaded: steamclient.so keeps threads of its own, and
    // unloading it under them is not safe.
    library_ = nullptr;
    client_ = nullptr;
    user_ = nullptr;
    utils_ = nullptr;
    pipe_ = 0;
    steamUser_ = 0;
    getCallback_ = nullptr;
    freeCallback_ = nullptr;
}

bool Session::LoggedOn() const
{
    return user_ && User(user_)->BLoggedOn();
}

uint64_t Session::SteamId() const
{
    return user_ ? User(user_)->GetSteamID().ConvertToUint64() : 0;
}

uint32_t Session::AppId() const
{
    return utils_ ? Utils(utils_)->GetAppID() : 0;
}

uint32_t Session::RequestTicket(uint32_t ipv4, uint16_t port, unsigned char *buffer, uint32_t capacity, uint32_t *size)
{
    if (size)
        *size = 0;
    if (!user_ || !buffer || !capacity || !size || capacity > 0x7FFFFFFF)
        return k_HAuthTicketInvalid;
    SteamNetworkingIdentity server;
    server.Clear();
    server.SetIPv4Addr(ipv4, port);
    uint32 written = 0;
    const HAuthTicket handle = User(user_)->GetAuthSessionTicket(buffer, static_cast<int>(capacity), &written, &server);
    if (handle == k_HAuthTicketInvalid || !written || written > capacity)
    {
        if (handle != k_HAuthTicketInvalid)
            User(user_)->CancelAuthTicket(handle);
        return k_HAuthTicketInvalid;
    }
    *size = written;
    return handle;
}

void Session::CancelTicket(uint32_t handle)
{
    if (user_ && handle != k_HAuthTicketInvalid)
        User(user_)->CancelAuthTicket(handle);
}

int Session::BeginAuth(const void *ticket, uint32_t size, uint64_t steamId)
{
    if (!user_)
        return -1;
    if (!ticket || !size || size > 0x7FFFFFFF || !steamId)
        return k_EBeginAuthSessionResultInvalidTicket;
    CSteamID id;
    id.SetFromUint64(steamId);
    return User(user_)->BeginAuthSession(ticket, static_cast<int>(size), id);
}

void Session::EndAuth(uint64_t steamId)
{
    if (!user_ || !steamId)
        return;
    CSteamID id;
    id.SetFromUint64(steamId);
    User(user_)->EndAuthSession(id);
}

int Session::Pump(void (*onVerdict)(const AuthVerdict &verdict, void *context), void *context)
{
    if (!user_)
        return 0;
    int count = 0;
    CallbackMsg_t message;
    int32 call = 0;
    while (getCallback_(pipe_, &message, &call))
    {
        ++count;
        if (message.m_iCallback == ValidateAuthTicketResponse_t::k_iCallback && message.m_pubParam &&
            message.m_cubParam >= static_cast<int>(sizeof(ValidateAuthTicketResponse_t)))
        {
            ValidateAuthTicketResponse_t response;
            std::memcpy(&response, message.m_pubParam, sizeof(response));
            const AuthVerdict verdict = {response.m_SteamID.ConvertToUint64(),
                response.m_OwnerSteamID.ConvertToUint64(), static_cast<int>(response.m_eAuthSessionResponse)};
            if (onVerdict)
                onVerdict(verdict, context);
        }
        freeCallback_(pipe_);
    }
    return count;
}
} // namespace linux_steam
