// Steam for 64-bit Windows builds, which have no 64-bit Steam library to link
// (deps/steamsdk holds the 32-bit steam_api only). Every call behaves as the
// 32-bit build does when SteamAPI_Init() fails because Steam is not running:
// the client has no ticket to send, and a server rejects every client ticket.
// The 32-bit build keeps the real integration in win_steam.cpp.
#include <universal/q_shared.h>
#include "win_steam.h"

#include <qcommon/qcommon.h>

bool g_steamInitialized = false;

void Steam_Init() {}

// Steam_Init runs before Com_Init, when nothing printed would be seen.
void Steam_PrintStartupMessages()
{
    Com_PrintWarning(CON_CHANNEL_SYSTEM,
        "Steam is unavailable in the 64-bit Windows build; Steam-authenticated servers will reject it, "
        "and a server it hosts rejects Steam tickets.\n");
}

void Steam_Shutdown()
{
    g_steamInitialized = false;
}

void Steam_ShutdownGameServer() {}

bool Steam_UpdateClientAuthTicket(netadr_t)
{
    return false;
}

bool Steam_GetRawClientTicket(unsigned char **, uint32 *)
{
    return false;
}

void Steam_CancelClientTicket() {}

uint64_t Steam_GetClientSteamID64()
{
    return 0;
}

bool Steam_CheckClientTicket(const void *, uint32, uint64_t)
{
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        Com_PrintError(CON_CHANNEL_SYSTEM, "Rejecting client: Steam is unavailable in the 64-bit Windows build.\n");
    }
    return false;
}

void Steam_CheckClients() {}
void Steam_OnClientDropped(uint64_t) {}
void Steam_SV_AddTestCommands() {}
