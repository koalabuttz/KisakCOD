#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_input_core.h"
#include <client/client.h>
#include <cgame/cg_local.h>
#include <universal/profile.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include "ui.h"
#endif

char __cdecl Menu_IsVisible(UiContext *dc, menuDef_t *menu)
{
    if (!Window_IsVisible(dc->localClientNum, &menu->window))
        return 0;
    if (menu->window.ownerDrawFlags && !UI_OwnerDrawVisible(menu->window.ownerDrawFlags))
        return 0;
    if ((menu->window.staticFlags & 0x20000000) != 0
        && CL_IsCgameInitialized(dc->localClientNum)
        && CG_ScopeIsOverlayed(dc->localClientNum))
    {
        return 0;
    }
    if ((menu->window.staticFlags & 0x10000000) != 0
        && CL_IsCgameInitialized(dc->localClientNum)
        && CG_Flashbanged(dc->localClientNum))
    {
        return 0;
    }
    if ((menu->window.staticFlags & 0x40000000) != 0)
    {
        clientUIActive_t *clUI = CL_GetLocalClientUIGlobals(dc->localClientNum);

        if ((clUI->keyCatchers & KEYCATCH_UI) != 0)
            return 0;
    }
    if (!menu->visibleExp.numEntries || IsExpressionTrue(dc->localClientNum, &menu->visibleExp))
        return 1;
    if (uiscript_debug->current.integer)
    {
        if (menu->window.name)
            Com_Printf(CON_CHANNEL_UI, "hiding the %s menu becuase the 'visible when' expression was false\n", menu->window.name);
        else
            Com_Printf(CON_CHANNEL_UI, "hiding the %s menu becuase the 'visible when' expression was false\n", "unnamed");
    }
    return 0;
}

char __cdecl Menu_Paint(UiContext *dc, menuDef_t *menu)
{
    float fadeCycle; // [esp+1Ch] [ebp-14h]
    float v4; // [esp+20h] [ebp-10h]
    float v5; // [esp+24h] [ebp-Ch]
    int i; // [esp+28h] [ebp-8h]

    PROF_SCOPED("Menu_Paint");

    ZoneText(menu->window.name, strlen(menu->window.name));

    iassert(menu);

    if (*(_BYTE *)ui_showMenuOnly->current.integer
        && menu->window.name
        && I_stricmp(menu->window.name, ui_showMenuOnly->current.string))
    {
        return 0;
    }

    if (!Menu_IsVisible(dc, menu))
        return 0;

    if (menu->soundName)
        UI_PlayLocalSoundAliasByName(dc->localClientNum, menu->soundName);

    if (menu->blurRadius != 0.0)
    {
        v5 = dc->blurRadiusOut * dc->blurRadiusOut + menu->blurRadius * menu->blurRadius;
        v4 = sqrt(v5);
        dc->blurRadiusOut = v4;
    }

    if (menu->rectXExp.numEntries)
        menu->window.rect.x = GetExpressionFloat(dc->localClientNum, &menu->rectXExp);

    if (menu->rectYExp.numEntries)
        menu->window.rect.y = GetExpressionFloat(dc->localClientNum, &menu->rectYExp);

    Menu_UpdatePosition(dc->localClientNum, menu);
    if (menu->fullScreen && menu->window.background)
    {
        if (!menu)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        UI_DrawHandlePic(
            &scrPlaceView[dc->localClientNum],
            0.0,
            0.0,
            640.0,
            480.0,
            menu->window.rect.horzAlign,
            menu->window.rect.vertAlign,
            0,
            menu->window.background);
    }
    fadeCycle = (float)menu->fadeCycle;
    Window_Paint(dc, &menu->window, menu->fadeAmount, menu->fadeInAmount, menu->fadeClamp, fadeCycle);

    for (i = 0; i < menu->itemCount; ++i)
        Item_Paint(dc, menu->items[i]);

    if (g_debugMode)
    {
        if (!menu)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        UI_DrawRect(
            &scrPlaceView[dc->localClientNum],
            menu->window.rect.x,
            menu->window.rect.y,
            menu->window.rect.w,
            menu->window.rect.h,
            menu->window.rect.horzAlign,
            menu->window.rect.vertAlign,
            1.0,
            colorMagenta);
    }

    return 1;
}


