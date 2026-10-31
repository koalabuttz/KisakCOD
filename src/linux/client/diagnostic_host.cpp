// Standalone diagnostic host services. This is not Com_Init/CL_Frame: no console,
// game clients, server, profiler, VM or engine allocator is initialized here.
#include "diagnostic_host.h"
#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/threads.h>
#include <universal/assertive.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

const dvar_t *com_dedicated;
const dvar_t *com_sv_running;

static void log(const char *format, va_list args) { std::vfprintf(stderr, format, args); }
#define DIAGNOSTIC_LOG(name) void name(int, const char *format, ...) { va_list args; va_start(args,format); log(format,args); va_end(args); }
DIAGNOSTIC_LOG(Com_Printf)
DIAGNOSTIC_LOG(Com_DPrintf)
DIAGNOSTIC_LOG(Com_PrintError)
DIAGNOSTIC_LOG(Com_PrintWarning)
void Com_Error(errorParm_t, const char *format, ...) {
    va_list args; va_start(args,format); log(format,args); va_end(args);
    std::exit(5);
}
void MyAssertHandler(const char *file, int line, int, const char *format, ...) {
    std::fprintf(stderr,"Assertion at %s:%d: ",file,line);
    va_list args; va_start(args,format); log(format,args); va_end(args); std::abort();
}
uint32_t Sys_Milliseconds() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
}
const char *CopyString(const char *text) {
    auto result = ::strdup(text);
    if (!result) Com_Error(ERR_FATAL,"Diagnostic string allocation failed\n");
    return result;
}
void FreeString(const char *text) { std::free(const_cast<char *>(text)); }
char *Z_VirtualAlloc(int size, const char *, int) {
    auto result = static_cast<char *>(std::calloc(1,size));
    if (!result) Com_Error(ERR_FATAL,"Diagnostic allocation failed\n");
    return result;
}
void Z_VirtualFree(void *memory) { std::free(memory); }
void _copyDWord(uint32_t *dest, uint32_t value, uint32_t count) { std::fill_n(dest,count,value); }
// No console exists in this host. Core dvar registration/validation is production.
void Dvar_AddCommands() {}
bool Com_LogFileOpen() { return false; }
// The diagnostic never hosts a server or enables its authentication exceptions.
bool SV_UnverifiedGuidsEnabled() { return false; }
void CL_Netchan_AddOOBProfilePacket(int, int) {}
void SV_Netchan_AddOOBProfilePacket(int) {}

void Diagnostic_InitHost() {
    Sys_InitMainThread();
    Dvar_Init();
    com_dedicated = Dvar_RegisterInt("dedicated",0,DvarLimits(0,2),DVAR_INIT,"Standalone client diagnostic");
    com_sv_running = Dvar_RegisterBool("sv_running",false,DVAR_INIT,"No hosted server");
    // Do not claim full Netchan_Init: there is no console/profile command host.
    // Register the production OOB path's controls with their standard defaults.
    showpackets = Dvar_RegisterInt("showpackets",0,DvarLimits(0,2),DVAR_NOFLAG,"Show packets");
    fakelag_current = Dvar_RegisterInt("fakelag_current",0,DvarLimits(0,999),DVAR_NOFLAG,"Current fake lag value");
    fakelag_target = Dvar_RegisterInt("fakelag_target",0,DvarLimits(0,999),DVAR_NOFLAG,"Target fake lag value");
    fakelag_currentjitter = Dvar_RegisterInt("fakelag_currentjitter",0,DvarLimits(0,999),DVAR_NOFLAG,"Current jitter");
    fakelag_jitter = Dvar_RegisterInt("fakelag_jitter",0,DvarLimits(0,999),DVAR_NOFLAG,"Fake lag jitter");
    fakelag_packetloss = Dvar_RegisterFloat("fakelag_packetloss",0.0,DvarLimits(0.0f,1.0f),DVAR_NOFLAG,"Fake loss");
    // No file/config/console settings are imported. Bind an ephemeral socket.
    const dvar_t *bind = Dvar_RegisterString("net_ip","localhost",DVAR_LATCH,"Network IP Address");
    const dvar_t *port = Dvar_RegisterInt("net_port",28960,DvarLimits(0,65535),DVAR_LATCH,"Network port");
    Dvar_SetString(const_cast<dvar_t *>(bind), const_cast<char *>("0.0.0.0"));
    Dvar_SetInt(port,0);
}

void Diagnostic_ShutdownHost() { Dvar_Shutdown(); }
