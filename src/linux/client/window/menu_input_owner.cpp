#include "menu_input_owner.h"
#include "client_input_host.h"
#include "menu_assets.h"
#include "menu_composition_assets.h"
#include <ui/ui_cursor.h>
#include <ui/ui_time.h>
#include <ui/ui_registration.h>
#include "ui_audio_assets.h"
#include "window_host.h"
#include <ui/ui_input_core.h>
#include <ui/ui_expression_text.h>
#include <client_mp/client_mp.h>
#include <qcommon/qcommon.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>
const dvar_t *uiscript_debug;
const dvar_t *ui_showMenuOnly;
const dvar_t *ui_showList;
namespace kisak::window {
namespace { MenuInputOwner *current=nullptr; }
MenuInputOwner &MenuInputOwner::Current(){if(!current)throw std::logic_error("menu services unavailable");current->CheckThread();return *current;}
void MenuInputOwner::CheckThread() const {if(thread_!=std::this_thread::get_id())throw std::logic_error("menu owner thread mismatch");}
int MenuInputOwner::Play(void *context,const char *alias){auto &owner=*static_cast<MenuInputOwner*>(context);const int id=owner.audio_.Play(alias);std::printf("menu_sound alias=%s id=%d driver=%s\n",alias,id,SDL_GetCurrentAudioDriver());return id;}
MenuInputOwner::MenuInputOwner(ClientInputHost &input,CpuRenderer &renderer,const std::string &bundle,const std::string &images,int width,int height)
 : input_(input),renderer_(renderer),scripts_(this,Play),assets_(LoadMenuControlAsset(bundle)),thread_(std::this_thread::get_id()) {
    if(current)throw std::logic_error("one owned menu service per process");
    if(renderer_.Status()!=CpuRenderer::State::stopped)throw std::logic_error("Menu input owner requires an exclusively stopped renderer");
    try {
    // Complete resource/script admission precedes the first menu action.
    LoadMenuAudio(audio_,bundle);
    renderer_.Initialize(width,height);
    fonts_=LoadMenuFonts(renderer_,bundle,images);
    composition_=std::make_unique<MenuCompositionAssets>(LoadMenuComposition(renderer_,bundle,images));
    if(composition_->items.size()!=8)throw std::invalid_argument("Traversal requires eight admitted background items");
    for(size_t n=0;n<8;++n){auto &entry=*composition_->items[n];entry.item.parent=&assets_->menu;paintItems_[n]=&entry.item;paintMaterials_[n]={entry.materialName,entry.material,&entry.item};}
    for(size_t n=0;n<assets_->items.size();++n)paintItems_[n+8]=&assets_->items[n];
    assets_->menu.items=paintItems_.data();assets_->menu.itemCount=paintItems_.size();
    auto admit=[&](const char *&script){if(script&&*script)script=scripts_.RegisterScript(script);};
    admit(assets_->menu.onOpen);admit(assets_->menu.onClose);admit(assets_->menu.onESC);
    for(auto &item:assets_->items){admit(item.action);admit(item.onFocus);admit(item.leaveFocus);}
    dc_.localClientNum=0;dc_.isCursorVisible=false;
    menuDef_t *registered=&assets_->menu;
    if(UI_ReplaceOwnedMenus(&dc_,&registered,1)!=UiOwnedMenuRegistration::ok)
        throw std::logic_error("Owned menu context registration failed");
    Dvar_RegisterBool("m_filter",false,DVAR_ARCHIVE,"Allow mouse movement smoothing");
    ui_showMenuOnly=Dvar_RegisterString("ui_showMenuOnly","",DVAR_NOFLAG,"Restrict admitted menu painting by name");
    ui_showList=Dvar_RegisterBool("ui_showList",false,DVAR_NOFLAG,"UI visible-list diagnostics (unsupported in offline frame)");
    uiscript_debug=Dvar_RegisterInt("uiscript_debug",0,0,2,DVAR_NOFLAG,"UI script diagnostic level");
    Resize(width,height);current=this;
    input_.AttachMenu(*this);attached_=true;Open();
    } catch(...){ResetTransient();if(attached_){try {input_.DetachMenu(*this);}catch(...){}attached_=false;}UI_ShutdownOwnedMenuContext(&dc_);if(current==this)current=nullptr;uiscript_debug=nullptr;ui_showMenuOnly=nullptr;ui_showList=nullptr;renderer_.Shutdown();throw;}
    std::printf("menu_controls=ready source=options_look items=105,106,111,112 audio_driver=%s blur=unsupported frame=unavailable\n",SDL_GetCurrentAudioDriver());
}
MenuInputOwner::~MenuInputOwner(){
    try {input_.ReleaseEngineCursor();}catch(const std::exception &e){std::fprintf(stderr,"menu cursor teardown: %s\n",e.what());}
    try {if(attached_){attached_=false;input_.DetachMenu(*this);}Close();}
    catch(const std::exception&e){std::fprintf(stderr,"menu teardown: %s\n",e.what());}
    ResetTransient();UI_ShutdownOwnedMenuContext(&dc_);if(current==this)current=nullptr;uiscript_debug=nullptr;ui_showMenuOnly=nullptr;ui_showList=nullptr;renderer_.Shutdown();
}
void MenuInputOwner::ResetTransient() noexcept {
    inHandleKey=0;g_waitingForKey=0;g_editingField=0;g_editItem=nullptr;g_bindItem=nullptr;itemCapture=nullptr;captureFunc=nullptr;captureData=nullptr;scrollInfo={};
}
bool MenuInputOwner::Opened() const {return assets_ && Window_IsVisible(0,&assets_->menu.window) && Menus_MenuIsInStack(const_cast<UiContext*>(&dc_),const_cast<menuDef_t*>(&assets_->menu));}
void MenuInputOwner::Open(){CheckThread();if(Opened())return;ResetTransient();
    const auto prior=scrPlaceView[0];scrPlaceView[0]=placement_;
    try {Menus_Open(&dc_,&assets_->menu);}catch(...){scrPlaceView[0]=prior;ResetTransient();throw;}
    scrPlaceView[0]=prior;input_.SyncMenuCapture();Trace("open");
}
void MenuInputOwner::Close(){CheckThread();input_.ReleaseEngineCursor();if(!Opened())return;Menu_ClearFocus(&dc_,&assets_->menu);Menus_Close(&dc_,&assets_->menu);ResetTransient();if(attached_)input_.SyncMenuCapture();Trace("close");}
void MenuInputOwner::ResetFocus(){CheckThread();focused_=false;dc_.isCursorVisible=false;input_.ReleaseEngineCursor();Menu_ClearFocus(&dc_,&assets_->menu);ResetTransient();Trace("reset");}
void MenuInputOwner::Focus(bool focused){CheckThread();focused_=focused;if(!focused)ResetFocus();}
void MenuInputOwner::PointerPresence(bool inside){CheckThread();pointerInside_=inside;if(!inside){dc_.isCursorVisible=false;input_.ReleaseEngineCursor();}}
bool MenuInputOwner::CursorVisible() const {return Opened()&&focused_&&pointerInside_&&dc_.isCursorVisible&&!Dvar_GetBool("cl_bypassMouseInput");}
void MenuInputOwner::Resize(int width,int height){CheckThread();if(width<1||height<1||width>16384||height>16384)throw std::invalid_argument("Invalid menu viewport");width_=width;height_=height;
    ScrPlace_SetupFloatViewportForDisplay(&placement_,0,0,float(width),float(height),1,width,height);
    for(auto &item:assets_->items)item.textRect[0].w=0; // real extent cache invalidated by viewport change
}
void MenuInputOwner::Key(int key,bool down){CheckThread();if(!Opened())return;
    if(key&K_CHAR_FLAG)return; // admitted controls have no text fields or on-key scripts
    switch(key){case K_TAB:case K_DOWNARROW:case K_UPARROW:case K_LEFTARROW:case K_RIGHTARROW:case K_KP_DOWNARROW:case K_KP_UPARROW:case K_ENTER:case K_KP_ENTER:case K_ESCAPE:case K_MOUSE1:case K_MOUSE2:case K_MOUSE3:case K_MWHEELUP:case K_MWHEELDOWN:case K_PGUP:case K_PGDN:break;default:return;}
    const auto prior=scrPlaceView[0];scrPlaceView[0]=placement_;
    try {Menu_HandleKey(&dc_,&assets_->menu,key,down);}
    catch(...){scrPlaceView[0]=prior;ResetTransient();std::fprintf(stderr,"menu action failed: cl_freelook=%d m_filter=%d (actual state retained)\n",Dvar_GetBool("cl_freelook"),Dvar_GetBool("m_filter"));throw;}
    scrPlaceView[0]=prior;if(!Opened()){Menu_ClearFocus(&dc_,&assets_->menu);ResetTransient();}Trace("key");
}
void MenuInputOwner::Mouse(int x,int y){CheckThread();if(!Opened())return;
    pointerX_=x;pointerY_=y;
    UI_UpdateCursorPosition(&dc_,&placement_,x,y);
    if(!focused_ || !pointerInside_)dc_.isCursorVisible=false;
    if(!dc_.isCursorVisible){Trace("mouse");return;}
    const auto prior=scrPlaceView[0];scrPlaceView[0]=placement_;
    try {Menu_HandleMouseMove(&dc_,&assets_->menu);}catch(...){scrPlaceView[0]=prior;ResetTransient();throw;}scrPlaceView[0]=prior;Trace("mouse");
}

int MenuInputOwner::FocusIndex() const {for(size_t i=0;i<assets_->items.size();++i)if(Window_HasFocus(0,&assets_->items[i].window))return assets_->sourceIndices[i];return -1;}
void MenuInputOwner::Trace(const char *phase) {
    // Logging must not allocate or throw between a UI action and capture sync.
    const TraceState state{true,Opened(),FocusIndex(),Dvar_GetBool("cl_freelook"),
        Dvar_GetBool("m_filter"),width_,height_,input_.Held(),pointerX_,pointerY_,CursorVisible(),pointerInside_,focused_,dc_.cursor.x,dc_.cursor.y,paintResult_.requestedBlur,paintResult_.painted,paintResult_.postprocessUnsupported};
    auto &cache=std::strcmp(phase,"present")==0?lastPresentTrace_:lastTrace_;
    if(state!=cache) {
        cache=state;
        std::printf("menu_state phase=%s open=%d focus=%d cl_freelook=%d m_filter=%d width=%d height=%d held=%d pointer_x=%d pointer_y=%d cursor_visible=%d pointer_inside=%d window_focused=%d cursor_x=%g cursor_y=%g requested_blur=%g traversal_painted=%d blur_unrendered=%d\n",
            phase,state.open,state.focus,state.freelook,state.filter,state.width,state.height,state.held,state.pointerX,state.pointerY,state.visible,state.pointerInside,state.windowFocused,state.cursorX,state.cursorY,state.requestedBlur,state.painted,state.blurUnrendered);
        if(std::strcmp(phase,"present")==0)std::printf("menu_traversal painted=%d item_count=%d blur_requested=%g blur_rendered=0 compositor=partial\n",state.painted,assets_->menu.itemCount,state.requestedBlur);
        if(std::strcmp(phase,"present")==0)std::printf("menu_frame scheduler=Menu_PaintAll registered=%d open=%d commands_emitted=%d blur_unrendered=%g\n",dc_.menuCount,dc_.openMenuCount,state.painted,state.requestedBlur);
        std::fflush(stdout);
    }
}
void MenuInputOwner::Present(WindowHost &window){CheckThread();if(renderer_.Status()!=CpuRenderer::State::ready)throw std::logic_error("Menu frame already active");const auto extent=window.SurfaceExtent();if(extent.width!=width_||extent.height!=height_)Resize(extent.width,extent.height);
    const auto before=renderer_.Pixels();if(before.width!=width_||before.height!=height_)renderer_.Resize(width_,height_);
    focused_=window.Focused();pointerInside_=window.PointerInside();
    if(focused_&&pointerInside_)SDL_GetMouseState(&pointerX_,&pointerY_);
    UI_UpdateCursorPosition(&dc_,&placement_,pointerX_,pointerY_);
    if(!focused_||!pointerInside_)dc_.isCursorVisible=false;
    const auto beforeMenu=assets_->menu;const auto beforeBlur=dc_.blurRadiusOut;const auto beforeResult=paintResult_;
    std::array<itemDef_s,12> beforeItems{};for(size_t n=0;n<paintItems_.size();++n)beforeItems[n]=*paintItems_[n];
    UI_UpdateContextTime(&dc_,&timeIndex_,previousTimes_,Sys_Milliseconds());
    auto frame=renderer_.BeginScopedFrame(window);
    try {
        const float clear[4]={.25f,.5f,.75f,1};renderer_.Clear(clear);renderer_.SetViewport(0,0,width_,height_);
        paintResult_=renderer_.DrawMenuFrame(fonts_,placement_,dc_,paintMaterials_);
        if(CursorVisible())renderer_.DrawCursor(composition_->cursor,placement_,placement_,dc_);
        frame.Finish();
    } catch(...) {
        // An SDL failure may occur after CPU replay. Keep the resources/pixels,
        // discard the failed frame and let a later tick rebuild it normally.
        for(size_t n=0;n<paintItems_.size();++n)*paintItems_[n]=beforeItems[n];
        assets_->menu=beforeMenu;dc_.blurRadiusOut=beforeBlur;paintResult_=beforeResult;
        frame.Cancel();
        try {input_.ReleaseEngineCursor();}catch(const std::exception &e){std::fprintf(stderr,"menu cursor error cleanup: %s\n",e.what());}
        throw;
    }
    window.SetEngineCursor(CursorVisible());
    Trace("present");
}
const char *MenuInputOwner::Localize(const char *key) const {CheckThread();return assets_->Localize(key);}
}
char *UI_SafeTranslateString(const char *key){return const_cast<char*>(kisak::window::MenuInputOwner::Current().Localize(key));}
Operand *EvaluateExpression(int,const statement_s *statement,Operand *result){if(!UI_TryLiteralExpression(statement,result))throw std::invalid_argument("Menu expression outside admitted literal subset");return result;}
