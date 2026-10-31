// Actual production CL_InitInput/CL_ShutdownInput within the standalone runtime.
// Commands execute through real Cmd/Cbuf. No renderer/frame/admission is faked.
#include <linux/client/window/client_input_host.h>
#include <client_mp/client_mp.h>
#include <qcommon/cmd.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <exception>

// MP cl_input.cpp owns these; the shared cl_input.h declares conflicting SP APIs.
extern const dvar_t *cl_analog_attack_threshold, *cl_stanceHoldTime;

namespace {
int checks, failures;
void Check(bool ok,const char *name) {
    ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);
}
constexpr const char *inputNames[]={
    "centerview", "+moveup", "-moveup", "+movedown", "-movedown",
    "+left", "-left", "+right", "-right", "+forward", "-forward", "+back", "-back",
    "+lookup", "-lookup", "+lookdown", "-lookdown", "+strafe", "-strafe",
    "+moveleft", "-moveleft", "+moveright", "-moveright", "+speed", "-speed",
    "+attack", "-attack", "+melee", "-melee", "+holdbreath", "-holdbreath",
    "+melee_breath", "-melee_breath", "+frag", "-frag", "+smoke", "-smoke",
    "+breath_sprint", "-breath_sprint", "+activate", "-activate", "+reload", "-reload",
    "+usereload", "-usereload", "+leanleft", "-leanleft", "+leanright", "-leanright",
    "+prone", "-prone", "+stance", "-stance", "+mlook", "-mlook", "toggleads", "leaveads",
    "+throw", "-throw", "+speed_throw", "-speed_throw", "+toggleads_throw", "-toggleads_throw",
    "lowerstance", "raisestance", "togglecrouch", "toggleprone", "goprone", "gocrouch",
    "+gostand", "-gostand", "+talk", "-talk", "+sprint", "-sprint"
};
bool Registered(bool present) {
    for(const char *name:inputNames)if(bool(Cmd_FindCommand(name))!=present)return false;
    return true;
}
void Execute(kisak::window::ClientInputHost &host,const char *text) {
    Cbuf_AddText(0,text);host.Tick();
}
int Buttons() {
    int result=0;
    CL_UpdateCmdButton(0,&result,8,1);  // strafe
    CL_UpdateCmdButton(0,&result,27,2); // sprint
    CL_UpdateCmdButton(0,&result,16,4); // frag
    return result;
}
}
int main() {
    try {
        for(int cycle=0;cycle<2;++cycle) {
            kisak::window::ClientInputHost host;
            Check(Registered(true),"full production input command set registered");
            const auto *analog=Dvar_FindVar("cl_analog_attack_threshold");
            const auto *stance=Dvar_FindVar("cl_stanceHoldTime");
            const auto *delta=Dvar_FindVar("cl_nodelta");
            Check(analog && stance && delta && analog==cl_analog_attack_threshold && stance==cl_stanceHoldTime
                && delta==cl_nodelta && std::fabs(analog->current.value-0.8f)<0.00001f
                && stance->current.integer==300 && !delta->current.enabled,
                "production input dvars own their normal defaults");
            Check(cl_freelook && cl_freelook->current.enabled && clientUIActives[0].connectionState==CA_DISCONNECTED
                && !clientUIActives[0].isRunning && !cls.rendererStarted && !cls.uiStarted,
                "input startup does not claim frame, renderer, UI or admission");
            Execute(host,"+talk 97 100;+sprint 98 100;+strafe 99 100;+frag 100 100\n");
            Check(IN_IsTalkKeyHeld() && Buttons()==7,"real registered talk, sprint, strafe and frag handlers activate input");
            Execute(host,"-talk 97 110;-sprint 98 110;-strafe 99 110;-frag 100 110\n");
            Check(!IN_IsTalkKeyHeld() && Buttons()==0,"matching real releases clear action state");
            Execute(host,"toggleprone\n");
            Check(clients[0].stance==CL_STANCE_PRONE && clientUIActives[0].connectionState==CA_DISCONNECTED,
                "production stance command updates owned input state without fake connection");
            Execute(host,"toggleprone;gocrouch;togglecrouch\n");
            Check(clients[0].stance==CL_STANCE_STAND,"production stance commands preserve toggle semantics");
            Execute(host,"+talk 97 120;+sprint 98 120;wait 20;+frag 100 130\n");
            host.Focus(false);host.Tick();
            Check(!IN_IsTalkKeyHeld() && Buttons()==0,"focus reset clears full input set and pending command work");
            Execute(host,"+talk 97 140\n");
            Check(IN_IsTalkKeyHeld(),"fresh full-input command executes after reset clears wait");
            host.Focus(false);
            CL_ShutdownInput();
            Check(Registered(false) && Cmd_FindCommand("echo")!=nullptr,
                "actual input shutdown removes only input commands while runtime stays alive");
            CL_InitInput();
            Check(Registered(true),"actual input initialization can restore registrations within the same runtime");
        }
        Check(!Cmd_FindCommand("+talk") && !Cmd_FindCommand("echo"),"owner teardown removes input and host registration records");
    } catch(const std::exception &error) {
        std::fprintf(stderr,"input startup failed: %s\n",error.what());++failures;
    }
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
