#include "window_host.h"
#include "client_input_host.h"
#include "shutdown_signal.h"
#include "frame_deadline.h"
#include <linux/sys_events.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <ui/keycodes.h>
#include <universal/com_memory.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
struct Metrics {
    unsigned keys=0,characters=0,mouse=0,resizes=0,focus=0,resets=0;
    bool trace=false;

} metrics;
class ShellInput : public kisak::window::ClientInputHost {
public:
    void Mouse(uint32_t time,int x,int y,int dx,int dy) override {
        ++metrics.mouse;
        EngineInput::Mouse(time,x,y,dx,dy);
        if(metrics.trace)std::printf("mouse x=%d y=%d dx=%d dy=%d\n",x,y,dx,dy);
    }
    void Resize(int width,int height) override {
        ClientInputHost::Resize(width,height);++metrics.resizes;
        if(metrics.trace)std::printf("resize %d %d\n",width,height);
    }
    void OnFocus(bool focused) override {
        ++metrics.focus;
        if(metrics.trace) { std::printf("focus %s\n",focused?"gained":"lost");
            if(!focused)std::printf("input=reset held=%d\n",Held()); }
    }
    void OnQuit() override { std::puts("window=closing"); }
};
void PresentDiagnostic(kisak::window::WindowHost &host,kisak::window::PixelBuffer &pixels) {
    const auto extent=host.SurfaceExtent();
    if(pixels.Resize(extent.width,extent.height)) {
        auto bytes=pixels.Bytes();
        for(int y=0;y<extent.height;++y)for(int x=0;x<extent.width;++x) {
            const bool left=x<extent.width/2,top=y<extent.height/2;
            auto *p=bytes.data()+(size_t(y)*extent.width+x)*4;
            p[0]=(top?left:!left)?255:0;
            p[1]=!left?255:0;p[2]=!top && left?255:0;p[3]=255;
        }
        std::printf("presentation=rgba8-diagnostic width=%d height=%d\n",extent.width,extent.height);
    }
    host.Present(pixels.View());
}
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
    bool relative=false,requestFrame=false;
    const char *server=nullptr;
    kisak::window::Config config;
    for(int i=1;i<argc;++i) {
        const std::string argument(argv[i]);
        if(argument=="--help") {
            std::puts("Usage: KisakCOD-client-input [--frames 1..10000] [--run-ms 1..60000] [--trace] [--relative] [--title NAME] [--frame] [--server IPv4:port]\n"
                "Production disconnected input host. Escape or window close exits. Console editing is reported in trace; real local commands and optional read-only discovery; no game frame or renderer.");return 0;
        } else if(argument=="--frames" && i+1<argc) {
            if(!Number(argv[++i],10000,frames))return 2;
        } else if(argument=="--run-ms" && i+1<argc) {
            if(!Number(argv[++i],60000,runMilliseconds))return 2;
        } else if(argument=="--trace")metrics.trace=true;
        else if(argument=="--relative")relative=true;
        else if(argument=="--frame")requestFrame=true;
        else if(argument=="--server" && i+1<argc && !server)server=argv[++i];
        else if(argument=="--title" && i+1<argc && *argv[i+1])config.title=argv[++i];
        else return 2;
    }
    Sys_InitializeCriticalSections();
    try {
        ShellInput input;
        if(requestFrame) { std::string reason;input.RequestFrame(reason);std::fprintf(stderr,"frame=unavailable %s\n",reason.c_str());return 5; }
        // Diagnostic preference only; the reusable platform host does not choose
        // the eventual game backend. SDL window surfaces may otherwise use GPU acceleration.
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"0");
        kisak::window::WindowHost host(input,config);
        kisak::window::ShutdownSignal signals;
        if(server)input.Discover(server);
        kisak::window::PixelBuffer pixels;
        PresentDiagnostic(host,pixels);
        std::printf("window=created driver=%s id=%u title=%s game=false input=engine connection=disconnected frame=unavailable presentation=rgba8-diagnostic\n",
            SDL_GetCurrentVideoDriver(),host.Id(),config.title);
        std::printf("window=ready id=%u driver=%s\n",host.Id(),SDL_GetCurrentVideoDriver());
        if(relative)host.SetRelativeMouse(true);
        bool lastRelative=host.RelativeMouseActive();
        std::string lastRelativeError=host.RelativeMouseError();
        if(relative)std::printf("relative requested=1 active=%d error=%s\n",lastRelative,lastRelativeError.c_str());
        std::fflush(stdout);
        const uint32_t started=SDL_GetTicks();
        unsigned drawn=0;
        kisak::window::FrameDeadline cadence;
        while(host.Running()) {
            host.Pump(cadence.WaitMilliseconds(kisak::window::FrameDeadline::Clock::now()));
            if(int signal=signals.PendingSignal()) { std::printf("signal=%d\n",signal);host.RequestQuit(); }
            if(relative && (host.RelativeMouseActive()!=lastRelative || host.RelativeMouseError()!=lastRelativeError)) {
                lastRelative=host.RelativeMouseActive();lastRelativeError=host.RelativeMouseError();
                std::printf("relative requested=%d active=%d error=%s\n",host.RelativeMouseRequested(),lastRelative,lastRelativeError.c_str());
            }
            sysEvent_t event{};
            while(Sys_PollQueuedEvent(&event)) {
                auto release=[](void *p) { if(p)Z_Free(p,10); };
                std::unique_ptr<void,decltype(release)> payload(event.evPtr,release);
                input.Dispatch(event);
                if(event.evType==SE_KEY) {
                    ++metrics.keys;

                    if(metrics.trace)std::printf("key=%d down=%d\n",event.evValue,event.evValue2);
                    if(event.evValue==K_ESCAPE && event.evValue2)host.RequestQuit();
                } else if(event.evType==SE_CHAR) {
                    ++metrics.characters;
                    if(metrics.trace)std::printf("character=%d\n",event.evValue);
                }
            }
            const bool tick=host.Running() && cadence.TakeTick(kisak::window::FrameDeadline::Clock::now());
            if(tick) {
                if(metrics.trace)std::printf("command_tick=%llu\n",static_cast<unsigned long long>(input.Ticks()+1));
                input.Tick();
            }
            if(tick) {
                const auto sample=input.ConsumeMouse();
                if(metrics.trace && (sample.x || sample.y))std::printf("input_tick mouse_dx=%d mouse_dy=%d frame=unavailable\n",sample.x,sample.y);
                if(metrics.trace)std::printf("editor text=%s cursor=%d held=%d\n",input.Text(),input.Cursor(),input.Held());
                PresentDiagnostic(host,pixels);++drawn;
            }
            if(!host.Running())break;
            if((frames && drawn>=frames) || (runMilliseconds && SDL_GetTicks()-started>=runMilliseconds))host.RequestQuit();
            if(metrics.trace)std::fflush(stdout);
        }
        std::printf("window=closed keys=%u characters=%u mouse=%u resizes=%u focus=%u resets=%u held=%d unsupported_text=%u relative_active=%d presented=%u input=engine frame=unavailable game=false\n",
            metrics.keys,metrics.characters,metrics.mouse,metrics.resizes,metrics.focus,input.Resets(),input.Held(),input.UnsupportedCharacters(),host.RelativeMouseActive(),drawn+1);
    } catch(const std::exception &error) {
        std::fprintf(stderr,"Window host failed: %s\n",error.what());Sys_ShutdownEvents();return 1;
    }
    Sys_ShutdownEvents();return 0;
}
