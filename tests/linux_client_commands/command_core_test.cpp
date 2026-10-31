// Production command registry, tokenizer and buffers. Single-threaded process,
// logging and external game/transport services below are explicit test boundaries.
#include <qcommon/cmd.h>
#include <qcommon/cmd_services.h>
#include <qcommon/qcommon.h>
#include <universal/q_shared.h>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>

static int calls, forwards, monitored, waits, checks, failures;
static std::string argument;
static void Mark() { ++calls;argument=Cmd_Argv(1); }
static void ServerMark() { ++calls;argument=SV_Cmd_Argv(1); }
static cmd_function_s first{}, second{}, serverCommand{}, throwingCommand{}, jumpingCommand{};
static std::jmp_buf errorTarget;
static void Jump() { std::longjmp(errorTarget,1); }
static void Throw() { throw std::runtime_error("fixture command failure"); }
extern bool cmd_insideCBufExecute[1];
extern CmdText sv_cmd_text;
extern int cmd_wait;
void Z_Free(void *p,int) { std::free(p); }
void Com_Printf(int,const char*,...) {}
void Com_PrintError(int,const char*,...) {}
void Com_Error(errorParm_t,const char *text,...) { throw std::runtime_error(text); }
void MyAssertHandler(const char*,int,int,const char *text,...) { throw std::runtime_error(text); }
bool Sys_IsMainThread() { return true; }
void Sys_EnterCriticalSection(int) {}
void Sys_LeaveCriticalSection(int) {}
void *Sys_GetValue(int) { static va_info_t info{};return &info; }
class FixtureServices final : public CmdServices {
public:
    void Monitor(const char*) override { ++monitored; }
    void WaitServer() override { ++waits; }
    bool InServerFrame() override { return false; }
    int DvarCommand() override { return 0; }
    bool ServerRunning() override { return false; }
    bool ServerGameCommand() override { std::abort(); }
    void Forward(int,const char*) override { ++forwards; }
};
CmdServices &Cmd_GetServices() { static FixtureServices services;return services; }
static void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
static void Execute(const char *text) { std::string copy=text;Cmd_ExecuteSingleCommand(0,0,copy.data()); }
static void Queue(const std::string &text) { Cbuf_AddText(0,text.c_str()); }
static void Reset() {
    Cmd_ShutdownCore();Cmd_InitCore();Cbuf_Init();calls=forwards=monitored=waits=0;argument.clear();
}
static void ServerQueue(const std::string &text) {
    if(text.size()>=unsigned(sv_cmd_text.maxsize))std::abort();
    std::memcpy(sv_cmd_text.data,text.data(),text.size());sv_cmd_text.cmdsize=text.size();
}
int main() {
    Cmd_InitCore();Cbuf_Init();
    Cmd_RemoveCommand("wait");Cmd_RemoveCommand("cmdlist");
    Execute("unknown");Check(forwards==1,"empty registry reaches explicit unknown-command boundary without crash");
    Cmd_AddCommandInternal("mark",Mark,&first);Execute("mark one");
    Check(calls==1 && argument=="one" && forwards==1,"sole registered command executes");
    Cmd_AddCommandInternal("other",Mark,&second);Execute("mark tail");
    Check(calls==2 && argument=="tail","tail command executes in multi-entry registry");
    Cmd_RemoveCommand("other");Cmd_RemoveCommand("mark");Execute("unknown");
    Check(forwards==2 && !Cmd_FindCommand("mark"),"removing all records leaves safe empty registry");
    Reset();Cmd_AddCommandInternal("mark",Mark,&first);
    Queue("mark \"a;b\";mark second\n");Cbuf_Execute(0,0);
    Check(calls==2 && argument=="second" && cmd_args.nesting==-1,"quoted delimiters and successive commands preserve tokenizer lifecycle");
    Queue("mark before;wait 2;mark after\n");Cbuf_Execute(0,0);
    Check(calls==3 && argument=="before","wait leaves queued work across ticks");
    Cbuf_Execute(0,0);Check(calls==3,"wait consumes requested additional tick");
    Cbuf_Execute(0,0);Check(calls==4 && argument=="after","queued work resumes after wait");
    Queue("wait 10;mark stale\n");Cbuf_Execute(0,0);Cbuf_Clear(0);Cbuf_Execute(0,0);
    Check(calls==4 && cmd_wait==0,"reset discards pending work and wait without executing stale commands");
    for(const char *bad : {"-1", "2junk", "99999999999999999999999"}) {
        const int before=calls;Queue(std::string("wait ")+bad+";mark invalid_wait\n");Cbuf_Execute(0,0);
        Check(calls==before+1 && cmd_wait==0,"invalid wait argument rejected without stalling queued work");Cbuf_Clear(0);
    }
    const int beforeZero=calls;Queue("wait 0;mark zero\n");Cbuf_Execute(0,0);
    Check(calls==beforeZero+1 && argument=="zero","wait zero preserves same-tick execution");
    Queue("wait;mark default\n");Cbuf_Execute(0,0);
    Check(calls==beforeZero+1,"wait without argument preserves one-tick deferral");
    Cbuf_Execute(0,0);Check(calls==beforeZero+2 && argument=="default","default wait resumes next tick");
    calls=4;
    Queue("mark fresh\n");Cbuf_Execute(0,0);Check(calls==5 && argument=="fresh","fresh work executes after reset");
    calls=forwards=0;
    std::string oversized="unknown ";oversized.resize(4096,'x');oversized+="mark\nmark next\n";
    Queue(oversized);Cbuf_Execute(0,0);
    Check(calls==1 && forwards==0 && argument=="next","oversized physical command discarded whole, suffix never executes, following command retained");
    calls=forwards=0;Cbuf_ExecuteBuffer(0,0,oversized.c_str());
    Check(calls==1 && forwards==0 && argument=="next","synchronous oversized command discarded whole without executing its suffix");
    calls=forwards=0;std::string boundary="mark ";boundary.resize(4095,'x');Queue(boundary+"\n");Cbuf_Execute(0,0);
    Check(calls==1 && argument.size()==4090 && forwards==0,"4095-byte command still accepted intact");
    calls=forwards=0;boundary.push_back('x');Queue(boundary);Cbuf_Execute(0,0);
    Check(calls==0 && forwards==0,"4096-byte unterminated command rejected without truncated dispatch");
    Queue("mark kept\n");Queue(std::string(65536,'x'));Cbuf_Execute(0,0);
    Check(calls==1 && argument=="kept","queue overflow preserves existing valid work");
    Cmd_AddServerCommandInternal("mark",ServerMark,&serverCommand);calls=waits=0;ServerQueue(oversized);Cbuf_Execute(0,0);
    Check(calls==1 && waits==1 && argument=="next","server buffer also discards whole oversized command before server dispatch");
    Queue("wait 100;mark never\n");Cbuf_Execute(0,0);Cmd_ShutdownCore();
    Check(!Cmd_FindCommand("mark") && cmd_wait==0 && cmd_args.nesting==-1 && sv_cmd_text.cmdsize==0,"shutdown clears owned registry, wait, tokenizer and queues");
    Cmd_InitCore();Cbuf_Init();Cmd_AddCommandInternal("mark",Mark,&first);calls=0;Queue("mark recreated\n");Cbuf_Execute(0,0);
    Check(calls==1 && argument=="recreated","same registration record works after full teardown/recreate");
    Cmd_AddCommandInternal("throw",Throw,&throwingCommand);Queue("throw;mark must_not_run\n");
    bool threw=false;try { Cbuf_Execute(0,0); }catch(const std::runtime_error&) { threw=true; }
    Check(threw && cmd_args.nesting==-1 && !cmd_insideCBufExecute[0],"throwing command unwinds tokenizer and execution ownership");
    Cmd_ShutdownCore();Cmd_InitCore();Cbuf_Init();calls=0;Cmd_AddCommandInternal("mark",Mark,&first);Cbuf_Execute(0,0);
    Check(calls==0 && !Cmd_FindCommand("throw"),"teardown after callback failure discards pending commands and registry");
    Cmd_AddCommandInternal("jump",Jump,&jumpingCommand);
    // Only POD/function frames lie between this setjmp and the callback.
    // No temporary std::string remains alive across Cbuf_Execute's longjmp.
    if (setjmp(errorTarget)==0) {
        Queue("jump;mark nonlocal_stale\n");Cbuf_Execute(0,0);
        Check(false,"legacy nonlocal callback reaches error recovery");
    } else {
        Cmd_ComErrorCleanup();
        Check(cmd_args.nesting==-1 && !cmd_insideCBufExecute[0],"legacy nonlocal error cleanup restores tokenizer/execution state");
    }
    Cmd_ShutdownCore();Cmd_InitCore();Cbuf_Init();calls=0;Cmd_AddCommandInternal("mark",Mark,&first);Cbuf_Execute(0,0);
    Check(calls==0 && !Cmd_FindCommand("jump"),"teardown after nonlocal error discards pending commands before recreation");
    Cmd_ShutdownCore();std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
