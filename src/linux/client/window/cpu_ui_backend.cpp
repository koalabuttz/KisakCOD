#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <cmath>
#include <stdexcept>
namespace kisak::window {
namespace {
struct ActiveUi { CpuRenderer *renderer=nullptr;CpuRenderer::UiMaterialHandle handle=0;Material *expected=nullptr; };
thread_local ActiveUi active;
}
class CpuUiBridge {
public:
    static void Record(float x,float y,float w,float h,float s0,float t0,float s1,float t1,const float *color,Material *material) {
        if(!active.renderer || material!=active.expected)throw std::logic_error("UI renderer entry requires scoped owned material");
        active.renderer->RecordUiStretchPic(active.handle,x,y,w,h,s0,t0,s1,t1,color);
    }
};
void CpuRenderer::DrawUiImage(UiMaterialHandle material,const ScreenPlacement &placement,
                             float x,float y,float width,float height,int horizontal,int vertical,const float *color) {
    CheckThread();if(state_!=State::recording || active.renderer)throw std::logic_error("UI scope requires exclusive recording renderer");
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
}
// Link-selected CPU renderer entry. The normal game entry keeps its existing
// material technique/depth validation and shares the same emission/color packer.
void R_AddCmdDrawStretchPic(float x,float y,float w,float h,float s0,float t0,float s1,float t1,
                            const float *color,Material *material) {
    kisak::window::CpuUiBridge::Record(x,y,w,h,s0,t0,s1,t1,color,material);
}
