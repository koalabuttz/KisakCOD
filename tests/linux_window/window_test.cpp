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
#include <string>
#include <thread>
#include <vector>

namespace {
int checks,failures,resets,mouseCalls,lastX,lastY,lastDx,lastDy;
std::array<bool,256> consumerHeld{};
void check(bool condition,const char *name) {
    ++checks;if(!condition)++failures;
    std::printf("%s: %s\n",condition?"PASS":"FAIL",name);
}
void checkTrace(const std::vector<std::string> &actual,const std::vector<std::string> &expected,const char *name) {
    if(actual!=expected) {
        std::printf("trace actual:");
        for(const auto &line:actual)std::printf(" [%s]",line.c_str());
        std::printf("\ntrace expect:");
        for(const auto &line:expected)std::printf(" [%s]",line.c_str());
        std::printf("\n");
    }
    check(actual==expected,name);
}
void ResetConsumer(int n) { if(n!=0)std::abort();++resets;consumerHeld.fill(false); }
int MouseConsumer(int x,int y,int dx,int dy) { ++mouseCalls;lastX=x;lastY=y;lastDx=dx;lastDy=dy;return 0; }
class Input : public kisak::window::EngineInput {
public:
    explicit Input(uint32_t shift=5000) : EngineInput(MouseConsumer,ResetConsumer),clockShift(shift) {}
    // The drain, not the poll, applies motion and pointer boundaries.
    using EngineInput::MotionOf;
    using EngineInput::DeliverMouse;
    using EngineInput::DeliverPointerPresence;
    uint32_t NowMilliseconds() const override { return SDL_GetTicks()+clockShift; }
    void Resize(int w,int h) override { width=w;height=h;++resizeCalls; }
    void OnFocus(bool focused) override { focus=focused;++focusCalls; }
    void OnQuit() override { ++quitCalls; }
    void OnPointerPresence(bool inside) override { pointer=inside;++pointerCalls;if(failPointer)throw std::runtime_error("fixture pointer failure"); }
    bool pointer=false,failPointer=false;int pointerCalls=0;
    int width=0,height=0,resizeCalls=0,focusCalls=0,quitCalls=0;
    bool focus=false;
    uint32_t clockShift;
};
class FaultInput : public Input {
public:
    bool failClock=false,failRelease=false,failQuit=false;
    int releaseAttempts=0,quitAttempts=0;
    uint32_t NowMilliseconds() const override {
        if(failClock)throw std::runtime_error("fixture clock failure");
        return Input::NowMilliseconds();
    }
    void Key(uint32_t time,int key,bool down) override {
        if(!down){++releaseAttempts;if(failRelease)throw std::runtime_error("fixture key release failure");}
        Input::Key(time,key,down);
    }
    void OnQuit() override {
        ++quitAttempts;if(failQuit)throw std::runtime_error("fixture quit failure");
        Input::OnQuit();
    }
};
std::vector<sysEvent_t> Drain() {
    std::vector<sysEvent_t> events;sysEvent_t event{};
    while(Sys_PollQueuedEvent(&event)) {
        if(event.evType==SE_KEY && event.evValue>=0 && event.evValue<256)consumerHeld[event.evValue]=event.evValue2;
        events.push_back(event);
    }
    return events;
}
std::vector<std::string> order;
// Main-equivalent drain: pump, then apply every queued record in the order the
// queue yields it. The trace records what a consumer observed, including the
// pointer state a click landed on, so ordering and target are both visible.
std::vector<sysEvent_t> DrainApplied(Input &input) {
    std::vector<sysEvent_t> events;sysEvent_t event{};
    while(Sys_PollQueuedEvent(&event)) {
        if(event.evType==SE_KEY) {
            if(event.evValue>=0 && event.evValue<256)consumerHeld[event.evValue]=event.evValue2;
            order.push_back("key "+std::to_string(event.evValue)+(event.evValue2?" down ":" up ")
                +std::to_string(lastX)+","+std::to_string(lastY));
        } else if(event.evType==SE_CHAR) {
            order.push_back("char "+std::to_string(event.evValue));
        } else if(event.evType==SE_MOUSE) {
            const auto motion=input.MotionOf(event);
            order.push_back("mouse "+std::to_string(motion.x)+","+std::to_string(motion.y)
                +" d"+std::to_string(motion.dx)+","+std::to_string(motion.dy));
            input.DeliverMouse(motion);
        } else if(event.evType==SE_POINTER) {
            order.push_back(std::string("pointer ")+(event.evValue?"inside":"left"));
            input.DeliverPointerPresence(event.evValue!=0);
        }
        events.push_back(event);
    }
    return events;
}
// SDL can hand a pushed window boundary to a following poll, so a test that
// asserts engine-side order settles the host's pointer state first instead of
// assuming the boundary and the motion beside it land in the same poll.
void SettlePointer(kisak::window::WindowHost &host,Input &input,bool inside) {
    for(int n=0;n<4 && host.PointerInside()!=inside;++n) { host.Pump();DrainApplied(input); }
}
// SDL can hand pushed events to a later poll, so a test that asserts on queue
// contents pumps until two consecutive polls deliver nothing.
std::vector<sysEvent_t> SettleInput(kisak::window::WindowHost &host,Input &input) {
    std::vector<sysEvent_t> events;int quiet=0;
    for(int n=0;n<16 && quiet<2;++n) {
        host.Pump();
        const auto batch=DrainApplied(input);
        quiet=batch.empty()?quiet+1:0;
        events.insert(events.end(),batch.begin(),batch.end());
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
void Motion(uint32_t id,int x,int y,int dx,int dy) {
    SDL_Event event{};event.type=SDL_MOUSEMOTION;event.motion.windowID=id;
    event.motion.x=x;event.motion.y=y;event.motion.xrel=dx;event.motion.yrel=dy;Push(event);
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
            Button(host.Id(),SDL_BUTTON_RIGHT,true);Button(host.Id(),SDL_BUTTON_RIGHT,false);host.Pump();events=DrainApplied(input);
            check(mouseCalls==1 && lastX==30 && lastY==40 && lastDx==-4 && lastDy==6
                && events.size()==3 && events[0].evType==SE_MOUSE && events[1].evValue==K_MOUSE2,"mouse coordinates and button mapping reach engine-compatible adapter");
            // Regression: Pump delivers the whole SDL batch before the main loop
            // drains, so motion applied at poll time overtook a click that arrived
            // before it. Batch motion A, a click on it, then motion B.
            {
                order.clear();const int callsBefore=mouseCalls;
                Motion(host.Id(),100,120,3,5);
                Button(host.Id(),SDL_BUTTON_LEFT,true);Button(host.Id(),SDL_BUTTON_LEFT,false);
                Motion(host.Id(),400,300,300,180);
                host.Pump();
                check(mouseCalls==callsBefore,"motion is deferred while a pumped batch is still arriving");
                const auto batched=DrainApplied(input);
                const std::vector<std::string> expected{
                    "mouse 100,120 d3,5",
                    "key "+std::to_string(K_MOUSE1)+" down 100,120",
                    "key "+std::to_string(K_MOUSE1)+" up 100,120",
                    "mouse 400,300 d300,180"};
                checkTrace(order,expected,"batched motion, click and later motion keep SDL arrival order through the drain");
                check(batched.size()==4 && batched[0].evType==SE_MOUSE && batched[1].evType==SE_KEY
                    && batched[2].evType==SE_KEY && batched[3].evType==SE_MOUSE,
                    "click is delivered against the pointer position it was pressed on, not a later sample");
                order.clear();
                Key(host.Id(),SDL_SCANCODE_SPACE,true);
                Motion(host.Id(),50,60,-7,-8);
                Key(host.Id(),SDL_SCANCODE_SPACE,false);
                host.Pump();
                const auto keyedEvents=DrainApplied(input);
                const std::vector<std::string> keyed{
                    "key "+std::to_string(K_SPACE)+" down 400,300",
                    "mouse 50,60 d-7,-8",
                    "key "+std::to_string(K_SPACE)+" up 50,60"};
                check(order==keyed && keyedEvents.size()==3,"keyboard pressed before motion stays before it in the same batch");
            }
            // Regression: motion must not evict a key transition. A release
            // followed by a full batch of motion and a wheel used to overflow the
            // 256-record ring and drop the release, stranding the held key.
            {
                order.clear();
                Key(host.Id(),SDL_SCANCODE_W,true);host.Pump();DrainApplied(input);
                check(consumerHeld['w'],"fixture delivers key-down before the pressure batch");
                Key(host.Id(),SDL_SCANCODE_W,false);
                for(int n=0;n<254;++n)Motion(host.Id(),200+n,300+n,1,1);
                SDL_Event step{};step.type=SDL_MOUSEWHEEL;step.wheel.windowID=host.Id();step.wheel.y=1;Push(step);
                host.Pump();
                const auto pressured=DrainApplied(input);
                check(!consumerHeld['w'],"key release survives 254 motion samples and a wheel in one batch");
                check(pressured.size()==4 && pressured[0].evType==SE_KEY && !pressured[0].evValue2
                    && pressured[1].evType==SE_MOUSE && pressured[2].evValue==K_MWHEELUP,
                    "release, coalesced motion and wheel stay distinct records in arrival order");
                const auto merged=input.MotionOf(pressured[1]);
                check(merged.x==453 && merged.y==553 && merged.dx==254 && merged.dy==254,
                    "consecutive motion coalesces to the latest position with summed deltas");
            }
            // Regression: motion interleaved with other input must not evict a
            // release either. 86 (motion, wheel) pairs after a release is 259
            // records for a 256-record ring, so admission has to reclaim motion.
            {
                order.clear();
                Key(host.Id(),SDL_SCANCODE_W,true);
                const auto down=SettleInput(host,input);
                check(consumerHeld['w'] && !down.empty(),"fixture delivers key-down before the interleaved batch");
                Key(host.Id(),SDL_SCANCODE_W,false);
                for(int n=0;n<86;++n) {
                    Motion(host.Id(),300+n,400+n,1,1);
                    SDL_Event step{};step.type=SDL_MOUSEWHEEL;step.wheel.windowID=host.Id();step.wheel.y=1;Push(step);
                }
                const auto interleaved=SettleInput(host,input);
                int transitions=0,motions=0;
                for(const auto &record:interleaved) {
                    if(record.evType==SE_KEY)++transitions;
                    else if(record.evType==SE_MOUSE)++motions;
                }
                check(!consumerHeld['w'],"key release survives interleaved motion and wheel pressure");
                check(interleaved.size()==256 && transitions==173 && motions==83
                    && interleaved[0].evType==SE_KEY && !interleaved[0].evValue2,
                    "full ring keeps every key transition and reclaims only queued motion");
            }
            // Regression: a pointer boundary is a transition like any other, so it
            // must not be applied ahead of the motion and click around it. The
            // boundary is settled first because SDL may hand a pushed window event
            // to a following poll.
            {
                WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);host.Pump();DrainApplied(input);
                SettlePointer(host,input,true);order.clear();
                Motion(host.Id(),600,400,2,3);
                Button(host.Id(),SDL_BUTTON_LEFT,true);Button(host.Id(),SDL_BUTTON_LEFT,false);
                WindowEvent(host.Id(),SDL_WINDOWEVENT_LEAVE);
                host.Pump();DrainApplied(input);SettlePointer(host,input,false);
                const std::vector<std::string> leaving{
                    "mouse 600,400 d2,3",
                    "key "+std::to_string(K_MOUSE1)+" down 600,400",
                    "key "+std::to_string(K_MOUSE1)+" up 600,400",
                    "pointer left"};
                checkTrace(order,leaving,"pointer leave is ordered after the motion and click that preceded it");
                order.clear();
                WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);
                host.Pump();DrainApplied(input);SettlePointer(host,input,true);
                Motion(host.Id(),700,500,-2,-3);
                Button(host.Id(),SDL_BUTTON_LEFT,true);Button(host.Id(),SDL_BUTTON_LEFT,false);
                host.Pump();DrainApplied(input);
                const std::vector<std::string> entering{
                    "pointer inside",
                    "mouse 700,500 d-2,-3",
                    "key "+std::to_string(K_MOUSE1)+" down 700,500",
                    "key "+std::to_string(K_MOUSE1)+" up 700,500"};
                checkTrace(order,entering,"pointer entry is ordered before the motion and click that followed it");
            }
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
            const int unfocusedCalls=mouseCalls;
            Key(host.Id(),SDL_SCANCODE_A,true);Text(host.Id(),"background");Push(motion);host.Pump();
            check(Drain().empty() && mouseCalls==unfocusedCalls,"unfocused keyboard text and mouse input are ignored");
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
        {
            FaultInput sink;sink.failClock=true;bool rejected=false;
            try {kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);}
            catch(const std::runtime_error&){rejected=true;}
            check(rejected && !SDL_WasInit(SDL_INIT_VIDEO) && SDL_WasInit(SDL_INIT_EVENTS),"throwing constructor clock rolls back owned SDL subsystem without losing external events");
            sink.failClock=false;
            {kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);}
            check(sink.quitAttempts==1 && !SDL_WasInit(SDL_INIT_VIDEO),"window can be recreated after partial constructor failure");
        }
        {
            FaultInput sink;
            {
                kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);
                SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
                WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);Key(host.Id(),SDL_SCANCODE_A,true);Key(host.Id(),SDL_SCANCODE_B,true);host.Pump();Drain();
                sink.failClock=sink.failRelease=sink.failQuit=true;
            }
            check(sink.releaseAttempts==2 && sink.quitAttempts==1 && !SDL_WasInit(SDL_INIT_VIDEO),"destructor attempts every held release and quit despite callback errors and frees SDL");
            check(!consumerHeld['a'] && !consumerHeld['b'],"final quit reset runs after failed per-key callbacks");
        }
        {
            FaultInput sink;bool propagated=false;
            {
                kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);
                sink.failClock=true;
                try {host.RequestQuit();}catch(const std::runtime_error&){propagated=true;}
                check(propagated && !host.Running(),"explicit quit reports callback failure and stops dispatch");
            }
            check(sink.quitAttempts==1 && !SDL_WasInit(SDL_INIT_VIDEO),"failed explicit quit still completes sink cleanup during destruction");
            Input fresh;kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(fresh,config);
            check(host.Running(),"singleton remains reusable after throwing teardown");
        }
        {
            // Keep video alive around host destruction to inspect restored global state.
            if(SDL_InitSubSystem(SDL_INIT_VIDEO)<0)throw std::runtime_error(SDL_GetError());
            SDL_ShowCursor(SDL_ENABLE);
            Input sink;
            {
                kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(sink,config);
                SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
                WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);host.Pump();DrainApplied(sink);
                host.SetEngineCursor(true);
                check(host.PointerInside() && sink.pointer && host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_DISABLE,"engine cursor hides native cursor only in focused pointer-owning window");
                host.NativeCursorRequest(false);
                check(!host.NativeCursorRequested() && host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_DISABLE,"legacy cursor request cannot override presented custom cursor ownership");
                Key(host.Id(),SDL_SCANCODE_A,true);host.Pump();Drain();const int priorResets=resets;
                WindowEvent(host.Id()+1,SDL_WINDOWEVENT_LEAVE);host.Pump();
                check(host.PointerInside() && host.EngineCursorActive(),"foreign pointer boundary cannot release cursor ownership");
                WindowEvent(host.Id(),SDL_WINDOWEVENT_LEAVE);host.Pump();DrainApplied(sink);
                check(!host.PointerInside() && !sink.pointer && !host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE && consumerHeld['a'] && resets==priorResets,"pointer leave restores native cursor without releasing held keys or keyboard focus");
                WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);host.Pump();DrainApplied(sink);
                check(host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_DISABLE,"pointer reentry reacquires requested engine cursor");
                WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);host.Pump();
                check(!host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE && !consumerHeld['a'],"focus loss restores cursor and still resets actual input consumer");
                WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);host.Pump();DrainApplied(sink);
                check(host.EngineCursorActive(),"focus gain reacquires engine cursor when pointer remains inside");
                host.SetEngineCursor(false);host.NativeCursorRequest(false);
                check(SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE && !host.EngineCursorActive(),"disconnected no-UI hide request records intent while keeping editor native cursor visible");
                host.SetEngineCursor(true);
                bool conflict=false;try{host.SetRelativeMouse(true);}catch(const std::logic_error&){conflict=true;}
                check(conflict && !host.RelativeMouseRequested(),"relative capture cannot silently override custom cursor ownership");
                WindowEvent(host.Id(),SDL_WINDOWEVENT_LEAVE);host.Pump();DrainApplied(sink);
                sink.failPointer=true;WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);bool rejected=false;
                // The boundary is queued, so the poll cannot run the consumer: a
                // failing callback surfaces when the ordered queue applies it.
                host.Pump();
                check(host.PointerInside() && !rejected,"pointer boundary is queued instead of run by the poll");
                try{DrainApplied(sink);}catch(const std::runtime_error&){rejected=true;}
                check(rejected,"throwing pointer callback reports failure when the queue applies it");
                sink.failPointer=false;host.SetEngineCursor(false);
                check(!host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,"cursor ownership stays with the window when a consumer callback fails");
                sink.failPointer=false;host.SetEngineCursor(true);
                host.RequestQuit();
                check(!host.EngineCursorActive() && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,"explicit close releases native cursor before sink teardown");
            }
            SDL_ShowCursor(SDL_DISABLE);
            {
                Input fresh;kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(fresh,config);
                SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);host.Pump();
                host.NativeCursorRequest(false);host.SetEngineCursor(true);
            }
            check(SDL_ShowCursor(SDL_QUERY)==SDL_DISABLE,"destruction restores a previously hidden external native cursor");
            SDL_ShowCursor(SDL_ENABLE);
            {
                FaultInput fresh;kisak::window::Config config;config.hidden=true;kisak::window::WindowHost host(fresh,config);
                SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);WindowEvent(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);WindowEvent(host.Id(),SDL_WINDOWEVENT_ENTER);host.Pump();host.SetEngineCursor(true);fresh.failQuit=true;
            }
            check(SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,"throwing destructor sink cannot strand hidden native cursor");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        SDL_QuitSubSystem(SDL_INIT_EVENTS);Sys_ShutdownEvents();
    } catch(const std::exception &error) { std::fprintf(stderr,"test exception: %s\n",error.what());++failures; }
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
