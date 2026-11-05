#pragma once

// A session with the Steam client that is already running on this machine,
// through the steamclient.so every Steam install ships (~/.steam/sdk32 for
// i386, ~/.steam/sdk64 for x86_64). It does what src/win32/win_steam.cpp does
// with steam_api.dll: log in as an app, issue auth session tickets, and check
// tickets other players send with BeginAuthSession.
//
// Nothing links against Steam. The library is dlopen()ed when the session
// opens, so a build needs only the SDK headers in deps/steamsdk, and a machine
// without Steam simply fails to open a session.
//
// This header and its unit know nothing about the engine, so the asset-free
// test can drive them against a stand-in steamclient.so.

#include <cstddef>
#include <cstdint>

namespace linux_steam
{
enum class OpenResult
{
    ok,
    noLibraryPath,     // no override and no HOME to find ~/.steam from
    libraryMissing,    // dlopen failed
    symbolMissing,     // CreateInterface, Steam_BGetCallback or Steam_FreeLastCallback
    noClientInterface, // the library does not serve this SDK's ISteamClient
    noPipe,            // the Steam client is not running
    noUser,            // no logged-in Steam user to connect to
    noUserInterface,   // the library does not serve this SDK's ISteamUser
};

const char *OpenResultName(OpenResult result);

// The SDK's k_EBeginAuthSessionResultOK and k_EAuthSessionResponseOK, for
// callers that do not include the SDK headers.
constexpr int kBeginAuthOK = 0;
constexpr int kAuthResponseOK = 0;

// One ValidateAuthTicketResponse_t: Steam's verdict on a ticket passed to
// BeginAuthSession. response is an EAuthSessionResponse; 0 is OK.
struct AuthVerdict
{
    uint64_t steamId;
    uint64_t ownerSteamId;
    int response;
};

// The library a session opens: $KISAK_STEAMCLIENT_LIBRARY when set, otherwise
// $HOME/.steam/sdk32/steamclient.so (sdk64 on x86_64). False when neither
// gives a path or it does not fit.
bool DefaultLibraryPath(char *buffer, size_t capacity);

class Session
{
public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session();

    // Sets SteamAppId/SteamGameId to appId for this process, loads the
    // library and connects to the logged-in user. On failure nothing stays
    // open; LoaderError() has dlerror()'s text for libraryMissing.
    OpenResult Open(const char *libraryPath, uint32_t appId);
    void Close();
    bool IsOpen() const { return user_ != nullptr; }
    const char *LoaderError() const { return loaderError_; }

    bool LoggedOn() const;
    uint64_t SteamId() const;
    // The app id Steam says this process runs as, or 0 if unknown.
    uint32_t AppId() const;

    // Issues a ticket bound to the server at ipv4 (host byte order) and port.
    // Returns the ticket handle, or 0 on failure (size is then 0).
    uint32_t RequestTicket(uint32_t ipv4, uint16_t port, unsigned char *buffer, uint32_t capacity, uint32_t *size);
    void CancelTicket(uint32_t handle);

    // An EBeginAuthSessionResult (0 is OK), or -1 when the session is closed.
    int BeginAuth(const void *ticket, uint32_t size, uint64_t steamId);
    void EndAuth(uint64_t steamId);

    // Reads every pending callback. Each ValidateAuthTicketResponse_t is passed
    // to onVerdict; the rest are released unread. Returns how many were read.
    int Pump(void (*onVerdict)(const AuthVerdict &verdict, void *context), void *context);

private:
    void *library_ = nullptr;
    void *client_ = nullptr; // ISteamClient
    void *user_ = nullptr;   // ISteamUser
    void *utils_ = nullptr;  // ISteamUtils, optional
    int32_t pipe_ = 0;
    int32_t steamUser_ = 0;
    bool (*getCallback_)(int32_t, void *, int32_t *) = nullptr;
    void (*freeCallback_)(int32_t) = nullptr;
    char loaderError_[256] = {};
};
} // namespace linux_steam
