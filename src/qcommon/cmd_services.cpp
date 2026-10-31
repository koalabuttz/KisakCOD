#include "cmd_services.h"
#include "cmd.h"
#include "qcommon.h"
#include "threads.h"
#include <script/scr_debugger.h>
// Narrow server interface: no need for server world/demo structures here.
int __cdecl SV_GameCommand();
void __cdecl SV_WaitServer();
extern int com_inServerFrame;
#if defined(KISAK_MP) && !defined(KISAK_DEDICATED)
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include <client/client.h>
#endif

namespace {
class GameCmdServices final : public CmdServices {
public:
    void Monitor(const char *text) override { Scr_MonitorCommand(text); }
    void WaitServer() override {
#ifndef KISAK_RADIANT
        SV_WaitServer();
#endif
    }
    bool InServerFrame() override {
#ifndef KISAK_RADIANT
        return com_inServerFrame;
#else
        return false;
#endif
    }
    int DvarCommand() override { return Dvar_Command(); }
    bool ServerRunning() override { return com_sv_running && com_sv_running->current.enabled; }
    bool ServerGameCommand() override { return SV_GameCommand(); }
    void Forward(int client,const char *text) override {
#if defined(__linux__) && defined(KISAK_DEDICATED)
        Com_Printf(CON_CHANNEL_SYSTEM,"Unknown command: %s\n",Cmd_Argv(0));
#elif !defined(KISAK_RADIANT)
        CL_ForwardCommandToServer(client,text);
#endif
    }
};
}
CmdServices &Cmd_GetServices() { static GameCmdServices services;return services; }
