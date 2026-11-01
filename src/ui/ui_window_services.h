#pragma once
struct ScreenPlacement;
struct Material;
void UI_WindowDrawRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c);
void UI_WindowDrawTopBottom(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c);
void UI_WindowDrawSides(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c);
void UI_WindowDrawHighlightRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,float size,const float *c,const float *d);
void UI_WindowFillRect(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c);
void UI_WindowDrawLoadBar(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *c,Material *m);
float UI_WindowBorderLowLightScale();
