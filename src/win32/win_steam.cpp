// LWSS: this is a Rudimentary Steam Authentication for the Client -> Dedicated Server
// The Client Generates a Steam ticket and sends it to the Server (in lieu of a CD Key)
// The Server checks this Ticket and then puts in a delayed networked check to Steam.

// You can try to disable this code, but you really shouldn't since Server GUID's are set to the SteamID64
// An added benefit of Steam is that their SteamID can't be changed without them spending another 20$

#include <universal/q_shared.h>
#include "win_steam.h"

#include <windows.h>

#include <qcommon/qcommon.h>

#include <steamsdk/steam/steam_api.h>
#include <steamsdk/steam/isteamgameserver.h>
#include <sys/stat.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <qcommon/cmd.h>
#include <universal/steam_optin.h>

// win_main.cpp: the command line, copied before Steam_Init runs.
extern char sys_cmdline[1024];

#ifdef KISAK_MP
#include <server_mp/server_mp.h>
#include <server_mp/sv_handshake_auth.h>
#endif

#include <cstdarg>
#include <functional>
#include <string>

#ifdef KISAK_DEDICATED
#include <steamsdk/steam/steam_gameserver.h>
#endif

// 7940 = COD4 APPID
#define APPID 7940
#define APPID_STR "7940"

// KISAK: a dedicated server can log on to Steam as an anonymous game server
// (KISAK_STEAM_SERVER_LOGIN=anonymous, see Steam_Init) rather than as the Steam
// client's signed-in user, as the Linux server can (src/linux/steam.cpp). It
// then checks tickets with ISteamGameServer, and nobody's account is in use on
// its machine. Only the dedicated server has this mode.
#ifdef KISAK_DEDICATED
static bool s_gameServer = false;
static bool s_gameServerLogonReported = false;
#else
static const bool s_gameServer = false;
#endif


bool g_steamInitialized = false;
uint64_t g_steamID = 0;
char g_steamName[64]{ 0 };

static HAuthTicket steamClientAuthTicket = 0;
static unsigned char steamClientTicket[1024]{ 0 };
static uint32 steamClientTicketSize = 0;

std::mutex kickPlayerMutex;
std::vector<uint64_t> steamIDsToKick;

uint64_t Steam_GetClientSteamID64()
{
	iassert(g_steamID != 0);
	return g_steamID;
}

bool Steam_GetRawClientTicket(unsigned char **pBuffer, uint32 *pSize)
{
	if (!steamClientTicketSize || !steamClientTicket[0])
	{
		iassert(0);
		return false;
	}

	*pSize = steamClientTicketSize;
	*pBuffer = steamClientTicket;

	return true;
}

void Steam_CancelClientTicket()
{
	if (steamClientAuthTicket && !s_gameServer)
	{
		SteamUser()->CancelAuthTicket(steamClientAuthTicket);
		steamClientAuthTicket = 0;
	}
}

// This seems to require a class... This sucks
class SteamCallbackHandler
{
public:
	SteamCallbackHandler();
	~SteamCallbackHandler() = default;

	STEAM_CALLBACK(SteamCallbackHandler, Callback_BeginAuthResponse, ValidateAuthTicketResponse_t, m_CallbackBeginAuthResponse);

};

SteamCallbackHandler::SteamCallbackHandler()
	: m_CallbackBeginAuthResponse(this, &SteamCallbackHandler::Callback_BeginAuthResponse)
{
}
static SteamCallbackHandler *callbackHandler;


// Steam's verdict on a ticket this process began checking.
static void Steam_OnAuthVerdict(const ValidateAuthTicketResponse_t *pCallback)
{
	uint64_t id64 = pCallback->m_SteamID.ConvertToUint64();

	if (pCallback->m_eAuthSessionResponse != k_EAuthSessionResponseOK)
	{
		// KICK PLAYER FROM SERVER
		{
			std::lock_guard<std::mutex> _(kickPlayerMutex);
			steamIDsToKick.push_back(id64);
		}
	}
	else
	{
		Com_Printf(CON_CHANNEL_DONT_FILTER, "[Kisak] Player %lld passed Steam Auth Check\n", id64);
	}
}

void SteamCallbackHandler::Callback_BeginAuthResponse(ValidateAuthTicketResponse_t *pCallback)
{
	Steam_OnAuthVerdict(pCallback);
}

#ifdef KISAK_DEDICATED
// The same verdicts, delivered on the game server's own connection to Steam
// (SteamGameServer_RunCallbacks), plus what becomes of that connection.
class SteamGameServerCallbackHandler
{
public:
	SteamGameServerCallbackHandler();
	~SteamGameServerCallbackHandler() = default;

