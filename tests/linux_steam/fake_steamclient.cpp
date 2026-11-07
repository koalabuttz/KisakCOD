// A stand-in for Steam's steamclient.so, for the asset-free test of
// src/linux/steam/steamclient_session.cpp. It exports what the session loads
// (CreateInterface, Steam_BGetCallback, Steam_FreeLastCallback) and serves this
// SDK's ISteamClient and ISteamUser. The test drives it through
// FakeSteam_Control(): what each call answers, and a record of what was asked.
// No Steam install is looked for or touched.

#include "fake_steamclient.h"

#include <steamsdk/steam/steam_api.h>
#include <steamsdk/steam/isteamclient.h>

#include <cstdlib>
#include <cstring>

namespace
{
FakeSteamControl g_control;

class FakeUser final : public ISteamUser
{
public:
    HSteamUser GetHSteamUser() override { return 1; }
    bool BLoggedOn() override { return g_control.loggedOn; }
    CSteamID GetSteamID() override
    {
        CSteamID id;
        id.SetFromUint64(g_control.steamId);
        return id;
    }
    int InitiateGameConnection_DEPRECATED(void *, int, CSteamID, uint32, uint16, bool) override { return 0; }
    void TerminateGameConnection_DEPRECATED(uint32, uint16) override {}
    void TrackAppUsageEvent(CGameID, int, const char *) override {}
    bool GetUserDataFolder(char *, int) override { return false; }
    void StartVoiceRecording() override {}
    void StopVoiceRecording() override {}
    EVoiceResult GetAvailableVoice(uint32 *, uint32 *, uint32) override { return k_EVoiceResultNotInitialized; }
    EVoiceResult GetVoice(bool, void *, uint32, uint32 *, bool, void *, uint32, uint32 *, uint32) override
    {
        return k_EVoiceResultNotInitialized;
    }
    EVoiceResult DecompressVoice(const void *, uint32, void *, uint32, uint32 *, uint32) override
    {
        return k_EVoiceResultNotInitialized;
    }
    uint32 GetVoiceOptimalSampleRate() override { return 0; }
    HAuthTicket GetAuthSessionTicket(void *ticket, int maxTicket, uint32 *written,
        const SteamNetworkingIdentity *identity) override
    {
        ++g_control.ticketRequests;
        g_control.identityIsIp = identity && identity->m_eType == k_ESteamNetworkingIdentityType_IPAddress;
        g_control.identityIPv4 = identity ? identity->m_ip.GetIPv4() : 0;
        g_control.identityPort = identity ? identity->m_ip.m_port : 0;
        g_control.ticketCapacity = maxTicket;
        const uint32 size = g_control.ticketSize;
        if (size && maxTicket > 0 && size <= static_cast<uint32>(maxTicket))
            std::memcpy(ticket, g_control.ticket, size);
        // reportedTicketSize lets a test make the library claim more than it wrote.
        *written = g_control.reportedTicketSize ? g_control.reportedTicketSize : size;
        return g_control.ticketHandle;
    }
    HAuthTicket GetAuthTicketForWebApi(const char *) override { return k_HAuthTicketInvalid; }
    EBeginAuthSessionResult BeginAuthSession(const void *ticket, int size, CSteamID id) override
    {
        ++g_control.beginCalls;
        g_control.beginSteamId = id.ConvertToUint64();
        g_control.beginSize = size;
        g_control.beginTicketMatches = size == static_cast<int>(g_control.ticketSize) &&
            !std::memcmp(ticket, g_control.ticket, g_control.ticketSize);
        return static_cast<EBeginAuthSessionResult>(g_control.beginResult);
    }
    void EndAuthSession(CSteamID id) override
    {
        ++g_control.endCalls;
        g_control.endSteamId = id.ConvertToUint64();
    }
    void CancelAuthTicket(HAuthTicket handle) override
    {
        ++g_control.cancelCalls;
        g_control.cancelledHandle = handle;
    }
    EUserHasLicenseForAppResult UserHasLicenseForApp(CSteamID, AppId_t) override
    {
        return k_EUserHasLicenseResultDoesNotHaveLicense;
    }
    bool BIsBehindNAT() override { return false; }
    void AdvertiseGame(CSteamID, uint32, uint16) override {}
    SteamAPICall_t RequestEncryptedAppTicket(void *, int) override { return k_uAPICallInvalid; }
    bool GetEncryptedAppTicket(void *, int, uint32 *) override { return false; }
    int GetGameBadgeLevel(int, bool) override { return 0; }
    int GetPlayerSteamLevel() override { return 0; }
    SteamAPICall_t RequestStoreAuthURL(const char *) override { return k_uAPICallInvalid; }
    bool BIsPhoneVerified() override { return false; }
    bool BIsTwoFactorEnabled() override { return false; }
    bool BIsPhoneIdentifying() override { return false; }
    bool BIsPhoneRequiringVerification() override { return false; }
    SteamAPICall_t GetMarketEligibility() override { return k_uAPICallInvalid; }
    SteamAPICall_t GetDurationControl() override { return k_uAPICallInvalid; }
    bool BSetDurationControlOnlineState(EDurationControlOnlineState) override { return false; }
};

FakeUser g_user;

class FakeClient final : public ISteamClient
{
public:
    HSteamPipe CreateSteamPipe() override
    {
        const char *appId = std::getenv("SteamAppId");
        const char *gameId = std::getenv("SteamGameId");
        g_control.appIdEnvAtPipe = appId ? static_cast<uint32_t>(std::strtoul(appId, nullptr, 10)) : 0;
        g_control.gameIdEnvAtPipe = gameId ? static_cast<uint32_t>(std::strtoul(gameId, nullptr, 10)) : 0;
        ++g_control.pipesCreated;
        return g_control.pipe;
    }
    bool BReleaseSteamPipe(HSteamPipe pipe) override
    {
        ++g_control.pipesReleased;
        g_control.releasedPipe = pipe;
        return true;
    }
    HSteamUser ConnectToGlobalUser(HSteamPipe pipe) override
    {
        g_control.connectPipe = pipe;
        return g_control.user;
    }
    HSteamUser CreateLocalUser(HSteamPipe *, EAccountType) override { return 0; }
    void ReleaseUser(HSteamPipe pipe, HSteamUser user) override
    {
        ++g_control.usersReleased;
        g_control.releasedUser = user;
        g_control.releasedUserPipe = pipe;
    }
    ISteamUser *GetISteamUser(HSteamUser user, HSteamPipe pipe, const char *version) override
    {
        g_control.userVersionMatches = !std::strcmp(version, STEAMUSER_INTERFACE_VERSION);
        g_control.userInterfaceHandlesMatch = user == g_control.user && pipe == g_control.pipe;
        return g_control.serveUser ? &g_user : nullptr;
    }
    ISteamGameServer *GetISteamGameServer(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    void SetLocalIPBinding(const SteamIPAddress_t &, uint16) override {}
    ISteamFriends *GetISteamFriends(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamUtils *GetISteamUtils(HSteamPipe, const char *) override { return nullptr; }
    ISteamMatchmaking *GetISteamMatchmaking(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamMatchmakingServers *GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char *) override
    {
        return nullptr;
    }
    void *GetISteamGenericInterface(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamUserStats *GetISteamUserStats(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamGameServerStats *GetISteamGameServerStats(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamApps *GetISteamApps(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamNetworking *GetISteamNetworking(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamRemoteStorage *GetISteamRemoteStorage(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamScreenshots *GetISteamScreenshots(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamGameSearch *GetISteamGameSearch(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    void RunFrame() override {}
    uint32 GetIPCCallCount() override { return 0; }
    void SetWarningMessageHook(SteamAPIWarningMessageHook_t) override {}
    bool BShutdownIfAllPipesClosed() override { return false; }
    ISteamHTTP *GetISteamHTTP(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamController *GetISteamController(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamUGC *GetISteamUGC(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamMusic *GetISteamMusic(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamMusicRemote *GetISteamMusicRemote(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamHTMLSurface *GetISteamHTMLSurface(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*)()) override {}
    void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*)()) override {}
    void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t) override {}
    ISteamInventory *GetISteamInventory(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamVideo *GetISteamVideo(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamParentalSettings *GetISteamParentalSettings(HSteamUser, HSteamPipe, const char *) override
    {
        return nullptr;
    }
    ISteamInput *GetISteamInput(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamParties *GetISteamParties(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    ISteamRemotePlay *GetISteamRemotePlay(HSteamUser, HSteamPipe, const char *) override { return nullptr; }
    void DestroyAllInterfaces() override {}
};

FakeClient g_client;

// Callback payloads live here while the session reads them, as Steam's do
// until Steam_FreeLastCallback.
unsigned char g_payload[256];
bool g_callbackOut = false;
} // namespace

extern "C" __attribute__((visibility("default"))) FakeSteamControl *FakeSteam_Control()
{
    return &g_control;
}

extern "C" __attribute__((visibility("default"))) void *CreateInterface(const char *version, int *returnCode)
{
    ++g_control.createCalls;
    const bool match = !std::strcmp(version, STEAMCLIENT_INTERFACE_VERSION);
    if (returnCode)
        *returnCode = match && g_control.serveClient ? 0 : 1;
    return match && g_control.serveClient ? static_cast<ISteamClient *>(&g_client) : nullptr;
}

extern "C" __attribute__((visibility("default"))) bool Steam_BGetCallback(HSteamPipe pipe, CallbackMsg_t *message,
    int32 *call)
{
    ++g_control.getCallbackCalls;
    if (pipe != g_control.pipe || g_control.callbackNext >= g_control.callbackCount)
        return false;
    if (g_callbackOut)
        ++g_control.callbackReadWithoutFree;
    const FakeSteamCallback &next = g_control.callbacks[g_control.callbackNext];
    std::memset(g_payload, 0, sizeof(g_payload));
    int size = next.size;
    if (next.id == ValidateAuthTicketResponse_t::k_iCallback)
    {
        ValidateAuthTicketResponse_t response = {};
        response.m_SteamID.SetFromUint64(next.steamId);
        response.m_OwnerSteamID.SetFromUint64(next.ownerSteamId);
        response.m_eAuthSessionResponse = static_cast<EAuthSessionResponse>(next.response);
        std::memcpy(g_payload, &response, sizeof(response));
        if (size < 0)
            size = sizeof(response);
    }
    else if (size < 0)
    {
        size = 12;
    }
    message->m_hSteamUser = g_control.user;
    message->m_iCallback = next.id;
    message->m_pubParam = g_payload;
    message->m_cubParam = size;
    if (call)
        *call = 0;
    g_callbackOut = true;
    return true;
}

extern "C" __attribute__((visibility("default"))) void Steam_FreeLastCallback(HSteamPipe pipe)
{
    ++g_control.freeCallbackCalls;
    if (pipe != g_control.pipe || !g_callbackOut)
    {
        ++g_control.freeWithoutCallback;
        return;
    }
    g_callbackOut = false;
    ++g_control.callbackNext;
}

extern "C" __attribute__((visibility("default"))) int FakeSteam_ValidateCallbackId()
{
    return ValidateAuthTicketResponse_t::k_iCallback;
}
