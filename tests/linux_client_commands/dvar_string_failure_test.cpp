// Real pool exhaustion inside new-dvar registration. The old write-lock leak
// is observed from another thread; failed baseline exits without unsafe cleanup.
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <qcommon/cmd.h>
#include <universal/com_memory.h>
#include <script/scr_stringlist.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
extern scrStringDebugGlob_t* scrStringDebugGlob;
static int checks,failures;
static void Check(bool ok,const char* name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);std::fflush(stdout); }
int main(int argc,char** argv)
{
    const bool failName=argc>1 && !std::strcmp(argv[1],"--name");
    Sys_InitializeCriticalSections();Sys_InitMainThread();SL_Init();Cmd_InitCore();Dvar_Init();
    // Leave previously used externally-owned string slots behind, then reuse.
    Dvar_RegisterString("previous_owned","old value",DVAR_EXTERNAL,"");
    Dvar_Shutdown();Dvar_Init();
    const auto prior=Dvar_RegisterString("unchanged","stable",DVAR_EXTERNAL,"");
    const auto latched=Dvar_RegisterString("latched_pending","original",DVAR_EXTERNAL|DVAR_LATCH,"");
    std::vector<const char*> refs;bool exhausted=false;
    for(unsigned i=0;i<256;++i) {
        std::string text(8192,'x');const auto prefix=std::to_string(i);text.replace(0,prefix.size(),prefix);
        try { refs.push_back(CopyString(text.c_str())); } catch(const std::runtime_error&) { exhausted=true;break; }
    }
    Check(exhausted,"real MT pool exhausted before registration");
    // Public Dvar_SetStringFromSource deliberately truncates to 1023 bytes.
    // Exhaust that smaller bucket too, rather than mistaking truncation for
    // failed transactional assignment.
    bool setterExhausted=false;
    for(unsigned i=0;i<64;++i) {
        std::string text(1023,'y');const auto prefix=std::to_string(i);text.replace(0,prefix.size(),prefix);
        try { refs.push_back(CopyString(text.c_str())); } catch(const std::runtime_error&) { setterExhausted=true;break; }
    }
    if(!setterExhausted) throw std::runtime_error("setter allocation bucket not exhausted");
    const auto before=scrStringDebugGlob->totalRefCount;
    const std::string large(8192,'z');
    const char* name=failName?large.c_str():"failed_owned_name";
    const auto originalCurrent=prior->current.string;
    const auto originalPending=latched->latched.string;
    bool setFailed=false;
    try { Dvar_SetString(prior,large.c_str()); } catch(const std::runtime_error&) {setFailed=true;}
    Check(setFailed && prior->current.string==originalCurrent && prior->latched.string==originalCurrent && prior->reset.string==originalCurrent,"failed current assignment preserves all string aliases");
    setFailed=false;
    try { Dvar_SetStringFromSource(const_cast<dvar_s*>(latched),const_cast<char*>(large.c_str()),DVAR_SOURCE_EXTERNAL); } catch(const std::runtime_error&) {setFailed=true;}
    Check(setFailed && latched->current.string==originalPending && latched->latched.string==originalPending && latched->reset.string==originalPending,"failed latched assignment preserves all string aliases");
    Check(scrStringDebugGlob->totalRefCount==before,"failed dvar assignments preserve owned references");
    bool failed=false;
    try { Dvar_RegisterString(name,large.c_str(),DVAR_EXTERNAL,""); } catch(const std::runtime_error&) { failed=true; }
    Check(failed,failName?"owned-name allocation fails":"value allocation fails after owned-name allocation");
    Check(scrStringDebugGlob->totalRefCount==before,"failed registration leaves reference ownership unchanged");
    std::atomic<bool> finished=false;const dvar_s* published=nullptr;
    std::thread probe([&]{published=Dvar_FindVar(name);finished=true;});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!finished && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    Check(finished,"registration failure releases dvar write lock");
    if(!finished){std::printf("dvar string failure: %d checks, %d failures\n",checks,failures);std::fflush(stdout);_exit(1);}
    probe.join();Check(published==nullptr,"failed registration is not published");
    Check(!std::strcmp(prior->current.string,"stable"),"existing dvar remains intact");
    for(auto p:refs)FreeString(p);
    const auto retry=Dvar_RegisterString(name,"retry",DVAR_EXTERNAL,"");
    Check(retry && !std::strcmp(retry->current.string,"retry"),"registration recovers after releasing pool capacity");
    Dvar_Shutdown();Cmd_ShutdownCore();Check(scrStringDebugGlob->totalRefCount==0,"shutdown balances names and weak string aliases");SL_Shutdown();
    std::printf("dvar string failure: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