	STEAM_GAMESERVER_CALLBACK(SteamGameServerCallbackHandler, Callback_AuthResponse, ValidateAuthTicketResponse_t, m_CallbackAuthResponse);
	STEAM_GAMESERVER_CALLBACK(SteamGameServerCallbackHandler, Callback_ConnectFailure, SteamServerConnectFailure_t, m_CallbackConnectFailure);
	STEAM_GAMESERVER_CALLBACK(SteamGameServerCallbackHandler, Callback_Disconnected, SteamServersDisconnected_t, m_CallbackDisconnected);
};

SteamGameServerCallbackHandler::SteamGameServerCallbackHandler()
	: m_CallbackAuthResponse(this, &SteamGameServerCallbackHandler::Callback_AuthResponse)
	, m_CallbackConnectFailure(this, &SteamGameServerCallbackHandler::Callback_ConnectFailure)
	, m_CallbackDisconnected(this, &SteamGameServerCallbackHandler::Callback_Disconnected)
{
}
static SteamGameServerCallbackHandler *gameServerCallbackHandler;

void SteamGameServerCallbackHandler::Callback_AuthResponse(ValidateAuthTicketResponse_t *pCallback)
{
	Steam_OnAuthVerdict(pCallback);
}

void SteamGameServerCallbackHandler::Callback_ConnectFailure(SteamServerConnectFailure_t *pCallback)
{
	// Reported once per outage; Steam keeps retrying by itself. Until it is
	// logged on, Steam_CheckClientTicket rejects every ticket.
	if (s_gameServerLogonReported || pCallback->m_bStillRetrying)
	{
		return;
	}
	s_gameServerLogonReported = true;
	Com_Printf(CON_CHANNEL_DONT_FILTER,
		"[Kisak] Steam: the anonymous game server could not log on (result %d). Tickets are rejected until it does.\n",
		(int)pCallback->m_eResult);
}

void SteamGameServerCallbackHandler::Callback_Disconnected(SteamServersDisconnected_t *pCallback)
{
	s_gameServerLogonReported = false;
	Com_Printf(CON_CHANNEL_DONT_FILTER,
		"[Kisak] Steam: the anonymous game server lost its connection to Steam (result %d). Tickets are rejected until it is back.\n",
		(int)pCallback->m_eResult);
}

// Prints the logon once it has happened (and again after an outage).
static void Steam_ReportGameServerLogon()
{
	if (s_gameServerLogonReported || !SteamGameServer()->BLoggedOn())
	{
		return;
	}
	s_gameServerLogonReported = true;
	Com_Printf(CON_CHANNEL_DONT_FILTER, "[Kisak] Steam: logged on as an anonymous game server for app %d\n", APPID);
}
#endif

void Steam_CheckClients()
{
#ifdef KISAK_MP
#ifdef KISAK_DEDICATED
	if (s_gameServer)
	{
		SteamGameServer_RunCallbacks();
		Steam_ReportGameServerLogon();
	}
	else
#endif
	{
		SteamAPI_RunCallbacks();
	}

	// KISAK: taken out under the lock and acted on outside it. SV_OnSteamAuthRejected
	// drops the connected players holding the SteamID64 (free and zombie slots keep
	// stale identities and are left alone) and forgets its challenges, so the
	// verdict also holds for a connect that comes after it.
	std::vector<uint64_t> rejected;
	{
		std::lock_guard<std::mutex> _(kickPlayerMutex);
		rejected.swap(steamIDsToKick);
	}

	if (com_sv_running && com_sv_running->current.enabled)
	{
		for (uint64_t steamID64 : rejected)
		{
			SV_OnSteamAuthRejected(steamID64);
		}
	}
#endif
}


// KISAK: Steam_Init runs before Com_Init (win_main.cpp), when Com_Printf has no
// console window or log file to write to, so everything it says was lost: a
// mistyped KISAK_STEAM_SERVER_LOGIN, "logging on as an anonymous game server",
// "SteamAPI_Init() failed". It keeps those lines here, and
// Steam_PrintStartupMessages prints them once Com_Init has run.
static std::string s_startupMessages;

static void Steam_StartupPrintf(const char *format, ...)
{
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	s_startupMessages += line;
	if (s_startupMessages.empty() || s_startupMessages.back() != '\n')
		s_startupMessages += '\n';
}

void Steam_PrintStartupMessages()
{
	if (!s_startupMessages.empty())
		Com_Printf(CON_CHANNEL_DONT_FILTER, "%s", s_startupMessages.c_str());
	s_startupMessages.clear();
	s_startupMessages.shrink_to_fit();
}

