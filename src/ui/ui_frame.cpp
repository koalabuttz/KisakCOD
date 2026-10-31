#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_input_core.h"
#include <client/client.h>
#include <universal/profile.h>
#include <algorithm>
#include <string>

int __cdecl Menu_Count(UiContext *dc)
{
    return dc->menuCount;
}

void __cdecl Menu_PaintAll(UiContext *dc)
{
    PROF_SCOPED("Menu_PaintAll");

    char *v1; // eax
    Font_s *font; // [esp+2Ch] [ebp-41Ch]
    bool showVisibleList; // [esp+33h] [ebp-415h]
    int menuIndex; // [esp+34h] [ebp-414h]
    int menuIndexa; // [esp+34h] [ebp-414h]
    int menuIndexb; // [esp+34h] [ebp-414h]
    menuDef_t *menu; // [esp+38h] [ebp-410h]
    menuDef_t *menua; // [esp+38h] [ebp-410h]
    menuDef_t *menub; // [esp+38h] [ebp-410h]
    bool anyFullscreen; // [esp+3Eh] [ebp-40Ah]
    char visibleList[1024]; // [esp+40h] [ebp-408h] BYREF
    int drawStart; // [esp+444h] [ebp-4h]

    showVisibleList = ui_showList->current.enabled;
    KISAK_NULLSUB();
    if (showVisibleList)
        Menu_PaintAll_BeginVisibleList(visibleList, 0x400u);
    dc->blurRadiusOut = 0.0;
    if (captureFunc)
        captureFunc(dc, captureData);
    anyFullscreen = 0;
    drawStart = 0;
    for (menuIndex = dc->openMenuCount - 1; menuIndex >= 0; --menuIndex)
    {
        menu = dc->menuStack[menuIndex];
        if (!menu)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 6124, 0, "%s", "menu");
        if (menu->fullScreen)
        {
            drawStart = menuIndex;
            anyFullscreen = 1;
            break;
        }
    }

    {
        PROF_SCOPED("Menu_PaintAll_PaintMenus");
        if (!anyFullscreen)
        {
            for (menuIndexa = 0; menuIndexa < Menu_Count(dc); ++menuIndexa)
            {
                menua = dc->Menus[menuIndexa];
                if (!menua)
                    MyAssertHandler(".\\ui\\ui_shared.cpp", 6139, 0, "%s", "menu");
                if (!Menus_MenuIsInStack(dc, menua) && Menu_Paint(dc, menua) && showVisibleList)
                    Menu_PaintAll_AppendToVisibleList(visibleList, 0x400u, (char *)menua->window.name);
            }
        }
    }

    {
        PROF_SCOPED("Menu_PaintAll_PaintOpenMenus");
        for (menuIndexb = drawStart; menuIndexb < dc->openMenuCount; ++menuIndexb)
        {
            menub = dc->menuStack[menuIndexb];
            if (!menub)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 6153, 0, "%s", "menu");
            if (Menu_Paint(dc, menub) && showVisibleList)
                Menu_PaintAll_AppendToVisibleList(visibleList, 0x400u, (char *)menub->window.name);
        }
    }

    {
        PROF_SCOPED("Menu_PaintAll_Debug");

        if (g_debugMode)
        {
            font = UI_GetFontHandle(&scrPlaceView[dc->localClientNum], 0, 0.5);
            v1 = va("fps: %f", dc->FPS);
            UI_DrawText(&scrPlaceView[dc->localClientNum], v1, 0x7FFFFFFF, font, 5.0, 25.0, 0, 0, 0.5, colorWhite, 0);
        }
    }

    {
        PROF_SCOPED("Menu_PaintAll_DrawVisibleList");

        if (showVisibleList)
            Menu_PaintAll_DrawVisibleList(visibleList, dc);
    }
    
}
