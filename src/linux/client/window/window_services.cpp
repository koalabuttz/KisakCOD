// Small process services for the standalone window shell/test. Actual queue
// locking comes from sys_critical.cpp. No engine game host is initialized.
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/assertive.h>
#include <SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

uint32_t Sys_Milliseconds() { return SDL_GetTicks(); }
void Com_Printf(int,const char *format,...) {
    va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);
}
void Z_Free(void *memory,int) { std::free(memory); }
void MyAssertHandler(const char *file,int line,int,const char *format,...) {
    std::fprintf(stderr,"Assertion %s:%d: ",file,line);
    va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::abort();
}
void Com_Error(errorParm_t,const char *format,...) {
    va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::exit(1);
}
