#pragma once
#include "ui_shared.h"
struct scrollInfo_s // sizeof=0x20
{                                       // ...
    int nextScrollTime;                 // ...
    int nextAdjustTime;                 // ...
    int adjustValue;                    // ...
    int scrollKey;                      // ...
    float xStart;                       // ...
    float yStart;                       // ...
    itemDef_s *item;                    // ...
    int scrollDir;                      // ...
};

extern int g_waitingForKey;
extern int g_editingField;
extern struct itemDef_s *g_editItem;
extern int g_debugMode;
extern void(__cdecl *captureFunc)(UiContext *, void *);
extern void *captureData;
extern itemDef_s *itemCapture;
extern itemDef_s *g_bindItem;
extern int inHandleKey;
extern scrollInfo_s scrollInfo;
extern rectDef_s rect;
// Existing game services retained outside the reusable navigation/dispatch core.
void Item_ListBox_MouseEnter(int,itemDef_s*,float,float);
void Menus_HandleOOBClick(UiContext*,menuDef_t*,int,int);
int Item_ListBox_OverLB(int,itemDef_s*,float,float);
void Scroll_ListBox_AutoFunc(UiContext*,void*);
void Scroll_ListBox_ThumbFunc(UiContext*,void*);
int Item_Slider_OverSlider(int,itemDef_s*,float,float);
void Scroll_Slider_ThumbFunc(UiContext*,itemDef_s**);
BOOL Menu_HandleMouseMove(UiContext*,menuDef_t*);
BOOL Item_ContainsMouse(UiContext*,itemDef_s*);
