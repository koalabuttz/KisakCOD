#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <win32/win_steam.h>

// The Windows build uses the Steam client API. A native headless server needs
// the Steam GameServer runtime and its authentication callbacks instead. Until
// that integration is provided, keep ticket verification explicitly unavailable
// rather than accepting unverified Steam IDs or loading a Windows DLL.
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