// callback hook for debug text emitted from the Steam API
extern "C" void SteamAPIDebugTextHook(int nSeverity, const char *pchDebugText)
{
	Com_Printf(CON_CHANNEL_DONT_FILTER, "%s", pchDebugText);
}

// This Creates a "steam_appid.txt" next to the executable for the Steam API to read
static void CreateAppIdFile()
{
	// Check if File Exists Already
	struct stat fileinfo;
	if (stat("steam_appid.txt", &fileinfo) == 0)
	{
		return;
	}

	FILE *fp = fopen("steam_appid.txt", "w");
	if (!fp)
	{
		Steam_StartupPrintf("[Kisak] Error opening 'steam_appid.txt'.");
		return;
	}

	fprintf(fp, "%d", APPID);
	fclose(fp);
}

#ifdef KISAK_DEDICATED
// KISAK_STEAM_SERVER_LOGIN=anonymous. It is an environment variable because
// Steam starts before the command line runs (as on Linux). Any other value
// turns the backend off: a mistyped setting must not fall back to using the
// machine's Steam account.
static void Steam_InitGameServer(const char *login)
{
	if (strcmp(login, "anonymous"))
	{
		Steam_StartupPrintf(
			"[Kisak] Steam: KISAK_STEAM_SERVER_LOGIN must be \"anonymous\" or unset, not \"%s\". Steam authentication is off.\n",
			login);
		return;
	}

	// The game port only describes the server to Steam. With the query port
	// shared and nothing passing Steam's queries on, and with advertising left
	// off (SetAdvertiseServerActive is never called), the server is not listed.
	SteamErrMsg error = {};
	if (SteamGameServer_InitEx(0, 28960, STEAMGAMESERVER_QUERY_PORT_SHARED, eServerModeAuthentication, "1.7.0.0", &error)
		!= k_ESteamAPIInitResult_OK)
	{
		Steam_StartupPrintf(
			"[Kisak] Steam: could not start an anonymous game server (%s). Is Steam installed? Steam authentication is off.\n",
			error);
		return;
	}

	SteamGameServer()->SetProduct("cod4");
	SteamGameServer()->SetGameDescription("Call of Duty 4");
	SteamGameServer()->SetModDir("main");
	SteamGameServer()->SetDedicatedServer(true);
	SteamGameServer()->LogOnAnonymous();

	gameServerCallbackHandler = new SteamGameServerCallbackHandler();
	s_gameServerLogonReported = false;
	s_gameServer = true;
	g_steamInitialized = true;
	Steam_StartupPrintf("[Kisak] Steam: logging on as an anonymous game server for app %d\n", APPID);
}
#endif

void Steam_Init()
{
#if defined(KISAK_MP) && !defined(KISAK_DEDICATED)
	// KISAK: Steam is opt-in for the client (src/universal/steam_optin.h).
	// Without +set cl_steam 1 nothing here runs: no steam_appid.txt, no
	// SteamAPI_Init, and the player joins with a key code.
	if (!Steam_ClientOptedIn(sys_cmdline))
	{
		Steam_StartupPrintf("[Kisak] Steam is off, so remote servers are joined with your key code. "
			"Start the game with +set cl_steam 1 to join with Steam.\n");
		return;
	}
#endif
	CreateAppIdFile();
	SetEnvironmentVariableA("SteamAppId", APPID_STR);
	SetEnvironmentVariableA("SteamGameId", APPID_STR);
	SetEnvironmentVariableA("SteamOverlayGameId", APPID_STR);

#ifdef KISAK_DEDICATED
	char serverLogin[32] = {};
	const DWORD serverLoginLength = GetEnvironmentVariableA("KISAK_STEAM_SERVER_LOGIN", serverLogin, sizeof(serverLogin));
	if (serverLoginLength)
	{
		// A value too long for the buffer is not "anonymous" either.
		Steam_InitGameServer(serverLoginLength < sizeof(serverLogin) ? serverLogin : "(too long)");
		return;
	}
#endif

	if (!SteamAPI_Init())
	{
		Steam_StartupPrintf("[Kisak] SteamAPI_Init() failed. Is the Steam Client Running? Do you own COD4?");
		return;
	}

	if (!SteamUser()->BLoggedOn())
	{
		Steam_StartupPrintf("[Kisak] Steam User isn't Logged On.");
		return;
	}

	g_steamInitialized = true;

	SteamClient()->SetWarningMessageHook(&SteamAPIDebugTextHook);

	I_strncpyz(g_steamName, SteamFriends()->GetPersonaName(), sizeof(g_steamName));
	g_steamID = SteamUser()->GetSteamID().ConvertToUint64();

	callbackHandler = new SteamCallbackHandler();
}

