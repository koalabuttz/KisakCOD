// Explicit genuine-asset integration fixture. Requires the privately prepared
// control bundle and image directory; no replacement assets or fabricated UI.
#include <linux/client/window/menu_input_owner.h>
#include <linux/client/window/client_input_host.h>
#include <linux/client/window/window_host.h>
#include <client_mp/client_mp.h>
#include <ui/ui_input_core.h>
#include <ui/ui_registration.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {int checks,failures;void Check(bool value,const char *name){++checks;failures+=!value;std::printf("%s: %s\n",value?"PASS":"FAIL",name);}template<class F>bool Reject(F f){try{f();return false;}catch(const std::exception&){return true;}}}
int main(int argc,char **argv){
 if(argc!=3){std::fprintf(stderr,"requires genuine control bundle and image directory\n");return 2;}
 Sys_InitializeCriticalSections();
 try {
  {
  kisak::window::ClientInputHost input;
  kisak::window::Config config;config.width=640;config.height=480;
  kisak::window::WindowHost window(input,config);kisak::window::ClientInputHost::WindowCursorBinding cursorBinding(input,window);
  kisak::window::CpuRenderer renderer;
  Check(Reject([&]{kisak::window::MenuInputOwner owner(input,renderer,"/nonexistent-menu-input-fixture",argv[2],640,480);}) && renderer.Status()==kisak::window::CpuRenderer::State::stopped && !cls.uiStarted,"missing bundle leaves renderer and borrowed UI readiness unchanged");
  Check(Reject([&]{kisak::window::MenuInputOwner owner(input,renderer,argv[1],"/nonexistent-menu-image-fixture",640,480);}) && renderer.Status()==kisak::window::CpuRenderer::State::stopped && !cls.uiStarted,"failed font atlas initialization rolls back owned renderer generation");
  renderer.Initialize(8,8);
  Check(Reject([&]{kisak::window::MenuInputOwner owner(input,renderer,argv[1],argv[2],640,480);}) && renderer.Pixels().width==8,"live unrelated renderer generation rejected without mutation");renderer.Shutdown();
  for(int cycle=0;cycle<3;++cycle){
   const bool savedLook=Dvar_GetBool("cl_freelook"),savedFilter=Dvar_GetBool("m_filter");
   {
    kisak::window::MenuInputOwner owner(input,renderer,argv[1],argv[2],640,480);
    owner.Present(window);
    auto *paintMenu=owner.Context().Menus[0];bool canonical=paintMenu->itemCount==12;
    for(int n=0;n<paintMenu->itemCount;++n)canonical&=paintMenu->items[n]->parent==paintMenu;
    Check(canonical&&paintMenu->items[0]->type==0&&paintMenu->items[7]->type==0&&paintMenu->items[8]->type==1&&paintMenu->items[9]->type==11&&std::strcmp(paintMenu->items[9]->dvar,"cl_freelook")==0,"one canonical real source-ordered twelve-item graph owns painting and navigation");
    Check(paintMenu->items[0]->window.background&&paintMenu->items[0]->window.background==paintMenu->items[1]->window.background,"actual literal white registration shares owned material identity with static white item");
    owner.Present(window);Check(owner.Context().blurRadiusOut==7,"repeated real menu traversal retains actual blur request without cross-frame accumulation");
    Check(Dvar_GetBool("cl_freelook")==savedLook && Dvar_GetBool("m_filter")==savedFilter,"recreated menu preserves settings only within existing input runtime");
    Check(owner.Opened() && cls.uiStarted && input.MenuOpen(),"real admitted menu opens and presents");
    Check(UI_ReplaceOwnedMenus(&owner.Context(),owner.Context().Menus,1)==UiOwnedMenuRegistration::activeMenus && owner.Opened(),"live menu cannot be reloaded out from under actual input and script state");
    owner.Key(K_DOWNARROW,true);owner.Key(K_DOWNARROW,false);
    Check(owner.FocusIndex()==106 || owner.FocusIndex()==112,"real cursor navigation focuses an exported yes/no item");
    const char *name=owner.FocusIndex()==106?"cl_freelook":"m_filter";const bool before=Dvar_GetBool(name);
    owner.Key(K_ENTER,true);const bool after=Dvar_GetBool(name);owner.Key(K_ENTER,false);
    Check(after!=before && Dvar_GetBool(name)==after,"actual yes/no handler toggles once on key down, not release");
    const bool repeatBefore=Dvar_GetBool(name);
    CL_KeyEvent(0,K_ENTER,1,Sys_Milliseconds());const bool repeatAfter=Dvar_GetBool(name);
    CL_KeyEvent(0,K_ENTER,1,Sys_Milliseconds());
    Check(repeatAfter!=repeatBefore && Dvar_GetBool(name)==repeatAfter && input.Held()==1 && playerKeys[0].keys[K_ENTER].repeats==2,"actual client key-repeat suppresses repeated yes/no toggle while tracking held key");
    CL_KeyEvent(0,K_ENTER,0,Sys_Milliseconds());CL_KeyEvent(0,K_ENTER,1,Sys_Milliseconds());
    Check(Dvar_GetBool(name)==repeatBefore,"fresh press after repeat release toggles again and retains changed setting");
    input.Focus(false);
    Check(input.Held()==0 && !playerKeys[0].keys[K_ENTER].down,"focus loss releases held actual client key before menu teardown");
    owner.ResetFocus();Check(owner.FocusIndex()==-1 && !inHandleKey && !itemCapture && !g_editItem && !g_bindItem,"focus reset clears actual focus and transient capture pointers");
    const bool savedLook=Dvar_GetBool("cl_freelook"),savedFilter=Dvar_GetBool("m_filter");
    CL_KeyEvent(0,K_ESCAPE,1,Sys_Milliseconds());CL_KeyEvent(0,K_ESCAPE,0,Sys_Milliseconds());
    Check(!owner.Opened() && !(clientUIActives[0].keyCatchers&KEYCATCH_UI),"actual client Escape closes menu and releases UI catcher");
    Check(UI_ReplaceOwnedMenus(&owner.Context(),owner.Context().Menus,1)==UiOwnedMenuRegistration::ok && !UILocalVar_Find(&owner.Context().localVars,"ui_hideBack") && owner.Context().openMenuCount==0,"closed real menu reload releases script localvars and resets registration stack");
    owner.Open();owner.Present(window);Check(owner.Opened() && Dvar_GetBool("cl_freelook")==savedLook && Dvar_GetBool("m_filter")==savedFilter,"same owner reopens preserving only live dvar settings");
    CL_KeyEvent(0,K_DOWNARROW,1,Sys_Milliseconds());CL_KeyEvent(0,K_DOWNARROW,0,Sys_Milliseconds());owner.Present(window);
    Check((owner.FocusIndex()==106||owner.FocusIndex()==112) && UILocalVar_Find(&owner.Context().localVars,"ui_highlight") && input.Held()==0,"reloaded registry supports actual client key dispatch and recreates focus-script localvars");
    SDL_Event focus{};focus.type=SDL_WINDOWEVENT;focus.window.windowID=window.Id();focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED;SDL_PushEvent(&focus);
    SDL_Event enter=focus;enter.window.event=SDL_WINDOWEVENT_ENTER;SDL_PushEvent(&enter);window.Pump(0);window.SetEngineCursor(true);
    Check(window.EngineCursorActive(),"same-window teardown fixture owns an active engine cursor lease");
    renderer.BeginFrame();const float black[4]={0,0,0,1};renderer.Clear(black);renderer.Submit();
   }
   Check(!window.EngineCursorActive(),"owner destruction releases cursor while platform window remains alive");
   Check(renderer.Status()==kisak::window::CpuRenderer::State::stopped && !cls.uiStarted && !input.MenuOpen(),"destruction cancels pending frame and detaches/releases renderer generation");
   input.Focus(false);Check(input.Held()==0,"input reset after owner destruction has no dangling callback");
  }
  }
  {kisak::window::ClientInputHost fresh;
   Check(Dvar_GetBool("cl_freelook") && Dvar_FindVar("m_filter")==nullptr,"fresh input runtime resets defaults without loading saved UI settings");}
 }catch(const std::exception &e){std::fprintf(stderr,"fixture failed: %s\n",e.what());return 1;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
