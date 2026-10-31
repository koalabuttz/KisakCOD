#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <ui/ui_font_selection.h>
#include <client/client.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace kisak::window {
namespace {
struct ActiveUi { CpuRenderer *renderer=nullptr;CpuRenderer::UiMaterialHandle handle=0;Material *expected=nullptr; };
thread_local ActiveUi active;
struct ActiveText { CpuRenderer *renderer=nullptr;CpuRenderer::FontHandle handle=0;Font_s *expected=nullptr;std::string_view text;bool menu=false; };
thread_local ActiveText activeText;
thread_local Font_s *menuFont=nullptr;
struct MenuSelection { const CachedAssets_t *assets=nullptr;const dvar_s *small=nullptr,*big=nullptr,*extra=nullptr; };
thread_local MenuSelection menuSelection;
}
class CpuUiBridge {
public:
    static void RecordText(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                           float rotation,const float *color,int style,int cursorPos) {
        if(!activeText.renderer || font!=activeText.expected || (!activeText.menu && text!=activeText.text.data()))throw std::logic_error("Text entry requires scoped owned font/text");
        std::string_view line=activeText.text;
        if(activeText.menu) {
            const auto length=strnlen(text,1025);
            if(length>1024)throw std::length_error("Unbounded wrapped line");
            line={text,length};maxChars=std::min(maxChars,1024);
        }
        activeText.renderer->RecordText(activeText.handle,line,maxChars,x,y,xScale,yScale,rotation,color,style,cursorPos);
    }
    static void Record(float x,float y,float w,float h,float s0,float t0,float s1,float t1,const float *color,Material *material) {
        if(!active.renderer || material!=active.expected)throw std::logic_error("UI renderer entry requires scoped owned material");
        active.renderer->RecordUiStretchPic(active.handle,x,y,w,h,s0,t0,s1,t1,color);
    }
};
void CpuRenderer::DrawUiImage(UiMaterialHandle material,const ScreenPlacement &placement,
                             float x,float y,float width,float height,int horizontal,int vertical,const float *color) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("UI scope requires exclusive recording renderer");
    auto found=uiMaterials_.find(material);if(found==uiMaterials_.end())throw std::invalid_argument("Stale UI material");
    for(float value:{x,y,width,height})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("Invalid UI geometry");
    if(horizontal<0 || horizontal>7 || vertical<0 || vertical>7)throw std::invalid_argument("Unsupported UI alignment");
    active={this,material,&found->second->value};
    // Standalone host errors throw. Explicit cleanup supports that contract;
    // no claim of unwinding an engine Com_Error longjmp through this host.
    try { UI_DrawHandlePic(&placement,x,y,width,height,horizontal,vertical,color,active.expected); }
    catch(...) {active={};throw;}
    active={};
}
void CpuRenderer::DrawUiText(FontHandle font,const ScreenPlacement &placement,std::string_view text,int maxChars,
                             float x,float y,int horizontal,int vertical,float scale,const float *color,int style) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Text scope requires exclusive recording renderer");
    auto found=fonts_.find(font);if(found==fonts_.end())throw std::invalid_argument("Stale font handle");
    if(text.size()>1024 || text.find('\0')!=std::string_view::npos)throw std::invalid_argument("Invalid bounded text input");
    for(float value:{x,y,scale})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("Invalid UI text geometry");
    if(scale<=0 || horizontal<0 || horizontal>7 || vertical<0 || vertical>7)throw std::invalid_argument("Invalid UI text scale/alignment");
    std::string ownedText(text);
    activeText={this,font,&found->second->value,ownedText};
    try { UI_DrawText(&placement,ownedText.c_str(),maxChars,activeText.expected,x,y,horizontal,vertical,scale,color,style); }
    catch(...) {activeText={};throw;}
    activeText={};
}

