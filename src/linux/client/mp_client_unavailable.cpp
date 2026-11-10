// Client services native Linux does not provide yet, for KisakCOD-mp.
//
// KisakCOD-mp links the whole multiplayer game. What its units call that no
// Linux unit defines yet is here, in three kinds:
//
//   1. Optional services, answered the way the engine answers when the service
//      is missing on Windows: no remote script debugger attached, HTTP downloads
//      that could not start (dl_main.cpp without libcurl) and no voice device
//      (win_voice.cpp when DirectSound capture fails). Each says which Windows
//      state it is.
//   2. The fast-file branch of a function whose unit does not compile here.
//   3. Code that only raw-asset loading (useFastFile 0) or an attached remote
//      debugger reaches. Neither exists in this build, so each aborts with its
//      name instead of running invented code.
//
// Nothing here pretends a service works. Each entry leaves this file when the
// real service is ported; see docs/linux-mp-client.md.
//
// KisakCOD-sp (the singleplayer game) links this file too. Singleplayer has no
// HTTP downloads, voice or net_listen command, so those entries are
// multiplayer-only; the rest it calls the same way. See docs/linux-sp-game.md.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#ifdef KISAK_MP
#include <qcommon/dl_main.h>
#include <client_mp/client_mp.h>
#endif
#include <database/database.h>
#include <gfx_d3d/r_material.h>
#include <win32/win_local.h>
#include <win32/win_net_debug.h>
#include <xanim/xanim.h>

#include <cstdio>
#include <cstdlib>

namespace
{
[[noreturn]] void Unreachable(const char *what, const char *why)
{
    std::fprintf(stderr, "KisakCOD-mp: reached %s, which needs %s; native Linux does not have it\n", what, why);
    std::fflush(nullptr);
    std::abort();
}
} // namespace

// ---- 1. optional services ---------------------------------------------------

// src/win32/win_net_debug.cpp: the remote script debugger's socket. None is ever
// attached, which is the state before net_listen accepts one: every socket read,
// write and acknowledgement in scr_debugger.cpp sits behind
// Sys_IsRemoteDebugClient(), and Sys_UpdateDebugSocket answers 0 ("no event")
// when no listening socket exists.
int Sys_IsRemoteDebugClient()
{
    return 0;
}

int Sys_UpdateDebugSocket()
{
    return 0;
}

#ifdef KISAK_MP
void Sys_Listen_f()
{
    Com_PrintWarning(CON_CHANNEL_DONT_FILTER,
                     "net_listen: the remote script debugger is not available on native Linux.\n");
}

// src/win32/win_voice.cpp with no voice device: Voice_Init fails, so
// g_voice_initialized stays 0 and every other entry point takes its
// "not initialized" branch. No voice data is captured, sent or played, no level
// is reported and no client is heard talking.
bool Voice_Init()
{
    return false;
}

void Voice_Shutdown()
{
    cl_voiceCommunication.voicePacketCount = 0;
}

double Voice_GetVoiceLevel()
{
    return 0.0;
}

void Voice_Playback()
{
}

int Voice_GetLocalVoiceData()
{
    return 0;
}

void Voice_IncomingVoiceData(unsigned __int8 talker, unsigned __int8 *data, int packetDataSize)
{
    (void)talker;
    (void)data;
    (void)packetDataSize;
}

bool Voice_IsClientTalking(uint32_t clientNum)
{
    iassert(clientNum < 64);
    return false;
}

// src/qcommon/dl_main.cpp is the libcurl HTTP downloader, which this target
// links only with KISAK_LINUX_HTTP_DOWNLOADS. Without it, these are its answers
// when curl could not be initialized: nothing is in progress, cancelling does
// nothing, and a download cannot begin, so the client tells the server
// "wwwdl fail" and falls back to the UDP download.
#ifndef KISAK_LINUX_HTTP_DOWNLOADS
void DL_CancelDownload()
{
}

