#include <ui/ui_window_services.h>
#include <stdexcept>
// Link-selected unsupported window services. Admission rejects these branches.
void UI_WindowDrawRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { throw std::invalid_argument("Unsupported window service: UI_DrawRect"); }
void UI_WindowDrawTopBottom(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { throw std::invalid_argument("Unsupported window service: UI_DrawTopBottom"); }
void UI_WindowDrawSides(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { throw std::invalid_argument("Unsupported window service: UI_DrawSides"); }
void UI_WindowDrawHighlightRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c,const float *d) { throw std::invalid_argument("Unsupported window service: UI_DrawHighlightRect"); }
void UI_WindowFillRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c) { throw std::invalid_argument("Unsupported window service: UI_FillRect"); }
void UI_WindowDrawLoadBar(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c,Material *m) { throw std::invalid_argument("Unsupported window service: UI_DrawLoadBar"); }
float UI_WindowBorderLowLightScale() { throw std::invalid_argument("Unsupported window border lighting"); }