void CpuRenderer::PaintMenu(const MenuFonts &roles,const ScreenPlacement &placement,itemDef_s &item,
                           std::string_view text,const float *color,
                           void(*paint)(void*,itemDef_s*,const char*,const float*),void *context,bool fullItem) {
    CheckThread();
    if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Menu scope requires exclusive recording renderer");
    if(text.size()>1023 || text.find('\0')!=std::string_view::npos)throw std::invalid_argument("Invalid bounded menu text");
    for(size_t i=0;i<text.size();++i) {
        const unsigned char c=text[i];
        if((c<32 && c!='\n' && c!='\r') || c>=127 || (c=='^' && i+1<text.size() && text[i+1]>='0' && text[i+1]<='9'))
            throw std::invalid_argument("Unsupported menu text encoding/control");
    }
    const auto &r=item.window.rect;
    for(float value:{r.x,r.y,r.w,r.h,item.textalignx,item.textaligny,item.textscale,item.window.borderSize})
        if(!std::isfinite(value) || std::abs(value)>16384)throw std::invalid_argument("Invalid menu geometry");
    if(r.w<1 || r.h<0 || item.textscale<=0 || item.textscale>4 || (item.textStyle!=0 && item.textStyle!=3 && item.textStyle!=6) || (item.itemFlags&2) ||
       item.fontEnum<0 || item.fontEnum>6 || item.textAlignMode<0 || item.textAlignMode>14 || (item.textAlignMode&3)==3 ||
       r.horzAlign<0 || r.horzAlign>7 || r.vertAlign<0 || r.vertAlign>7)
        throw std::invalid_argument("Unsupported menu text layout/style");
    if(color)for(unsigned i=0;i<4;++i)if(!std::isfinite(color[i]))throw std::invalid_argument("Invalid menu color");
    for(float value:{roles.smallThreshold,roles.bigThreshold,roles.extraBigThreshold})
        if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("Invalid font threshold");
    CachedAssets_t assets{};
    auto resolve=[&](FontHandle handle)->Font_s* {
        if(!handle)return nullptr;
        auto it=fonts_.find(handle);if(it==fonts_.end())throw std::invalid_argument("Stale menu font");return &it->second->value;
    };
    assets.smallFont=resolve(roles.small);assets.textFont=resolve(roles.text);assets.bigFont=resolve(roles.big);
    assets.extraBigFont=resolve(roles.extraBig);assets.boldFont=resolve(roles.bold);assets.consoleFont=resolve(roles.console);assets.objectiveFont=resolve(roles.objective);
    dvar_s small{},big{},extra{};small.current.value=roles.smallThreshold;big.current.value=roles.bigThreshold;extra.current.value=roles.extraBigThreshold;
    Font_s *selected=UI_SelectFont(&placement,item.fontEnum,item.textscale,assets,&small,&big,&extra);
    if(!selected)throw std::invalid_argument("Selected menu font role unavailable");
    FontHandle selectedHandle=0;for(auto &[handle,owned]:fonts_)if(&owned->value==selected){selectedHandle=handle;break;}
    std::string ownedText(text);itemDef_s local=item;
    const auto priorCommands=commands_;const size_t priorBindings=fontBindingCount_;
    const auto priorPlacement=scrPlaceView[0];scrPlaceView[0]=placement;
    activeText={this,selectedHandle,selected,ownedText,true};menuFont=selected;menuSelection={&assets,&small,&big,&extra};
    try { paint(context,&local,ownedText.c_str(),color); }
    catch(...) { commands_=priorCommands;fontBindingCount_=priorBindings;activeText={};menuFont=nullptr;menuSelection={};scrPlaceView[0]=priorPlacement;throw; }
    activeText={};menuFont=nullptr;menuSelection={};scrPlaceView[0]=priorPlacement;if(fullItem)item=local;else item.textRect[0]=local.textRect[0];
}
void CpuRenderer::DrawMenuText(const MenuFonts &roles,const ScreenPlacement &placement,itemDef_s &item,std::string_view text,const float *color) {
    PaintMenu(roles,placement,item,text,color,[](void*,itemDef_s *local,const char *value,const float *tint){
        Item_Text_AutoWrapped_Paint(0,local,value,tint,false,nullptr,false);
    },nullptr,false);
}
void CpuRenderer::DrawMenuItem(const MenuFonts &roles,const ScreenPlacement &placement,UiContext &dc,itemDef_s &item) {
    if(dc.localClientNum!=0 || !item.parent || (item.type!=1 && item.type!=11))throw std::invalid_argument("Unsupported owned menu item painter");
    PaintMenu(roles,placement,item,{},nullptr,[](void *context,itemDef_s *local,const char*,const float*){
        auto *dc=static_cast<UiContext*>(context);
        if(local->type==11)Item_YesNo_Paint(dc,local);else Item_Text_Paint(dc,local);
    },&dc,true);
}
Font_s *ScopedMenuFont(const ScreenPlacement *placement,int fontEnum,float scale) {
    if(!activeText.renderer || !activeText.menu || !menuFont)throw std::logic_error("Menu font selection requires scoped owned roles");
    auto *selected=UI_SelectFont(placement,fontEnum,scale,*menuSelection.assets,menuSelection.small,menuSelection.big,menuSelection.extra);
    if(selected!=menuFont)throw std::logic_error("Unexpected font selection inside menu scope");
    return selected;
}

}
// Link-selected CPU renderer entry. The normal game entry keeps its existing
// material technique/depth validation and shares the same emission/color packer.
void R_AddCmdDrawStretchPic(float x,float y,float w,float h,float s0,float t0,float s1,float t1,
                            const float *color,Material *material) {
    kisak::window::CpuUiBridge::Record(x,y,w,h,s0,t0,s1,t1,color,material);
}

void R_AddCmdDrawText(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                      float rotation,const float *color,int style) {
    kisak::window::CpuUiBridge::RecordText(text,maxChars,font,x,y,xScale,yScale,rotation,color,style,-1);
}
void R_AddCmdDrawTextWithCursor(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                                float rotation,const float *color,int style,int cursorPos,char) {
    kisak::window::CpuUiBridge::RecordText(text,maxChars,font,x,y,xScale,yScale,rotation,color,style,cursorPos);
}

Font_s *UI_GetFontHandle(const ScreenPlacement *placement,int fontEnum,float scale) { return kisak::window::ScopedMenuFont(placement,fontEnum,scale); }
void UI_DrawTextWithGlow(const ScreenPlacement *,const char *,int,Font_s *,float,float,int,int,float,const float *,int,const float *,bool,bool) {
    throw std::invalid_argument("CPU menu subtitles/glow are unavailable");
}
