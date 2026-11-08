// win_local.h: Win32-specific Quake3 header file
#pragma once // addition

#if defined (_MSC_VER) && (_MSC_VER >= 1200)
#pragma warning(disable : 4201)
#pragma warning( push )
#endif
//#include <windows.h>
//#include "../qcommon/platform.h"
#if defined (_MSC_VER) && (_MSC_VER >= 1200)
#pragma warning( pop )
#endif

#ifndef _XBOX
#define DIRECTINPUT_VERSION 0x0800  //[ 0x0300 | 0x0500 | 0x0700 | 0x0800 ]
#if defined(__linux__) || defined(__APPLE__) || defined(__vita__)
// Native Linux has no Windows SDK: dinput/winsock/wsipx are DirectInput and
// WinSock 1.x headers. The dedicated server uses the engine's own input and net
// entry points (Sys_*, NET_*, IN_*), which the POSIX backend implements against
// the platform, so only the Win32 base types are needed here. See
// src/linux/compat.h; input/net device code stays outside this header.
#include <linux/compat.h>
#include <mutex> // the non-Windows s_criticalSections branch below is std::mutex
#else
#include <dinput.h>
//#include <dsound.h>
#include <winsock.h>
#include <wsipx.h>
#endif
#endif
#include <qcommon/qcommon.h>
#ifdef KISAK_MP
#include <qcommon/net_chan_mp.h>
#elif KISAK_SP
#include <qcommon/net_chan.h>
#elif defined(KISAK_RADIANT)
// ─────────────────────────────────────────────────────────────────────────────
// The Radiant tools build cannot include qcommon/net_chan.h or qcommon/msg.h —
// both #error on anything but KISAK_SP. These local defs stand in, and their
// layouts are pinned to the real engine structs with static_asserts (below) so
// they can never silently drift from what compiled engine code expects.
//
//  netadr_t : 20 bytes, identical to win_net.h's KISAK_RADIANT branch — the
//             cod4-accurate Q3-lineage layout WITH the legacy ipx[10] tail.
//             Deliberately NOT net_chan.h's reduced 12-byte SP variant (no ipx);
//             win_local.h and win_net.h share the KISAK_RADIANT_NETADR_DEFINED
//             guard so the two definitions stay byte-identical.
//  msg_t    : full 40-byte (0x28) layout copied verbatim from qcommon/msg.h, so
//             that net / client code eventually pulled into the editor
//             (remote-compile / live-reload) sees the correct field offsets.
//             No compiled radiant TU references msg_t today — the
//             Sys_GetPacket / Sys_GetBroadcastPacket decls that would are
//             SP/MP-gated below.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef KISAK_RADIANT_NETADR_DEFINED
#define KISAK_RADIANT_NETADR_DEFINED
struct netadr_t { int type; unsigned char ip[4]; unsigned short port; unsigned char ipx[10]; };
#endif
#ifndef KISAK_RADIANT_MSG_DEFINED
#define KISAK_RADIANT_MSG_DEFINED
struct msg_t   // == qcommon/msg.h (sizeof 0x28)
{
    int overflowed;
    int readOnly;
    unsigned char *data;
    unsigned char *splitData;
    int maxsize;
    int cursize;
    int splitSize;
    int readcount;
    int bit;
    int lastEntityRef;
};
#endif
// Permanent layout regression net — fail the build if either struct drifts from
// the real engine layout documented above.
static_assert(sizeof(netadr_t) == 20,            "radiant netadr_t must match win_net.h (cod4) layout");
static_assert(offsetof(netadr_t, ip)   == 4,     "netadr_t.ip offset");
static_assert(offsetof(netadr_t, port) == 8,     "netadr_t.port offset");
static_assert(offsetof(netadr_t, ipx)  == 10,    "netadr_t.ipx offset");
static_assert(sizeof(msg_t) == 40,               "radiant msg_t must match qcommon/msg.h layout");
static_assert(offsetof(msg_t, data)      == 8,   "msg_t.data offset");
static_assert(offsetof(msg_t, maxsize)   == 16,  "msg_t.maxsize offset");
static_assert(offsetof(msg_t, readcount) == 28,  "msg_t.readcount offset");
#endif

