// Real SDL event queue -> production window/input adapter -> actual engine queue.
// A small consumer records state instead of invoking CL_KeyEvent/game UI.
#include <linux/client/window/window_host.h>
#include <linux/client/window/engine_input.h>
#include <linux/sys_events.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <universal/com_memory.h>
#include <ui/keycodes.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int checks,failures,resets,mouseCalls,lastX,lastY,lastDx,lastDy;
std::array<bool,256> consumerHeld{};
void check(bool condition,const char *name) {
    ++checks;if(!condition)++failures;
    std::printf("%s: %s\n",condition?"PASS":"FAIL",name);
}
void ResetConsumer(int n) { if(n!=0)std::abort();++resets;consumerHeld.fill(false); }
int MouseConsumer(int x,int y,int dx,int dy) { ++mouseCalls;lastX=x;lastY=y;lastDx=dx;lastDy=dy;return 0; }
class Input : public kisak::window::EngineInput {
public:
    explicit Input(uint32_t shift=5000) : EngineInput(MouseConsumer,ResetConsumer),clockShift(shift) {}
    uint32_t NowMilliseconds() const override { return SDL_GetTicks()+clockShift; }
    void Resize(int w,int h) override { width=w;height=h;++resizeCalls; }
    void OnFocus(bool focused) override { focus=focused;++focusCalls; }
    void OnQuit() override { ++quitCalls; }
    int width=0,height=0,resizeCalls=0,focusCalls=0,quitCalls=0;
    bool focus=false;
    uint32_t clockShift;
};
std::vector<sysEvent_t> Drain() {
    std::vector<sysEvent_t> events;sysEvent_t event{};
    while(Sys_PollQueuedEvent(&event)) {
        if(event.evType==SE_KEY && event.evValue>=0 && event.evValue<256)consumerHeld[event.evValue]=event.evValue2;
        events.push_back(event);
    }
    return events;
}
uint32_t Push(SDL_Event event) { if(SDL_PushEvent(&event)!=1)throw std::runtime_error(SDL_GetError());return event.common.timestamp; }
void WindowEvent(uint32_t id,uint8_t kind,int a=0,int b=0) {
    SDL_Event event{};event.type=SDL_WINDOWEVENT;event.window.windowID=id;event.window.event=kind;
    event.window.data1=a;event.window.data2=b;Push(event);
}
uint32_t Key(uint32_t id,SDL_Scancode scan,bool down,bool repeat=false,uint32_t time=0) {
    SDL_Event event{};event.type=down?SDL_KEYDOWN:SDL_KEYUP;event.key.windowID=id;
    event.key.keysym.scancode=scan;event.key.repeat=repeat;event.key.timestamp=time;return Push(event);
}
void Text(uint32_t id,const char *text) {
    SDL_Event event{};event.type=SDL_TEXTINPUT;event.text.windowID=id;
    std::strncpy(event.text.text,text,sizeof(event.text.text)-1);Push(event);
}
void Button(uint32_t id,uint8_t button,bool down) {
    SDL_Event event{};event.type=down?SDL_MOUSEBUTTONDOWN:SDL_MOUSEBUTTONUP;event.button.windowID=id;
    event.button.button=button;Push(event);
}
}
int main()
{
    Sys_InitializeCriticalSections();
    try {
        check(SDL_InitSubSystem(SDL_INIT_EVENTS)==0,"external SDL event ownership initializes");
        Input input;
        {
            kisak::window::Config config;config.hidden=true;config.width=320;config.height=200;
            kisak::window::WindowHost host(input,config);
            std::printf("SDL driver=%s window=%u\n",SDL_GetCurrentVideoDriver(),host.Id());
            check(host.Id()!=0 && SDL_GetWindowFromID(host.Id())==host.NativeWindow(),"real SDL window created with a valid identity");
            bool secondRejected=false;
            try { kisak::window::WindowHost second(input,config); } catch(const std::logic_error &) { secondRejected=true; }
            check(secondRejected && host.Running(),"second host cannot silently steal the process-wide SDL event queue");
            host.Present(10,20,30);
            check(SDL_GetWindowSurface(host.NativeWindow())!=nullptr,"real software surface presents without game renderer");
            SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);Sys_ShutdownEvents();
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);host.Pump();Drain();
            Key(host.Id(),SDL_SCANCODE_A,true);Key(host.Id(),SDL_SCANCODE_A,true,true);Key(host.Id(),SDL_SCANCODE_A,false);
            host.Pump();auto events=Drain();
            check(events.size()==3 && events[0].evType==SE_KEY && events[0].evValue=='a' && events[0].evValue2
                && events[1].evValue2 && !events[2].evValue2,"physical key repeat and release reach actual engine queue");
            uint32_t timestamp=SDL_GetTicks()+100;
            timestamp=Key(host.Id(),SDL_SCANCODE_F1,true,false,timestamp);host.Pump();events=Drain();
            check(events.size()==1 && uint32_t(events[0].evTime)-timestamp>=4998
                && uint32_t(events[0].evTime)-timestamp<=5002,"SDL timestamps are calibrated into the consumer clock domain");
            Key(host.Id(),SDL_SCANCODE_F1,false);host.Pump();Drain();
            Key(host.Id(),SDL_SCANCODE_LSHIFT,true);Key(host.Id(),SDL_SCANCODE_RSHIFT,true);Key(host.Id(),SDL_SCANCODE_LSHIFT,false);
            host.Pump();events=Drain();
            check(events.size()==1 && events[0].evValue==K_SHIFT && consumerHeld[K_SHIFT],"left/right modifiers retain shared engine key until both release");
            Key(host.Id(),SDL_SCANCODE_RSHIFT,false);host.Pump();events=Drain();
            check(events.size()==1 && !events[0].evValue2 && !consumerHeld[K_SHIFT],"last physical modifier releases shared engine key");
            Key(host.Id(),SDL_SCANCODE_LCTRL,true);Key(host.Id(),SDL_SCANCODE_A,true);Key(host.Id(),SDL_SCANCODE_A,false);Key(host.Id(),SDL_SCANCODE_LCTRL,false);
            host.Pump();events=Drain();
            check(events.size()==5 && events[2].evType==SE_CHAR && events[2].evValue==1,"control editing shortcut reaches legacy character interface");
            Key(host.Id(),SDL_SCANCODE_BACKSPACE,true);Key(host.Id(),SDL_SCANCODE_BACKSPACE,false);host.Pump();events=Drain();
            check(events.size()==3 && events[1].evType==SE_CHAR && events[1].evValue==8,"backspace produces key plus editing character without text-input dependency");
            Text(host.Id(),"A\xc3\xa9\xf0\x9f\x98\x80");host.Pump();events=Drain();
            check(events.size()==2 && events[0].evValue=='A' && events[1].evValue==233 && input.UnsupportedCharacters()==1,
                "UTF-8 decodes to printable Latin-1 and rejects unrepresentable scalar without truncation");
            Text(host.Id(),"\xc0\xaf\xed\xa0\x80");host.Pump();
            check(Drain().empty() && input.UnsupportedCharacters()==4,"overlong UTF-8 and surrogate scalars do not enter byte-oriented engine text");
            SDL_Event malformed{};malformed.type=SDL_TEXTINPUT;malformed.text.windowID=host.Id();
            std::memset(malformed.text.text,'x',sizeof(malformed.text.text));Push(malformed);host.Pump();
            check(Drain().empty(),"unterminated SDL text buffer is bounded and ignored");
            Key(host.Id()+100,SDL_SCANCODE_W,true);Text(host.Id()+100,"foreign");WindowEvent(host.Id()+100,SDL_WINDOWEVENT_CLOSE);host.Pump();
            check(Drain().empty() && host.Running(),"other-window events cannot mutate or close this host");
            SDL_Event motion{};motion.type=SDL_MOUSEMOTION;motion.motion.windowID=host.Id();
            motion.motion.x=30;motion.motion.y=40;motion.motion.xrel=-4;motion.motion.yrel=6;Push(motion);
            Button(host.Id(),SDL_BUTTON_RIGHT,true);Button(host.Id(),SDL_BUTTON_RIGHT,false);host.Pump();events=Drain();
            check(mouseCalls==1 && lastX==30 && lastY==40 && lastDx==-4 && lastDy==6
                && events.size()==2 && events[0].evValue==K_MOUSE2,"mouse coordinates and button mapping reach engine-compatible adapter");
            SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.windowID=host.Id();wheel.wheel.y=-2;wheel.wheel.direction=SDL_MOUSEWHEEL_FLIPPED;
            Push(wheel);host.Pump();events=Drain();
            check(events.size()==4 && events[0].evValue==K_MWHEELUP && events[0].evValue2 && !events[1].evValue2,"flipped wheel emits bounded press-release pairs");
            wheel.wheel.y=INT_MIN;Push(wheel);host.Pump();events=Drain();
            check(events.size()==32 && events[0].evValue==K_MWHEELUP,"extreme wheel count is bounded without signed overflow");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_RESIZED,640,480);WindowEvent(host.Id(),SDL_WINDOWEVENT_SIZE_CHANGED,640,480);
            WindowEvent(host.Id(),SDL_WINDOWEVENT_RESIZED,0,-1);host.Pump();
            check(input.resizeCalls==1 && input.width==640 && input.height==480,"resize duplicates and invalid dimensions do not duplicate state changes");
            Key(host.Id(),SDL_SCANCODE_W,true);Button(host.Id(),SDL_BUTTON_LEFT,true);host.Pump();Drain();
            int before=resets;WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);host.Pump();
            check(resets==before+1 && !consumerHeld['w'] && !consumerHeld[K_MOUSE1] && Drain().empty(),"focus loss clears consumer and stale queued input");
            Key(host.Id(),SDL_SCANCODE_A,true);Text(host.Id(),"background");Push(motion);host.Pump();
            check(Drain().empty() && mouseCalls==1,"unfocused keyboard text and mouse input are ignored");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);Key(host.Id(),SDL_SCANCODE_W,true);host.Pump();Drain();
            Key(host.Id(),SDL_SCANCODE_W,false);host.Pump();
            for(int n=0;n<256;++n)Sys_QueEvent(1,SE_CHAR,'x',0,0,nullptr);
            check(consumerHeld['w'],"fixture delivers key-down then evicts queued key-up through real ring overflow");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);host.Pump();
            check(!consumerHeld['w'] && Drain().empty(),"focus reset recovers delivered key whose release was evicted");
            char *console=static_cast<char *>(std::malloc(7));std::memcpy(console,"status",7);
            Sys_QueEvent(1,SE_CONSOLE,0,0,7,console);Sys_QueEvent(1,SE_KEY,'a',1,0,nullptr);Sys_QueEvent(1,SE_CHAR,'a',0,0,nullptr);
            Sys_DiscardQueuedInputEvents();events=Drain();
            check(events.size()==1 && events[0].evType==SE_CONSOLE && events[0].evPtr==console
                && !std::strcmp(static_cast<char *>(events[0].evPtr),"status"),"input discard preserves console payload ownership and order");
            Z_Free(console,10);
            char *first=static_cast<char *>(std::malloc(2)),*last=static_cast<char *>(std::malloc(2));
            std::memcpy(first,"A",2);std::memcpy(last,"B",2);
            Sys_QueEvent(1,SE_CONSOLE,0,0,2,first);
            for(int n=0;n<254;++n)Sys_QueEvent(1,SE_CHAR,'x',0,0,nullptr);
            Sys_QueEvent(1,SE_CONSOLE,0,0,2,last);
            Sys_DiscardQueuedInputEvents();events=Drain();
            check(events.size()==2 && events[0].evPtr==first && events[1].evPtr==last
                && !std::strcmp(first,"A") && !std::strcmp(last,"B"),
                "full-ring input compaction preserves multiple console payloads in order");
            Z_Free(first,10);Z_Free(last,10);Sys_ShutdownEvents();
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);Key(host.Id(),SDL_SCANCODE_A,true);host.Pump();Drain();
            WindowEvent(host.Id(),SDL_WINDOWEVENT_MINIMIZED);host.Pump();
            check(!host.Focused() && !consumerHeld['a'] && Drain().empty(),"minimize resets input even without a separate focus-loss event");
            bool wrongThread=false;
            std::thread worker([&]{try{host.Pump();}catch(const std::logic_error &){wrongThread=true;}});worker.join();
            check(wrongThread,"event pumping enforces SDL creating-thread ownership");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);host.Pump();
            bool relative=host.SetRelativeMouse(true);
            check((relative && host.RelativeMouseActive()) || (!relative && !host.RelativeMouseError().empty()),
                "relative mouse capture reports active backend or explicit unsupported error");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);host.Pump();
            check(!host.RelativeMouseActive() && host.RelativeMouseRequested(),"focus loss releases capture while retaining user request");
            WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);host.Pump();
            check(!relative || host.RelativeMouseActive(),"supported relative capture resumes when focus returns");
            host.SetRelativeMouse(false);
            check(!host.RelativeMouseActive() && !host.RelativeMouseRequested(),"explicit capture disable clears active and requested state");
            host.SetRelativeMouse(true);
            WindowEvent(host.Id(),SDL_WINDOWEVENT_CLOSE);host.Pump();host.RequestQuit();
            check(!host.Running() && input.quitCalls==1 && !host.RelativeMouseActive() && !SDL_GetRelativeMouseMode(),"window close requests shutdown exactly once");
        }
        check(!SDL_WasInit(SDL_INIT_VIDEO) && SDL_WasInit(SDL_INIT_EVENTS),"window destruction releases owned video and preserves external SDL event lifetime");
        for(int n=0;n<3;++n) {
            Input sink;kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);
            SDL_Event quit{};quit.type=SDL_QUIT;Push(quit);host.Pump(1);
            if(host.Running() || sink.quitCalls!=1)throw std::runtime_error("Repeated window lifecycle did not quit");
        }
        check(!SDL_WasInit(SDL_INIT_VIDEO),"repeated create quit destroy uses real SDL lifecycle without retained video ownership");
        {
            const uint32_t shift=uint32_t(0)-SDL_GetTicks()-10;
            Input wrapped(shift);kisak::window::Config config;config.hidden=true;
            kisak::window::WindowHost host(wrapped,config);
            SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);host.Pump();
            SDL_Delay(20);
            uint32_t timestamp=Key(host.Id(),SDL_SCANCODE_W,true);host.Pump();auto events=Drain();
            int32_t difference=events.empty()?100:int32_t(uint32_t(events[0].evTime)-(timestamp+shift));
            check(uint64_t(timestamp)+shift>UINT32_MAX && events.size()==1 && difference>=-2 && difference<=2,
                "timestamp calibration remains correct across 32-bit clock wrap");
        }
        SDL_QuitSubSystem(SDL_INIT_EVENTS);Sys_ShutdownEvents();
    } catch(const std::exception &error) { std::fprintf(stderr,"test exception: %s\n",error.what());++failures; }
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
