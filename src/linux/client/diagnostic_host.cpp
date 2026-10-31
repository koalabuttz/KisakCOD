// Shared standalone runtime services: real command/dvar/NET lifecycle,
// no renderer, game server, VM, profiler or full Com_Init/CL_Frame.
#include "diagnostic_host.h"
#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <qcommon/cmd.h>
#include <qcommon/cmd_services.h>
#include <client_mp/cl_auth.h>
#include <linux/net_internal.h>
#include <linux/sys_events.h>
#include <stdexcept>
#include <universal/assertive.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int com_frameTime;
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
    throw std::runtime_error("engine runtime error (see diagnostic)");
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
// No game console UI exists. Core dvar registration and commands are production.
void Dvar_AddCoreCommands();
void Dvar_AddCommands() { Dvar_AddCoreCommands(); }
bool Com_LogFileOpen() { return false; }
// The diagnostic never hosts a server or enables its authentication exceptions.
bool SV_UnverifiedGuidsEnabled() { return false; }
void CL_Netchan_AddOOBProfilePacket(int, int) {}
void SV_Netchan_AddOOBProfilePacket(int) {}

static void ClearOwnedDvarPointers() {
    com_dedicated=com_sv_running=nullptr;
    showpackets=fakelag_current=fakelag_target=fakelag_currentjitter=fakelag_jitter=fakelag_packetloss=nullptr;
}
static void InitializeDvars() {
    Dvar_Init();
    com_dedicated = Dvar_RegisterInt("dedicated",0,DvarLimits(0,2),DVAR_INIT,"Standalone client runtime");
    com_sv_running = Dvar_RegisterBool("sv_running",false,DVAR_INIT,"No hosted server");
    // Do not claim full Netchan_Init: network profile commands are not hosted.
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



void Z_Free(void *memory,int) { std::free(memory); }
void ReplaceString(const char **target,const char *text) {
    const char *replacement=CopyString(text);FreeString(*target);*target=replacement;
}
void Com_FreeEvent(char *memory) { std::free(memory); }

namespace kisak::client {
namespace { Runtime *owner=nullptr; }
Runtime &Runtime::Current() {
    if(!owner)throw std::logic_error("client runtime unavailable");
    return *owner;
}
Runtime::Runtime() {
    if(owner)throw std::logic_error("one client runtime per process");
    Sys_InitializeCriticalSections();Sys_InitMainThread();
    Cmd_InitCore();Cbuf_Init();
    try {
        InitializeDvars();NET_Init();com_frameTime=Sys_Milliseconds();owner=this;
        std::puts("runtime=ready commands=engine network=engine-udp script_monitor=unavailable connected=false");
    } catch(...) {
        NET_Shutdown();Dvar_Shutdown();Cmd_ShutdownCore();ClearOwnedDvarPointers();throw;
    }
}
Runtime::~Runtime() {
    ClearCommands();Sys_ShutdownEvents();CL_CancelPlatformAuthTicket();
    NET_Shutdown();Cmd_ShutdownCore();Dvar_Shutdown();
    ClearOwnedDvarPointers();com_frameTime=0;owner=nullptr;
}
void Runtime::Tick() {
    com_frameTime=Sys_Milliseconds();++ticks_;
    try { Cbuf_Execute(0,0); }catch(...) { ClearCommands();throw; }
}
void Runtime::Queue(const char *text) { Cbuf_AddText(0,text); }
void Runtime::ClearCommands() { Cbuf_Clear(0); }
void Runtime::RestartNetwork() { NET_Shutdown();NET_Init();std::puts("network=restarted connected=false"); }
}
namespace {
class RuntimeCommands final : public CmdServices {
public:
    // No script VM monitor is installed. Command execution itself is production.
    void Monitor(const char *) override {}
    void WaitServer() override { throw std::runtime_error("server dispatch unavailable in client runtime"); }
    bool InServerFrame() override { return false; }
    int DvarCommand() override { return Dvar_Command(); }
    bool ServerRunning() override { return false; }
    bool ServerGameCommand() override { throw std::runtime_error("server game commands unavailable"); }
    void Forward(int,const char *text) override { std::fprintf(stderr,"Unknown command (not forwarded): %s\n",text); }
};
}
CmdServices &Cmd_GetServices() {
    static RuntimeCommands services;
    return services;
}
