// Genuine prepared assets, real engine UI, isolated executable allocation faults.
#include <linux/client/window/menu_input_owner.h>
#include <linux/client/window/client_input_host.h>
#include <linux/client/window/window_host.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include <algorithm>
namespace {bool rejectSurface=false;thread_local bool rejectAllocation=false;thread_local unsigned attempted=0;int checks,failures;
void Check(bool ok,const char *what){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",what);}}
void *operator new(size_t size){++attempted;if(rejectAllocation){rejectAllocation=false;throw std::bad_alloc();}if(auto*p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void operator delete(void*p) noexcept{std::free(p);}void operator delete(void*p,size_t)noexcept{std::free(p);}
void *operator new[](size_t size){return ::operator new(size);}void operator delete[](void*p)noexcept{::operator delete(p);}void operator delete[](void*p,size_t)noexcept{::operator delete(p);}
// Only this test link wraps the backend call; normal calls use real SDL.
extern "C" int __real_SDL_UpdateWindowSurface(SDL_Window*);
extern "C" int __wrap_SDL_UpdateWindowSurface(SDL_Window *window){if(rejectSurface){rejectSurface=false;SDL_SetError("injected surface update failure");return -1;}return __real_SDL_UpdateWindowSurface(window);}
int main(int argc,char**argv){if(argc!=3)return 2;Sys_InitializeCriticalSections();try{
 kisak::window::ClientInputHost input;kisak::window::Config cfg;cfg.width=640;cfg.height=480;
 kisak::window::WindowHost window(input,cfg);kisak::window::CpuRenderer renderer;
 kisak::window::MenuInputOwner owner(input,renderer,argv[1],argv[2],640,480);owner.Present(window);
 unsigned before=attempted;bool traceThrew=false;rejectAllocation=true;
 try{owner.Trace("fault-telemetry");}catch(...){traceThrew=true;}rejectAllocation=false;
 Check(!traceThrew && attempted==before,"state telemetry needs no C++ allocation during input/teardown");
 before=attempted;rejectAllocation=true;bool frameThrew=false;
 try{owner.Present(window);}catch(...){frameThrew=true;}rejectAllocation=false;
 Check(!frameThrew && attempted==before,"genuine steady frame needs no C++ allocation");
 const auto oldExtent=renderer.Pixels();const auto resizeCount=renderer.Presented();
 SDL_SetWindowSize(SDL_GetWindowFromID(window.Id()),320,240);
 rejectAllocation=true;bool resizeFailed=false;
 try{owner.Present(window);}catch(const std::bad_alloc&){resizeFailed=true;}rejectAllocation=false;
 Check(resizeFailed && renderer.Status()==kisak::window::CpuRenderer::State::ready && renderer.Pixels().width==oldExtent.width && renderer.Presented()==resizeCount,"physical resize allocation failure preserves usable old generation");
 owner.Present(window);Check(renderer.Pixels().width==320 && renderer.Presented()==resizeCount+1,"same owner retries physical resize after allocation failure");
 const auto presented=renderer.Presented();rejectSurface=true;bool failed=false;
 try{owner.Present(window);}catch(const std::exception&){failed=true;}
 Check(failed && !rejectSurface,"actual SDL presentation failure reaches caller explicitly");
 Check(renderer.Status()==kisak::window::CpuRenderer::State::ready,"failed presentation releases pending frame ownership for retry");
 Check(renderer.Presented()==presented,"failed SDL presentation does not advance presented counter");
 bool recovered=true;try{owner.Present(window);}catch(...){recovered=false;}
 Check(recovered && renderer.Presented()==presented+1,"same owner can present next frame after SDL failure");
 }catch(const std::exception&e){rejectAllocation=false;std::fprintf(stderr,"fixture: %s\n",e.what());return 1;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}
