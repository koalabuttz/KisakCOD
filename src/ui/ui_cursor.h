#pragma once
struct UiContext;
struct ScreenPlacement;
struct Material;
void UI_UpdateCursorPosition(UiContext *,const ScreenPlacement *,int x,int y);
void UI_DrawCursor(UiContext *,const ScreenPlacement *full,const ScreenPlacement *view,Material *);
