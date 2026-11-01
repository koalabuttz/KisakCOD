#include "ui_cursor.h"
#include <universal/q_shared.h>
#include "ui_shared.h"
#include <client/client.h>
void UI_UpdateCursorPosition(UiContext *dc,const ScreenPlacement *full,int x,int y)
{
    dc->cursor.x = x / full->scaleVirtualToFull[0];
    dc->cursor.y = y / full->scaleVirtualToFull[1];
    bool cursorInBounds = dc->cursor.x >= 0.0
        && dc->cursor.x <= 640.0
        && dc->cursor.y >= 0.0
        && dc->cursor.y <= 480.0;
    dc->isCursorVisible = cursorInBounds;
}
void UI_DrawCursor(UiContext *dc,const ScreenPlacement *full,const ScreenPlacement *view,Material *material)
{
    float w = full->scaleVirtualToReal[0] * 32.0 / full->scaleVirtualToFull[0];
    float h = full->scaleVirtualToReal[1] * 32.0 / full->scaleVirtualToFull[1];
    float y = dc->cursor.y - h * 0.5;
    float x = dc->cursor.x - w * 0.5;
    UI_DrawHandlePic(view, x, y, w, h, 4, 4, 0, material);
}