int DL_BeginDownload(char *localName, char *remoteName)
{
    DL_CancelDownload();
    if (!localName || !*localName || !remoteName || !*remoteName)
        return 0;
    Com_Printf(CON_CHANNEL_CLIENT, "Could not initialize HTTP downloads\n");
    return 0;
}

bool DL_InProgress()
{
    return false;
}
#endif // !KISAK_LINUX_HTTP_DOWNLOADS

#endif // KISAK_MP

// win_wndproc.cpp's storage; R_RegisterDvars registers all three. The window
// procedure itself is src/linux/client/mp_client_input.cpp.
const dvar_t *r_fullscreen;
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;

// ---- 2. fast-file branches ---------------------------------------------------

// src/gfx_d3d/r_material_load_obj.cpp:6033, which needs d3dx9 to compile. With
// fast files only the technique sets hold device objects: the literal, string
// and state-map tables it clears are filled by raw-asset loading alone, and the
// rest is behind !IsFastFileLoad().
void Material_FreeAll()
{
    if (!IsFastFileLoad())
        Unreachable("Material_FreeAll", "raw-asset loading (useFastFile 0)");
    DB_EnumXAssets(ASSET_TYPE_TECHNIQUE_SET, Material_ReleaseTechniqueSet, 0, 1);
}

// ---- 3. paths this build never takes -------------------------------------------

#define KISAK_UNREACHABLE(ret, name, params, why) \
    ret name params { Unreachable(#name, why); }

// Raw-asset loading. Material_Register, Material_FindTechniqueSet and
// Material_Init call these only when IsFastFileLoad() is false; the BSP and
// effects loaders that call the others run only for raw assets too.
#define KISAK_RAW_ASSETS "raw-asset loading (useFastFile 0)"
KISAK_UNREACHABLE(Material *, Material_Load, (char *, int), KISAK_RAW_ASSETS)
KISAK_UNREACHABLE(MaterialTechniqueSet *, Material_FindTechniqueSet_LoadObj, (const char *, MtlTechSetNotFoundBehavior),
                  KISAK_RAW_ASSETS)
KISAK_UNREACHABLE(void, Material_PreLoadAllShaderText, (), KISAK_RAW_ASSETS)
KISAK_UNREACHABLE(Material *, R_GetBspMaterial, (uint32_t), KISAK_RAW_ASSETS)
KISAK_UNREACHABLE(void, Material_GetInfo, (Material *, MaterialInfo *), KISAK_RAW_ASSETS)

// The remote debugger's socket traffic, all behind Sys_IsRemoteDebugClient().
#define KISAK_REMOTE_DEBUGGER "an attached remote script debugger"
KISAK_UNREACHABLE(void, Sys_AckDebugSocket, (), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_EndWriteDebugSocket, (), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_FlushDebugSocketData, (), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(int, Sys_ReadDebugSocketData, (char *, int, int), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(int, Sys_ReadDebugSocketInt, (), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(char *, Sys_ReadDebugSocketString, (), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_ReadDebugSocketStringBuffer, (char *, int), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_WriteDebugSocketInt, (int), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_WriteDebugSocketMessageType, (unsigned __int8), KISAK_REMOTE_DEBUGGER)
KISAK_UNREACHABLE(void, Sys_WriteDebugSocketString, (char *), KISAK_REMOTE_DEBUGGER)

#ifdef KISAK_MP
#ifndef KISAK_LINUX_HTTP_DOWNLOADS
// HTTP download progress, reached only while a download that began is running.
#define KISAK_HTTP "an HTTP download in progress (libcurl)"
KISAK_UNREACHABLE(int, DL_DownloadLoop, (), KISAK_HTTP)
KISAK_UNREACHABLE(bool, DL_DLIsMotd, (), KISAK_HTTP)
#endif // !KISAK_LINUX_HTTP_DOWNLOADS
#endif // KISAK_MP