// KISAK: Sys_Quit calls this. It logs a dedicated server's anonymous game
// server off Steam, and does nothing in the Steam user's mode.
void Steam_ShutdownGameServer()
{
#ifdef KISAK_DEDICATED
	if (!s_gameServer)
	{
		return;
	}
	g_steamInitialized = false;
	s_gameServer = false;
	delete gameServerCallbackHandler;
	gameServerCallbackHandler = nullptr;
	SteamGameServer()->LogOff();
	SteamGameServer_Shutdown();
#endif
}

// (Currently Unused)
void Steam_Shutdown()
{
	SteamAPI_Shutdown();

	delete callbackHandler;

	g_steamInitialized = false;
	memset(g_steamName, 0, 64);
	g_steamID = 0;
}

bool Steam_UpdateClientAuthTicket(netadr_t serverIpv4)
{
	// A game server has no Steam user to ask for a ticket.
	if (!g_steamInitialized || s_gameServer)
	{
		return false;
	}

	SteamNetworkingIdentity snid;
	uint32 ip32 = 0;
	ip32 |= (serverIpv4.ip[0] << 24);
	ip32 |= (serverIpv4.ip[1] << 16);
	ip32 |= (serverIpv4.ip[2] << 8);
	ip32 |= (serverIpv4.ip[3]);
	snid.SetIPv4Addr(ip32, serverIpv4.port);

	steamClientAuthTicket = SteamUser()->GetAuthSessionTicket(steamClientTicket, sizeof(steamClientTicket), &steamClientTicketSize, &snid);

	return steamClientAuthTicket != k_HAuthTicketInvalid;
}

bool Steam_CheckClientTicket(const void* pAuthTicket, uint32 authTicketLen, uint64_t steamID64)
{
	iassert(steamID64);

	if (!g_steamInitialized)
	{
		Com_Printf(CON_CHANNEL_DONT_FILTER, "[Kisak] Steam_CheckClientTicket() - Steam is not Initialized! Returning False\n");
		return false;
	}

	CSteamID id;
	id.SetFromUint64(steamID64);

	EBeginAuthSessionResult res;
#ifdef KISAK_DEDICATED
	if (s_gameServer)
	{
		// No verdict can arrive while the game server is not logged on.
		if (!SteamGameServer()->BLoggedOn())
		{
			return false;
		}
		res = SteamGameServer()->BeginAuthSession(pAuthTicket, authTicketLen, id);
	}
	else
#endif
	{
		res = SteamUser()->BeginAuthSession(pAuthTicket, authTicketLen, id);
	}

	if (res != k_EBeginAuthSessionResultOK)
	{
		return false;
	}

	// Good for Now, the callback will kick him later if needed.
	return true;
}

void Steam_OnClientDropped(uint64_t steamID64)
{
	iassert(steamID64);

	// KISAK: no session is open once Steam is off (after Steam_ShutdownGameServer).
	if (!g_steamInitialized)
	{
		return;
	}

	CSteamID sid;
	sid.SetFromUint64(steamID64);
#ifdef KISAK_DEDICATED
	if (s_gameServer)
	{
		SteamGameServer()->EndAuthSession(sid);
		return;
	}
#endif
	SteamUser()->EndAuthSession(sid);
}

static void Steam_SV_TestKick()
{
	if (!com_sv_running->current.enabled)
	{
		Com_Printf(CON_CHANNEL_DONT_FILTER, "Server is not running.\n");
		return;
	}
	if (SV_Cmd_Argc() != 2)
	{
		const char *cmdName = SV_Cmd_Argv(0);
		Com_Printf(CON_CHANNEL_DONT_FILTER, "Usage: %s <SteamID64>\n", cmdName);
		return;
	}

	const char *arg1 = SV_Cmd_Argv(1);
	char *endptr;
	uint64_t steamID64 = strtoull(arg1, &endptr, 10);
	{
		std::lock_guard<std::mutex> _(kickPlayerMutex);
		steamIDsToKick.push_back(steamID64);
	}
}

cmd_function_s Steam_SV_TestSteamKick_SERVER;
cmd_function_s Steam_SV_TestSteamKick;
void Steam_SV_AddTestCommands()
{
	Cmd_AddCommandInternal("steam_testkick", Cbuf_AddServerText_f, &Steam_SV_TestSteamKick);
	Cmd_AddServerCommandInternal("steam_testkick", Steam_SV_TestKick, &Steam_SV_TestSteamKick_SERVER);
}