void	IN_MouseEvent (int mstate);

void	Sys_CreateConsole( void );
void	Sys_DestroyConsole( void );
void Sys_ShowConsole();

char	*Sys_ConsoleInput (void);

#if defined(KISAK_MP) || defined(KISAK_SP)
void Sys_ShowIP();
bool Sys_IsLANAddress(netadr_t adr);
bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr);

struct netadr_t;
struct msg_t;

qboolean	Sys_GetPacket ( netadr_t *net_from, msg_t *net_message );
qboolean	Sys_GetBroadcastPacket( msg_t *net_message );
#endif

// Input subsystem

void	IN_Init (void);
void	IN_Shutdown (void);
void	IN_JoystickCommands (void);

void IN_ShowSystemCursor(BOOL show);

// KISAKTODO void	IN_Move (usercmd_s *cmd); // usercmd_t -> usercmd_s
// add additional non keyboard / non mouse movement on top of the keyboard move cmd

void	IN_DeactivateWin32Mouse( void);

void	IN_Activate (qboolean active);
void	IN_Frame (void);

bool IN_IsTalkKeyHeld();

// window procedure
#ifndef _XBOX
LRESULT WINAPI MainWndProc (
    HWND    hWnd,
    UINT    uMsg,
    WPARAM  wParam,
    LPARAM  lParam);
#endif

void Conbuf_AppendText( const char *msg );
void Conbuf_AppendTextInMainThread(const char* msg);

#ifndef _XBOX
// LWSS: Accurate to cod4
typedef struct
{
	HINSTANCE		reflib_library;		// Handle to refresh DLL 
	qboolean		reflib_active;

	HWND			hWnd;
	HINSTANCE		hInstance;
	qboolean		activeApp;
	qboolean		isMinimized;
	qboolean		recenterMouse;

	OSVERSIONINFO	osversion;

	// when we get a windows message, we store the time off so keyboard processing
	// can know the exact time of an event
	unsigned		sysMsgTime;
} WinVars_t;

extern WinVars_t	g_wv;
#endif

struct alignas(8) SysInfo // sizeof=0x260
{                                       // ...
	double cpuGHz;                 // ...
	double configureGHz;           // ...
	int logicalCpuCount;                // ...
	int physicalCpuCount;               // ...
	int sysMB;                          // ...
	char gpuDescription[512];           // ...
	bool SSE;                           // ...
	char cpuVendor[13];                 // ...
	char cpuName[49];                   // ...
	// padding byte
	// padding byte
	// padding byte
	// padding byte
	// padding byte
};

#define	MAX_QUED_EVENTS		256
#define	MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

// LWSS add

#if defined(_WIN32)
extern _RTL_CRITICAL_SECTION s_criticalSections[];
#else
extern std::mutex s_criticalSections[];
#endif

extern int client_state; // LWSS ADD. This looks similar to signonstate
extern HWND g_splashWnd;

