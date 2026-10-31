#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <cmath>
#include <stdexcept>
namespace kisak::window {
namespace {
struct ActiveUi { CpuRenderer *renderer=nullptr;CpuRenderer::UiMaterialHandle handle=0;Material *expected=nullptr; };
thread_local ActiveUi active;
struct ActiveText { CpuRenderer *renderer=nullptr;CpuRenderer::FontHandle handle=0;Font_s *expected=nullptr;std::string_view text; };
thread_local ActiveText activeText;
}
class CpuUiBridge {
public:
    static void RecordText(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                           float rotation,const float *color,int style,int cursorPos) {
        if(!activeText.renderer || font!=activeText.expected || text!=activeText.text.data())throw std::logic_error("Text entry requires scoped owned font/text");
        activeText.renderer->RecordText(activeText.handle,activeText.text,maxChars,x,y,xScale,yScale,rotation,color,style,cursorPos);
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
