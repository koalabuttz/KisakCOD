#include "ui_window_services.h"
#include <universal/q_shared.h>
#include "ui_shared.h"
void UI_WindowDrawRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { UI_DrawRect(p,x,y,w,h,a,b,size,c); }
void UI_WindowDrawTopBottom(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { UI_DrawTopBottom(p,x,y,w,h,a,b,size,c); }
void UI_WindowDrawSides(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c) { UI_DrawSides(p,x,y,w,h,a,b,size,c); }
void UI_WindowDrawHighlightRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c,const float *d) { UI_DrawHighlightRect(p,x,y,w,h,a,b,size,c,d); }
void UI_WindowFillRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c) { UI_FillRect(p,x,y,w,h,a,b,c); }
void UI_WindowDrawLoadBar(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c,Material *m) { UI_DrawLoadBar(p,x,y,w,h,a,b,c,m); }
float UI_WindowBorderLowLightScale() { return ui_borderLowLightScale->current.value; }