#if defined(KISAK_RADIANT)
// Radiant tools build: use SP-compatible critical section layout
enum CriticalSection : __int32
{
	CRITSECT_CONSOLE = 0x0,
	CRITSECT_DEBUG_SOCKET = 0x1,
	CRITSECT_COM_ERROR = 0x2,
	CRITSECT_STATMON = 0x3,
	CRITSECT_SOUND_ALLOC = 0x4,
	CRITSECT_MEM_ALLOC0 = 0x5,
	CRITSECT_MEM_ALLOC1 = 0x6,
	CRITSECT_DEBUG_LINE = 0x7,
	CRITSECT_ALLOC_MARK = 0x8,
	CRITSECT_STREAMED_SOUND = 0x9,
	CRITSECT_FAKELAG = 0xA,
	CRITSECT_CLIENT_MESSAGE = 0xB,
	CRITSECT_CLIENT_CMD = 0xC,
	CRITSECT_DOBJ_ALLOC = 0xD,
	CRITSECT_START_SERVER = 0xE,
	CRITSECT_XANIM_ALLOC = 0xF,
	CRITSECT_KEY_BINDINGS = 0x10,
	CRITSECT_FX_VIS = 0x11,
	CRITSECT_SERVER_MESSAGE = 0x12,
	CRITSECT_SCRIPT_STRING = 0x13,
	CRITSECT_MEMORY_TREE = 0x14,
	CRITSECT_ASSERT = 0x15,
	CRITSECT_SCRIPT_DEBUGGER_ALLOC = 0x16,
	CRITSECT_MISSING_ASSET = 0x17,
	CRITSECT_PHYSICS = 0x18,
	CRITSECT_LIVE = 0x19,
	CRITSECT_AUDIO_PHYSICS = 0x1A,
	CRITSECT_CINEMATIC = 0x1B,
	CRITSECT_CINEMATIC_TARGET_CHANGE = 0x1C,
	CRITSECT_FX_ALLOC = 0x1D,
	CRITSECT_NETTHREAD_OVERRIDE = 0x1E,
	CRITSECT_CBUF = 0x1F,
	CRITSECT_SYS_EVENT_QUEUE,
	CRITSECT_FATAL_ERROR,
	CRITSECT_GPU_FENCE,
	CRITSECT_COUNT,
};
#elif defined(KISAK_MP)
enum CriticalSection : int
{
	CRITSECT_CONSOLE = 0x0,
	CRITSECT_DEBUG_SOCKET = 0x1,
	CRITSECT_COM_ERROR = 0x2,
	CRITSECT_STATMON = 0x3,
	CRITSECT_DEBUG_LINE = 0x4,
	CRITSECT_ALLOC_MARK = 0x5,
	CRITSECT_SCRIPT_STRING = 0x6,
	CRITSECT_MEMORY_TREE = 0x7,
	CRITSECT_ASSERT = 0x8,
	CRITSECT_RD_BUFFER = 0x9,
	CRITSECT_SYS_EVENT_QUEUE = 0xA,
	CRITSECT_GPU_FENCE = 0xB,
	CRITSECT_FATAL_ERROR = 0xC,
	CRITSECT_SCRIPT_DEBUGGER_ALLOC = 0xD,
	CRITSECT_MISSING_ASSET = 0xE,
	CRITSECT_PHYSICS = 0xF,
	CRITSECT_LIVE = 0x10,
	CRITSECT_AUDIO_PHYSICS = 0x11,
	CRITSECT_CINEMATIC = 0x12,
	CRITSECT_CINEMATIC_TARGET_CHANGE = 0x13,
	CRITSECT_FX_ALLOC = 0x14,
	CRITSECT_CBUF = 0x15,

	CRITSECT_COUNT = 0x16,
};
#elif KISAK_SP
enum CriticalSection : __int32
{
	CRITSECT_CONSOLE = 0x0,
	CRITSECT_DEBUG_SOCKET = 0x1,
	CRITSECT_COM_ERROR = 0x2,
	CRITSECT_STATMON = 0x3,
	CRITSECT_SOUND_ALLOC = 0x4,
	CRITSECT_MEM_ALLOC0 = 0x5,
	CRITSECT_MEM_ALLOC1 = 0x6,
	CRITSECT_DEBUG_LINE = 0x7,
	CRITSECT_ALLOC_MARK = 0x8,
	CRITSECT_STREAMED_SOUND = 0x9,
	CRITSECT_FAKELAG = 0xA,
	CRITSECT_CLIENT_MESSAGE = 0xB,
	CRITSECT_CLIENT_CMD = 0xC,
	CRITSECT_DOBJ_ALLOC = 0xD,
	CRITSECT_START_SERVER = 0xE,
	CRITSECT_XANIM_ALLOC = 0xF,
	CRITSECT_KEY_BINDINGS = 0x10,
	CRITSECT_FX_VIS = 0x11,
	CRITSECT_SERVER_MESSAGE = 0x12,
	CRITSECT_SCRIPT_STRING = 0x13,
	CRITSECT_MEMORY_TREE = 0x14,
	CRITSECT_ASSERT = 0x15,
	CRITSECT_SCRIPT_DEBUGGER_ALLOC = 0x16,
	CRITSECT_MISSING_ASSET = 0x17,
	CRITSECT_PHYSICS = 0x18,
	CRITSECT_LIVE = 0x19,
	CRITSECT_AUDIO_PHYSICS = 0x1A,
	CRITSECT_CINEMATIC = 0x1B,
	CRITSECT_CINEMATIC_TARGET_CHANGE = 0x1C,
	CRITSECT_FX_ALLOC = 0x1D,
	CRITSECT_NETTHREAD_OVERRIDE = 0x1E,
	CRITSECT_CBUF = 0x1F,

