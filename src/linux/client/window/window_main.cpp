#include "window_host.h"
#include "engine_input.h"
#include <linux/sys_events.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <ui/keycodes.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
struct Metrics {
    unsigned keys=0,characters=0,mouse=0,resizes=0,focus=0,resets=0;
    bool trace=false;
    std::array<bool,256> consumerHeld{};
} metrics;
int ShellMouse(int x,int y,int dx,int dy) {
    ++metrics.mouse;
    if(metrics.trace)std::printf("mouse x=%d y=%d dx=%d dy=%d\n",x,y,dx,dy);
    return 0;
}
void ResetShellInput(int) {
    ++metrics.resets;metrics.consumerHeld.fill(false);
    if(metrics.trace)std::puts("input=reset");
}
class ShellInput : public kisak::window::EngineInput {
public:
    ShellInput() : EngineInput(ShellMouse,ResetShellInput) {}
    void Resize(int width,int height) override {
        ++metrics.resizes;
        if(metrics.trace)std::printf("resize %d %d\n",width,height);
    }
    void OnFocus(bool focused) override {
        ++metrics.focus;
        if(metrics.trace)std::printf("focus %s\n",focused?"gained":"lost");
    }
    void OnQuit() override { std::puts("window=closing"); }
};
bool Number(const char *text,unsigned maximum,unsigned &value) {
    if(!*text)return false;
    unsigned parsed=0;
    for(const char *p=text;*p;++p) {
        if(*p<'0' || *p>'9' || parsed>(maximum-unsigned(*p-'0'))/10)return false;
        parsed=parsed*10+unsigned(*p-'0');
    }
    value=parsed;return value>0 && value<=maximum;
}
}
int main(int argc,char **argv)
{
    unsigned frames=0,runMilliseconds=0;
    bool relative=false;
    kisak::window::Config config;
    for(int i=1;i<argc;++i) {
        const std::string argument(argv[i]);
        if(argument=="--help") {
            std::puts("Usage: KisakCOD-client-window [--frames 1..10000] [--run-ms 1..60000] [--trace] [--relative] [--title NAME]\n"
                "SDL window/input shell. Escape or window close exits. Software surface only; no game session.");return 0;
        } else if(argument=="--frames" && i+1<argc) {
            if(!Number(argv[++i],10000,frames))return 2;
        } else if(argument=="--run-ms" && i+1<argc) {
            if(!Number(argv[++i],60000,runMilliseconds))return 2;
        } else if(argument=="--trace")metrics.trace=true;
        else if(argument=="--relative")relative=true;
        else if(argument=="--title" && i+1<argc && *argv[i+1])config.title=argv[++i];
        else return 2;
    }
    Sys_InitializeCriticalSections();
    try {
        ShellInput input;
        // Diagnostic preference only; the reusable platform host does not choose
        // the eventual game backend. SDL window surfaces may otherwise use GPU acceleration.
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"0");
        kisak::window::WindowHost host(input,config);
        host.Present(host.Focused()?35:45,host.Focused()?65:45,host.Focused()?100:45);
        std::printf("window=created driver=%s id=%u title=%s game=false presentation=SDL-window-surface\n",
            SDL_GetCurrentVideoDriver(),host.Id(),config.title);
        std::printf("window=ready id=%u driver=%s\n",host.Id(),SDL_GetCurrentVideoDriver());
        if(relative)host.SetRelativeMouse(true);
        bool lastRelative=host.RelativeMouseActive();
        std::string lastRelativeError=host.RelativeMouseError();
        if(relative)std::printf("relative requested=1 active=%d error=%s\n",lastRelative,lastRelativeError.c_str());
        std::fflush(stdout);
        const uint32_t started=SDL_GetTicks();
        unsigned drawn=0;
        while(host.Running()) {
            host.Pump(16);
            if(relative && (host.RelativeMouseActive()!=lastRelative || host.RelativeMouseError()!=lastRelativeError)) {
                lastRelative=host.RelativeMouseActive();lastRelativeError=host.RelativeMouseError();
                std::printf("relative requested=%d active=%d error=%s\n",host.RelativeMouseRequested(),lastRelative,lastRelativeError.c_str());
            }
            sysEvent_t event{};
            while(Sys_PollQueuedEvent(&event)) {
                if(event.evType==SE_KEY) {
                    ++metrics.keys;
                    if(event.evValue>=0 && event.evValue<256)metrics.consumerHeld[event.evValue]=event.evValue2;
                    if(metrics.trace)std::printf("key=%d down=%d\n",event.evValue,event.evValue2);
                    if(event.evValue==K_ESCAPE && event.evValue2)host.RequestQuit();
                } else if(event.evType==SE_CHAR) {
                    ++metrics.characters;
                    if(metrics.trace)std::printf("character=%d\n",event.evValue);
                }
            }
            if(!host.Running())break;
            host.Present(host.Focused()?35:45,host.Focused()?65:45,host.Focused()?100:45);
            ++drawn;
            if((frames && drawn>=frames) || (runMilliseconds && SDL_GetTicks()-started>=runMilliseconds))host.RequestQuit();
            if(metrics.trace)std::fflush(stdout);
        }
        unsigned held=0;for(bool down:metrics.consumerHeld)held+=down;
        std::printf("window=closed keys=%u characters=%u mouse=%u resizes=%u focus=%u resets=%u held=%u unsupported_text=%u game=false\n",
            metrics.keys,metrics.characters,metrics.mouse,metrics.resizes,metrics.focus,metrics.resets,held,input.UnsupportedCharacters());
    } catch(const std::exception &error) {
        std::fprintf(stderr,"Window host failed: %s\n",error.what());Sys_ShutdownEvents();return 1;
    }
    Sys_ShutdownEvents();return 0;
}