void __cdecl Item_Paint(UiContext *dc, itemDef_s *item)
{
    PROF_SCOPED("Item_Paint");

    //ZoneText(item->na)
    menuDef_t *v2; // esi
    menuDef_t *v3; // esi
    char *String; // eax
    char *ExpressionResultString; // eax
    float fadeCycle; // [esp+1Ch] [ebp-78h]
    rectDef_s *r; // [esp+34h] [ebp-60h]
    menuDef_t *parent; // [esp+48h] [ebp-4Ch]
    char lowerCaseName[68]; // [esp+4Ch] [ebp-48h] BYREF

    parent = item->parent;
    if (item)
    {
        if (item->window.ownerDrawFlags)
        {
            if (UI_OwnerDrawVisible(item->window.ownerDrawFlags))
                Window_AddDynamicFlags(dc->localClientNum, &item->window, 4);
            else
                Window_RemoveDynamicFlags(dc->localClientNum, &item->window, 4);
        }
        if ((item->dvarFlags & 0xC) == 0 || Item_EnableShowViaDvar(item, 4))
        {
            if (item->forecolorAExp.numEntries)
                item->window.foreColor[3] = GetExpressionFloat(dc->localClientNum, &item->forecolorAExp);
            if (Item_IsVisible(dc->localClientNum, item))
            {
                if (item->rectXExp.numEntries)
                {
                    v2 = item->parent;
                    item->window.rect.x = GetExpressionFloat(dc->localClientNum, &item->rectXExp) + v2->window.rect.x;
                }
                if (item->rectYExp.numEntries)
                {
                    v3 = item->parent;
                    item->window.rect.y = GetExpressionFloat(dc->localClientNum, &item->rectYExp) + v3->window.rect.y;
                }
                if (item->rectWExp.numEntries)
                    item->window.rect.w = GetExpressionFloat(dc->localClientNum, &item->rectWExp);
                if (item->rectHExp.numEntries)
                    item->window.rect.h = GetExpressionFloat(dc->localClientNum, &item->rectHExp);
                if (item->window.style == 5)
                {
                    String = (char *)Dvar_GetString(item->dvar);
                    I_strncpyz(lowerCaseName, String, 64);
                    I_strlwr(lowerCaseName);
                    item->window.background = Material_RegisterHandle(lowerCaseName, item->imageTrack);
                }
                else if (item->materialExp.numEntries)
                {
                    ExpressionResultString = GetExpressionResultString(dc->localClientNum, &item->materialExp);
                    I_strncpyz(lowerCaseName, ExpressionResultString, 64);
                    I_strlwr(lowerCaseName);
                    item->window.background = Material_RegisterHandle(lowerCaseName, item->imageTrack);
                }
                fadeCycle = (float)parent->fadeCycle;
                Window_Paint(dc, &item->window, parent->fadeAmount, parent->fadeInAmount, parent->fadeClamp, fadeCycle);
                if (g_debugMode)
                {
                    r = Item_CorrectedTextRect(dc->localClientNum, item);
                    UI_DrawRect(
                        &scrPlaceView[dc->localClientNum],
                        r->x,
                        r->y,
                        r->w,
                        r->h,
                        r->horzAlign,
                        r->vertAlign,
                        1.0,
                        colorGreen);
                }
                if (item->window.style != 5)
                {
                    switch (item->type)
                    {
                    case 0:
                    case 1:
                        Item_Text_Paint(dc, item);
                        break;
                    case 4:
                    case 9:
                    case 0x10:
                    case 0x11:
                    case 0x12:
                        Item_TextField_Paint(dc, item);
                        break;
                    case 6:
                        Item_ListBox_Paint(dc, item);
                        break;
                    case 8:
                        Item_OwnerDraw_Paint(dc, item);
                        break;
                    case 0xA:
                        Item_Slider_Paint(dc, item);
                        break;
                    case 0xB:
                        Item_YesNo_Paint(dc, item);
                        break;
                    case 0xC:
                        Item_Multi_Paint(dc, item);
                        break;
                    case 0xD:
                        Item_DvarEnum_Paint(dc, item);
                        break;
                    case 0xE:
                        Item_Bind_Paint(dc, item);
                        break;
                    case 0x13:
                        Item_GameMsgWindow_Paint(dc, item);
                        break;
                    default:
                        return;
                    }
                }
            }
        }
        else if (Window_HasFocus(dc->localClientNum, &item->window))
        {
            Menu_HandleKey(dc, parent, 155, 1);
            Menu_HandleKey(dc, parent, 155, 0);
        }
    }
}

