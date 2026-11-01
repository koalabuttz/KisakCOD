// Actual SL/MT allocation failure: process isolation keeps the pre-fix leaked
// lock from blocking teardown or hiding the result behind recursive locking.
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <universal/com_memory.h>
#include <script/scr_stringlist.h>
#include <ui/ui_shared.h>
#include <cstring>
#include <win32/win_local.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
extern scrStringDebugGlob_t* scrStringDebugGlob;
extern int Posix_CriticalSectionDepthForTest(int);
static int checks, failures;
static void Check(bool ok,const char* what) { ++checks; failures+=!ok; std::printf("%s: %s\n",ok?"PASS":"FAIL",what); std::fflush(stdout); }
int main()
{
    Sys_InitializeCriticalSections(); Sys_InitMainThread(); SL_Init();
    const char *original=CopyString("preserved");
    UILocalVar local{};local.type=UILOCALVAR_INT;UILocalVar_SetString(&local,const_cast<char*>("preserved"));
    UILocalVar integer{};integer.type=UILOCALVAR_INT;integer.u.integer=37;
    std::vector<const char*> refs;
    bool exhausted=false;
    for(unsigned i=0;i<256;++i) {
        std::string value(8192,'x'); auto prefix=std::to_string(i);value.replace(0,prefix.size(),prefix);
        try { refs.push_back(CopyString(value.c_str())); }
        catch(const std::runtime_error&) { exhausted=true;break; }
    }
    Check(exhausted && !refs.empty(),"real finite MT pool exhaustion reached");
    Check(Posix_CriticalSectionDepthForTest(CRITSECT_SCRIPT_STRING)==0,"exception releases outer string lock");
    std::atomic<bool> acquired=false;
    std::thread probe([&]{ Sys_EnterCriticalSection(CRITSECT_SCRIPT_STRING); acquired=true; Sys_LeaveCriticalSection(CRITSECT_SCRIPT_STRING); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!acquired && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Check(acquired,"another thread acquires string lock after exhaustion");
    if(!acquired) { std::printf("string owner: %d checks, %d failures\n",checks,failures);std::fflush(stdout);_exit(1); }
    probe.join();
    const auto before=scrStringDebugGlob->totalRefCount;
    const auto old=original;
    std::string failing(8192,'z');
    bool rejected=false;
    try { ReplaceString(&original,failing.c_str()); } catch(const std::runtime_error&) { rejected=true; }
    Check(rejected && original==old && !std::strcmp(original,"preserved"),"failed ReplaceString retains old owned reference");
    rejected=false;
    try { UILocalVar_SetString(&local,failing.data()); } catch(const std::runtime_error&) { rejected=true; }
    Check(rejected && local.type==UILOCALVAR_STRING && local.u.string==old,"failed UI string replacement preserves previous ownership");
    rejected=false;
    try { UILocalVar_SetString(&integer,failing.data()); } catch(const std::runtime_error&) { rejected=true; }
    Check(rejected && integer.type==UILOCALVAR_INT && integer.u.integer==37,"failed UI type conversion preserves integer");
    Check(scrStringDebugGlob->totalRefCount==before,"all failed replacements preserve actual reference counts");
    UILocalVar_SetString(&local,const_cast<char*>(local.u.string));
    ReplaceString(&original,original);
    Check(local.u.string==old && original==old && scrStringDebugGlob->totalRefCount==before,"full-pool self aliases reuse existing storage without allocation");
    FreeString(original);FreeString(local.u.string);
    for(auto p:refs) FreeString(p);
    const auto restored=CopyString("recovered");FreeString(restored);
    SL_Shutdown();Check(true,"pool releases all references and recovers after exhaustion");
    std::printf("string owner: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