	// LWSS ADD
	CRITSECT_SYS_EVENT_QUEUE,
	CRITSECT_FATAL_ERROR,
	CRITSECT_GPU_FENCE,
	// LWSS END

	CRITSECT_COUNT,
};

#endif

struct sysEvent_t // sizeof=0x18 on Windows, 0x20 on Linux
{                                       // ...
	int evTime;                         // ...
	sysEventType_t evType;              // ...
	int evValue;                        // ...
	int evValue2;                       // ...
	int evPtrLength;                    // ...
	void *evPtr;                        // ...
#ifndef _WIN32
	// Linux only: SE_MOUSE keeps the absolute position in the same record as the
	// relative delta, so one drained record describes one sample completely.
	// Zero for every other event type, and absent from the Windows layout.
	int evValue3;                       // absolute x
	int evValue4;                       // absolute y
#endif
};

struct FastCriticalSection
{
	volatile uint32_t readCount;
	volatile uint32_t writeCount;
};

void Sys_InitializeCriticalSections();
void Sys_EnterCriticalSection(int critSect);
void Sys_LeaveCriticalSection(int critSect);
void Sys_LockWrite(FastCriticalSection* critSect);
void Sys_UnlockWrite(FastCriticalSection* critSect);

int Sys_InterlockedIncrement(uint *addend);
int Sys_InterlockedDecrement(uint *addend);

void Sys_SetErrorText(const char* buf);
void Sys_Error(const char *error, ...);
void Sys_OutOfMemErrorInternal(const char* filename, int line);
void Sys_NormalExit();

void Sys_OpenURL(const char *url, int doexit);
void Sys_Quit();
void Sys_Print(const char *msg);
char *Sys_GetClipboardData();
int Sys_SetClipboardData(const char *text);
void Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr);
void Sys_ShutdownEvents();
void Sys_LoadingKeepAlive();
sysEvent_t *Sys_GetEvent(sysEvent_t *result);
void Sys_Init();

void Sys_In_Restart_f();
#ifdef KISAK_MP
void Sys_Net_Restart_f();
void Sys_Listen_f();
#endif

void Sys_Mkdir(const char *path);
BOOL Sys_RemoveDirTree(const char *path);
int Sys_CountFileList(char **list);
char **Sys_ListFiles(
	const char *directory,
	const char *extension,
	const char *filter,
	int *numfiles,
	int wantsubs);
char *Sys_Cwd();
const char *Sys_DefaultCDPath();
char *Sys_DefaultInstallPath();
void Sys_QuitAndStartProcess(const char *exeName, const char *parameters);


// win_voice
bool Voice_SendVoiceData();
bool Voice_Init();
void Voice_Shutdown();
double Voice_GetVoiceLevel();
void Voice_Playback();
int Voice_GetLocalVoiceData();
void Voice_IncomingVoiceData(unsigned __int8 talker, unsigned __int8 *data, int packetDataSize);
bool Voice_IsClientTalking(uint32_t clientNum);
char Voice_StartRecording();
char Voice_StopRecording();

extern SysInfo sys_info;