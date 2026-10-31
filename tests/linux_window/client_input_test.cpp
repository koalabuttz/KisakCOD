// Real SDL -> engine queue -> CL input consumers, with real Cmd/Cbuf dispatch.
// No game frame or connected state is fabricated.
#include <linux/client/window/client_input_host.h>
#include <linux/sys_events.h>
#include <linux/client/window/menu_input_client.h>
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <ui/keycodes.h>
#include <qcommon/cmd.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>
extern CmdText cmd_textArray[1];

namespace {
struct ThrowingMenu final : kisak::window::MenuInputClient {
    bool failOpened=false,failReset=false;int resets=0;
    void Key(int,bool) override {} void Mouse(int,int) override {}
    void Open() override {} void Close() override {} void Resize(int,int) override {}
    void ResetFocus() override { ++resets;if(failReset)throw std::runtime_error("fixture reset failure"); }
    bool Opened() const override { if(failOpened)throw std::runtime_error("fixture opened failure");return true; }
};
int checks, failures;

void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
void Push(SDL_Event event) { if(SDL_PushEvent(&event)!=1)throw std::runtime_error(SDL_GetError()); }
void Window(uint32_t id,int type,int a=0,int b=0) { SDL_Event e{};e.type=SDL_WINDOWEVENT;e.window.windowID=id;e.window.event=type;e.window.data1=a;e.window.data2=b;Push(e); }
void Key(uint32_t id,SDL_Scancode scan,bool down,bool repeat=false) { SDL_Event e{};e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.windowID=id;e.key.keysym.scancode=scan;e.key.repeat=repeat;Push(e); }
void Text(uint32_t id,const char *text) { SDL_Event e{};e.type=SDL_TEXTINPUT;e.text.windowID=id;std::strncpy(e.text.text,text,sizeof(e.text.text)-1);Push(e); }
void Drain(kisak::window::ClientInputHost &input) { sysEvent_t e{};while(Sys_PollQueuedEvent(&e))input.Dispatch(e); }
}
int main() {
    Sys_InitializeCriticalSections();
    try {
        Check(!std::is_copy_constructible_v<kisak::window::ClientInputHost> && !std::is_move_constructible_v<kisak::window::ClientInputHost>,"input ownership cannot be copied or moved");
        for(int cycle=0;cycle<2;++cycle) {
            kisak::window::ClientInputHost input;
            kisak::window::WindowHost host(input,{});
            {
                kisak::window::ClientInputHost::WindowCursorBinding temporary(input,host);
                CL_GetInputServices().ShowCursor(false);
                bool duplicate=false;try{kisak::window::ClientInputHost::WindowCursorBinding other(input,host);}catch(const std::logic_error&){duplicate=true;}
                Check(duplicate && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,"cursor service has one explicit binding and disconnected visibility policy");
            }
            bool unbound=false;try{CL_GetInputServices().ShowCursor(false);}catch(const std::logic_error&){unbound=true;}
            Check(unbound,"cursor binding destruction removes borrowed window service");
            kisak::window::ClientInputHost::WindowCursorBinding cursorBinding(input,host);
            auto pump=[&] { host.Pump(1);Drain(input); };
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);pump();
            Check(clientUIActives[0].connectionState==CA_DISCONNECTED && !cls.uiStarted && !clientUIActives[0].isRunning && !playerKeys[0].keys['b'].binding,"real input starts disconnected with game UI/frame uninitialized");
            bool rejected=false;try { kisak::window::ClientInputHost duplicate; }catch(const std::logic_error&) { rejected=true; }
            Check(rejected,"second input owner rejected");
            Key(host.Id(),SDL_SCANCODE_A,true);Key(host.Id(),SDL_SCANCODE_A,true,true);pump();
            Check(input.Held()==1 && playerKeys[0].keys['a'].repeats==2,"actual key consumer accounts repeat once");
            Text(host.Id(),"ab");pump();
            Check(!std::strcmp(input.Text(),"ab") && input.Cursor()==2,"actual character consumer edits production field");
            Key(host.Id(),SDL_SCANCODE_BACKSPACE,true);Key(host.Id(),SDL_SCANCODE_BACKSPACE,false);pump();
            Check(!std::strcmp(input.Text(),"a") && input.Cursor()==1,"actual backspace edits field");
            Key(host.Id(),SDL_SCANCODE_HOME,true);Key(host.Id(),SDL_SCANCODE_HOME,false);Text(host.Id(),"z");pump();
            Check(!std::strcmp(input.Text(),"za") && input.Cursor()==1,"actual field key and insertion semantics");
            Window(host.Id(),SDL_WINDOWEVENT_SIZE_CHANGED,1,1);pump();
            Check(g_consoleField.widthInPixels==8 && g_consoleField.scroll<=g_consoleField.cursor && g_consoleField.drawWidth<=1,"tiny resize uses bounded explicit terminal layout");
            SDL_Event mouse{};mouse.type=SDL_MOUSEMOTION;mouse.motion.windowID=host.Id();mouse.motion.xrel=7;mouse.motion.yrel=-4;Push(mouse);pump();
            Check(clients[0].mouseDx[0]==7 && clients[0].mouseDy[0]==-4,"actual CL_MouseEvent accumulates deltas");
            Check(!host.NativeCursorRequested() && SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,"actual disconnected CL_MouseEvent hide request uses bound editor cursor policy");
            const auto sample=input.ConsumeMouse();
            Check(sample.x==7 && sample.y==-4 && input.ConsumeMouse().x==0 && clients[0].mouseDy[0]==0,"input tick consumes real mouse deltas without a game frame");
            Push(mouse);pump();
            Cbuf_AddText(0,"+attack 97 100\n");input.Tick();int buttons=0;CL_UpdateCmdButton(0,&buttons,14,1);
            Check(buttons==1,"real command executor invokes IN_Attack_Down before reset");
            Cbuf_AddText(0,"-attack 97 100;+attack 97 100\n");input.Tick(); // leave fresh pulse unconsumed
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);pump();buttons=0;CL_UpdateCmdButton(0,&buttons,14,1);
            Check(input.Held()==0 && !playerKeys[0].keys['a'].down && buttons==0 && clients[0].mouseDx[0]==0 && clients[0].mouseDy[0]==0,"focus reset clears real keys, action active/pulse and mouse history");
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);Key(host.Id(),SDL_SCANCODE_A,false);Key(host.Id(),SDL_SCANCODE_A,true);pump();
            Check(input.Held()==1 && playerKeys[0].keys['a'].repeats==1,"fresh key works after reset");
            Key(host.Id(),SDL_SCANCODE_A,false);host.Pump(1); // queued up deliberately not consumed
            for(int i=0;i<256;++i)Sys_QueEvent(1,SE_CHAR,'x',0,0,nullptr);
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);pump();
            Check(input.Held()==0 && !playerKeys[0].keys['a'].down && !std::strcmp(input.Text(),"za"),"overflowed key release and stale text recovered by focus boundary");
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);pump();
            auto unsupported=input.Unsupported();Key(host.Id(),SDL_SCANCODE_RETURN,true);Key(host.Id(),SDL_SCANCODE_RETURN,false);pump();
            Check(input.Unsupported()==unsupported && !*input.Text() && cmd_textArray[0].cmdsize>0,"console submission enters production command buffer");
            // Disconnected down events remain console editing; real release binding
            // semantics still enqueue the matching command without fake admission.
            input.Tick();
            Cbuf_AddText(0,"bind b \"+attack\"\n");input.Tick();unsupported=input.Unsupported();
            Key(host.Id(),SDL_SCANCODE_B,true);Key(host.Id(),SDL_SCANCODE_B,false);pump();
            Check(input.Unsupported()==unsupported && cmd_textArray[0].cmdsize>0 && clientUIActives[0].connectionState==CA_DISCONNECTED,"real release binding queues command without fake admission");
            input.Tick();
            Cbuf_AddText(0,"wait 20;+attack 97 100\n");input.Tick();
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_LOST);pump();input.Tick();buttons=0;CL_UpdateCmdButton(0,&buttons,14,1);
            Check(buttons==0 && cmd_textArray[0].cmdsize==0,"focus cancels wait-delayed actions before they can reactivate input");
            Window(host.Id(),SDL_WINDOWEVENT_FOCUS_GAINED);pump();
            Cbuf_AddText(0,"+attack 97 100\n");input.Tick();buttons=0;CL_UpdateCmdButton(0,&buttons,14,1);
            Check(buttons==1,"focus reset clears wait state so fresh commands execute");
            std::string reason;Check(!input.RequestFrame(reason) && reason.find("CL_Frame unavailable")!=std::string::npos,"frame request names unavailable production dependencies");
            Key(host.Id(),SDL_SCANCODE_W,true);pump();host.RequestQuit();
            Check(input.Held()==0 && !host.Running(),"quit clears actual held input before teardown");
            const int originalCatchers=clientUIActives[0].keyCatchers;
            clientUIActives[0].keyCatchers|=KEYCATCH_UI; // preserve preexisting capture even on failed detach
            const bool priorStarted=cls.uiStarted;const int priorCatchers=clientUIActives[0].keyCatchers;
            ThrowingMenu menu;menu.failOpened=true;bool failedAttach=false;
            try {input.AttachMenu(menu);}catch(const std::runtime_error&){failedAttach=true;}
            Check(failedAttach && cls.uiStarted==priorStarted && clientUIActives[0].keyCatchers==priorCatchers,"failed attachment restores UI readiness and capture");
            menu.failOpened=false;input.AttachMenu(menu);menu.failReset=true;bool failedDetach=false;
            try {input.DetachMenu(menu);}catch(const std::runtime_error&){failedDetach=true;}
            Check(failedDetach && cls.uiStarted==priorStarted && clientUIActives[0].keyCatchers==priorCatchers,"failed reset detaches owner and restores prior capture while reporting error");
            const int resetCalls=menu.resets;input.Focus(false);
            Check(menu.resets==resetCalls,"later input reset cannot call failed detached owner");
            menu.failReset=false;input.AttachMenu(menu);input.DetachMenu(menu);
            Check(cls.uiStarted==priorStarted && clientUIActives[0].keyCatchers==priorCatchers,"fresh attachment works after failed attach and detach");
            clientUIActives[0].keyCatchers=originalCatchers;

        }
        bool unavailable=false;try { CL_GetInputServices(); }catch(const std::logic_error&) { unavailable=true; }
        Check(unavailable,"provider unavailable outside scoped host lifetime");
    } catch(const std::exception &e) { std::fprintf(stderr,"input test failed: %s\n",e.what());++failures; }
    Sys_ShutdownEvents();std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
