#include <universal/q_shared.h>
#include "ui_input_core.h"
#include <client/client.h>
#include <qcommon/cmd.h>
#include <universal/profile.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif defined(KISAK_SP)
#include "ui.h"
#endif

int g_waitingForKey;
int g_editingField;
struct itemDef_s *g_editItem;
int g_debugMode;
void(__cdecl *captureFunc)(UiContext *, void *);
void *captureData;
itemDef_s *itemCapture;
itemDef_s *g_bindItem;
int inHandleKey;
scrollInfo_s scrollInfo;
rectDef_s rect;

bool __cdecl Window_IsVisible(int localClientNum, const windowDef_t *w)
{
    if (!w)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 60, 0, "%s", "w");
    if (localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            23,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    return (w->dynamicFlags[localClientNum] & 4) != 0;
}

bool __cdecl Window_HasFocus(int localClientNum, const windowDef_t *w)
{
    if (!w)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 70, 0, "%s", "w");
    if (Window_IsVisible(localClientNum, w))
    {
        if (localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                23,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        return (w->dynamicFlags[localClientNum] & 2) != 0;
    }
    else
    {
        if (localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                23,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        if ((w->dynamicFlags[localClientNum] & 2) != 0 && !alwaysfails)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 74, 0, "Hidden window has focus!");
        return 0;
    }
}

BOOL __cdecl IsVisible(char flags)
{
    return (flags & 4) != 0 && (flags & 0x10) == 0;
}

void __cdecl Item_MouseEnter(UiContext *dc, itemDef_s *item, float x, float y)
{
    int localClientNum; // [esp+8h] [ebp-24h]
    rectDef_s r; // [esp+Ch] [ebp-20h] BYREF
    int flags; // [esp+24h] [ebp-8h]
    const rectDef_s *textRect; // [esp+28h] [ebp-4h]

    if (item)
    {
        textRect = Item_GetTextRect(dc->localClientNum, item);
        r = *textRect;
        r.y = r.y - r.h;
        r.horzAlign = textRect->horzAlign;
        r.vertAlign = textRect->vertAlign;
        if ((item->dvarFlags & 3) == 0 || Item_EnableShowViaDvar(item, 1))
        {
            if (Item_IsVisible(dc->localClientNum, item))
            {
                localClientNum = dc->localClientNum;
                if (dc->localClientNum)
                    MyAssertHandler(
                        "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                        23,
                        0,
                        "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                        localClientNum,
                        1);
                flags = item->window.dynamicFlags[localClientNum];
                if (Rect_ContainsPoint(dc->localClientNum, &r, x, y))
                {
                    if ((flags & 0x40) == 0)
                    {
                        Item_RunScript(dc, item, (char*)item->mouseEnterText);
                        Window_AddDynamicFlags(dc->localClientNum, &item->window, 64);
                    }
                    if ((flags & 1) == 0)
                    {
                        Item_RunScript(dc, item, (char *)item->mouseEnter);
                        Window_AddDynamicFlags(dc->localClientNum, &item->window, 1);
                    }
                }
                else
                {
                    if ((flags & 0x40) != 0)
                    {
                        Item_RunScript(dc, item, (char *)item->mouseExitText);
                        Window_RemoveDynamicFlags(dc->localClientNum, &item->window, 64);
                    }
                    if ((flags & 1) == 0)
                    {
                        Item_RunScript(dc, item, (char *)item->mouseEnter);
                        Window_AddDynamicFlags(dc->localClientNum, &item->window, 1);
                    }
                    if (item->type == 6)
                        Item_ListBox_MouseEnter(dc->localClientNum, item, x, y);
                }
            }
        }
    }
}

void __cdecl Item_MouseLeave(UiContext *dc, itemDef_s *item)
{
    int localClientNum; // [esp+4h] [ebp-4h]

    if (item)
    {
        localClientNum = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                23,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        if ((item->window.dynamicFlags[localClientNum] & 0x40) != 0)
        {
            Item_RunScript(dc, item, (char *)item->mouseExitText);
            Window_RemoveDynamicFlags(dc->localClientNum, &item->window, 64);
        }
        Item_RunScript(dc, item, (char *)item->mouseExit);
        Window_RemoveDynamicFlags(dc->localClientNum, &item->window, 768);
    }
    else
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2674, 0, "%s", "item");
    }
}

void __cdecl Item_SetMouseOver(const UiContext *dc, itemDef_s *item, int focus)
{
    if (item)
    {
        if (focus)
            Window_AddDynamicFlags(dc->localClientNum, &item->window, 1);
        else
            Window_RemoveDynamicFlags(dc->localClientNum, &item->window, 1);
    }
    else
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2692, 0, "%s", "item");
    }
}

BOOL __cdecl Menu_HandleMouseMove(UiContext *dc, menuDef_t *menu)
{
    const rectDef_s *v3; // eax
    itemDef_s *v4; // [esp+Ch] [ebp-4Ch]
    int v5; // [esp+10h] [ebp-48h]
    int v6; // [esp+18h] [ebp-40h]
    itemDef_s *v7; // [esp+1Ch] [ebp-3Ch]
    itemDef_s *v8; // [esp+2Ch] [ebp-2Ch]
    int v9; // [esp+30h] [ebp-28h]
    int localClientNum; // [esp+38h] [ebp-20h]
    int pass; // [esp+3Ch] [ebp-1Ch]
    itemDef_s *focusItem; // [esp+40h] [ebp-18h]
    int i; // [esp+44h] [ebp-14h]
    float x; // [esp+48h] [ebp-10h]
    float y; // [esp+4Ch] [ebp-Ch]
    int focusSet; // [esp+50h] [ebp-8h]
    itemDef_s *overItem; // [esp+54h] [ebp-4h]

    focusSet = 0;
    focusItem = 0;
    if (!dc->isCursorVisible)
        return 0;
    if (!menu)
        return 0;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            23,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    if ((menu->window.dynamicFlags[localClientNum] & 0x4004) == 0)
        return 0;
    if (itemCapture)
        return 0;
    if (g_waitingForKey)
        return 0;
    x = dc->cursor.x;
    y = dc->cursor.y;
    for (pass = 0; pass < 2; ++pass)
    {
        for (i = menu->itemCount - 1; i >= 0; --i)
        {
            v8 = menu->items[i];
            v9 = dc->localClientNum;
            if (dc->localClientNum)
                MyAssertHandler(
                    "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                    23,
                    0,
                    "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                    v9,
                    1);
            if ((v8->window.dynamicFlags[v9] & 0x4004) != 0
                && ((menu->items[i]->dvarFlags & 3) == 0 || Item_EnableShowViaDvar(menu->items[i], 1))
                && ((menu->items[i]->dvarFlags & 0xC) == 0 || Item_EnableShowViaDvar(menu->items[i], 4))
                && Item_IsVisible(dc->localClientNum, menu->items[i]))
            {
                if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window) && !focusItem)
                    focusItem = menu->items[i];
                v7 = menu->items[i];
                if (!v7)
                    MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
                if (Rect_ContainsPoint(dc->localClientNum, &v7->window.rect, x, y))
                {
                    if (pass == 1)
                    {
                        overItem = menu->items[i];
                        if (overItem->type
                            || !overItem->text
                            || (v3 = Item_CorrectedTextRect(dc->localClientNum, overItem),
                                Rect_ContainsPoint(dc->localClientNum, v3, x, y)))
                        {
                            v6 = dc->localClientNum;
                            if (dc->localClientNum)
                                MyAssertHandler(
                                    "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                                    23,
                                    0,
                                    "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                                    v6,
                                    1);
                            if (IsVisible(overItem->window.dynamicFlags[v6]))
                            {
                                Item_MouseEnter(dc, overItem, x, y);
                                if (!focusSet)
                                {
                                    focusSet = Item_SetFocus(dc, overItem, x, y);
                                    if (focusSet)
                                        focusItem = overItem;
                                }
                            }
                        }
                    }
                }
                else
                {
                    v4 = menu->items[i];
                    v5 = dc->localClientNum;
                    if (dc->localClientNum)
                        MyAssertHandler(
                            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                            23,
                            0,
                            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                            v5,
                            1);
                    if ((v4->window.dynamicFlags[v5] & 1) != 0)
                    {
                        Item_MouseLeave(dc, menu->items[i]);
                        Item_SetMouseOver(dc, menu->items[i], 0);
                    }
                }
            }
        }
    }
    if (!focusSet && focusItem && !Rect_ContainsPoint(dc->localClientNum, &focusItem->window.rect, x, y))
        Menu_ClearFocus(dc, menu);
    return focusSet != 0;
}

int __cdecl Item_SetFocus(UiContext *dc, itemDef_s *item, float x, float y)
{
    rectDef_s r; // [esp+28h] [ebp-2Ch] BYREF
    const rectDef_s *textRect; // [esp+40h] [ebp-14h]
    itemDef_s *oldFocus; // [esp+44h] [ebp-10h]
    menuDef_t *focusedMenu; // [esp+48h] [ebp-Ch]
    menuDef_t *parent; // [esp+4Ch] [ebp-8h]
    int i; // [esp+50h] [ebp-4h]

    if (!item)
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2183, 0, "%s", "item != NULL");
        return 0;
    }
    if ((item->window.staticFlags & 0x100000) != 0 || !Window_IsVisible(dc->localClientNum, &item->window))
        return 0;
    if (Window_HasFocus(dc->localClientNum, &item->window) && Item_IsVisible(dc->localClientNum, item))
        return 1;
    parent = item->parent;
    if (parent)
    {
        if (!Window_HasFocus(dc->localClientNum, &parent->window))
        {
            focusedMenu = Menu_GetFocused(dc);
            if (focusedMenu)
            {
                if (Rect_ContainsPoint(dc->localClientNum, &focusedMenu->window.rect, x, y)
                    && Rect_ContainsPoint(dc->localClientNum, &parent->window.rect, x, y))
                {
                    return 0;
                }
            }
        }
    }
    if ((item->dvarFlags & 3) != 0 && !Item_EnableShowViaDvar(item, 1))
        return 0;
    if (!Item_IsVisible(dc->localClientNum, item))
        return 0;
    oldFocus = Menu_ClearFocus(dc, item->parent);
    if (item->type)
    {
        Window_AddDynamicFlags(dc->localClientNum, &item->window, 2);
        if (item->onFocus)
            Item_RunScript(dc, item, (char*)item->onFocus);
    }
    else
    {
        textRect = Item_GetTextRect(dc->localClientNum, item);
        r = *textRect;
        r.y = r.y - r.h;
        r.horzAlign = textRect->horzAlign;
        r.vertAlign = textRect->vertAlign;
        if (Rect_ContainsPoint(dc->localClientNum, &r, x, y))
        {
            Window_AddDynamicFlags(dc->localClientNum, &item->window, 2);
        }
        else if (oldFocus)
        {
            Window_AddDynamicFlags(dc->localClientNum, &oldFocus->window, 2);
            if (oldFocus->onFocus)
                Item_RunScript(dc, oldFocus, (char*)oldFocus->onFocus);
        }
    }
    for (i = 0; i < parent->itemCount; ++i)
    {
        if (parent->items[i] == item && !Item_IsTextField(item))
        {
            Menu_SetCursorItem(dc->localClientNum, parent, i);
            return 1;
        }
    }
    return 1;
}

const rectDef_s *__cdecl Item_GetTextRect(int localClientNum, const itemDef_s *item)
{
    if (localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            43,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 44, 0, "%s", "item");
    return &item->textRect[localClientNum];
}

itemDef_s *__cdecl Menu_ClearFocus(UiContext *dc, menuDef_t *menu)
{
    itemDef_s *ret; // [esp+Ch] [ebp-8h]
    int i; // [esp+10h] [ebp-4h]

    if (!menu)
        return 0;
    ret = 0;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
        {
            if (ret)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 471, 0, "%s", "ret == NULL");
            ret = menu->items[i];
            Window_RemoveDynamicFlags(dc->localClientNum, &ret->window, 2);
            if (menu->items[i]->leaveFocus)
                Item_RunScript(dc, menu->items[i], (char*)menu->items[i]->leaveFocus);
        }
    }
    return ret;
}

bool __cdecl Rect_ContainsPoint(int localClientNum, const rectDef_s *rect, float x, float y)
{
    float compareY; // [esp+8h] [ebp-24h]
    rectDef_s compareRect; // [esp+Ch] [ebp-20h] BYREF
    const ScreenPlacement *scrPlace; // [esp+24h] [ebp-8h]
    float compareX; // [esp+28h] [ebp-4h]

    if (!rect)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 492, 0, "%s", "rect");
    compareRect.x = rect->x;
    compareRect.y = rect->y;
    compareRect.w = rect->w;
    compareRect.h = rect->h;
    scrPlace = &scrPlaceView[localClientNum];
    compareX = ScrPlace_ApplyX(scrPlace, x, 4);
    compareY = ScrPlace_ApplyY(scrPlace, y, 4);
    ScrPlace_ApplyRect(
        scrPlace,
        &compareRect.x,
        &compareRect.y,
        &compareRect.w,
        &compareRect.h,
        rect->horzAlign,
        rect->vertAlign);
    return compareRect.x <= (double)compareX
        && compareX <= compareRect.x + compareRect.w
        && compareRect.y <= (double)compareY
        && compareY <= compareRect.y + compareRect.h;
}

bool __cdecl Item_IsTextField(const itemDef_s *item)
{
    bool result; // al

    switch (item->type)
    {
    case 4:
    case 9:
    case 0x10:
    case 0x11:
    case 0x12:
        result = 1;
        break;
    default:
        result = 0;
        break;
    }
    return result;
}

void __cdecl Menu_HandleKey(UiContext *dc, menuDef_t *menu, int key, int down)
{
    int v4; // eax
    const rectDef_s *v5; // eax
    float x; // [esp+0h] [ebp-1A4h]
    float y; // [esp+4h] [ebp-1A0h]
    itemDef_s it; // [esp+1Ch] [ebp-188h] BYREF
    itemDef_s *item; // [esp+194h] [ebp-10h]
    int inHandler; // [esp+198h] [ebp-Ch]
    int i; // [esp+19Ch] [ebp-8h]
    const char *binding; // [esp+1A0h] [ebp-4h]

    item = 0;
    inHandler = 1;
    if (g_waitingForKey && down)
    {
        Item_Bind_HandleKey(dc, g_bindItem, key, down);
        inHandler = 0;
        return;
    }
    if (!g_editingField || !down)
        goto LABEL_13;
    if (!Item_TextField_HandleKey(dc, g_editItem, key))
    {
        g_editingField = 0;
        g_editItem = 0;
        inHandler = 0;
        return;
    }
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_MOUSE3)
    {
        g_editingField = 0;
        g_editItem = 0;
        Display_MouseMove(dc);
    LABEL_13:
        if (menu)
        {
            if (down
                && (key & K_CHAR_FLAG) == 0
                && menu->allowedBinding
                && (binding = Key_GetBinding(dc->localClientNum, key)) != 0
                && !I_stricmp(binding, menu->allowedBinding))
            {
                v4 = CL_ControllerIndexFromClientNum(dc->localClientNum);
                Cbuf_ExecuteBuffer(dc->localClientNum, v4, (char *)binding);
            }
            else if (!down
                || (menu->window.staticFlags & 0x1000000) != 0
                || menu->fullScreen
                || Rect_ContainsPoint(dc->localClientNum, &menu->window.rect, dc->cursor.x, dc->cursor.y)
                || inHandleKey
                || key != K_MOUSE1 && key != K_MOUSE2 && key != K_MOUSE3)
            {
                for (i = 0; i < menu->itemCount; ++i)
                {
                    if (Item_IsVisible(dc->localClientNum, menu->items[i]))
                    {
                        if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
                            item = menu->items[i];
                    }
                }
                if (key != K_MWHEELDOWN && key != K_MWHEELUP || item && item->type == 6)
                {
                    if (item && Item_HandleKey(dc, item, key, down))
                    {
                        Item_Action(dc, item);
                        inHandler = 0;
                    }
                    else if (down)
                    {
                        if (key <= 0 || key > 255 || !Menu_CheckOnKey(dc, menu, key))
                        {
                            switch (key)
                            {
                            case K_TAB:
                            case K_DOWNARROW:
                            case K_RIGHTARROW:
                            case K_KP_DOWNARROW:
                            case K_MWHEELDOWN:
                                Menu_SetNextCursorItem(dc, menu);
                                break;
                            case K_ENTER:
                            case K_KP_ENTER:
                            case K_MOUSE3:
                                if (item)
                                {
                                    if (Item_IsTextField(item))
                                    {
                                        item->cursorPos[dc->localClientNum] = 0;
                                        g_editingField = 1;
                                        g_editItem = item;
                                        Key_SetOverstrikeMode(dc->localClientNum, 1);
                                    }
                                    else
                                    {
                                        Item_Action(dc, item);
                                    }
                                }
                                break;
                            case K_ESCAPE:
                                if (!g_waitingForKey && menu->onESC)
                                {
                                    it.parent = menu;
                                    Item_RunScript(dc, &it, (char*)menu->onESC);
                                }
                                break;
                            case K_UPARROW:
                            case K_LEFTARROW:
                            case K_KP_UPARROW:
                            case K_MWHEELUP:
                                Menu_SetPrevCursorItem(dc, menu);
                                break;
                            case K_F11:
                                if (Dvar_GetInt("developer"))
                                    g_debugMode ^= 1u;
                                break;
                            case K_F12:
                                if (Dvar_GetInt("developer"))
                                    Cbuf_AddText(dc->localClientNum, "screenshot\n");
                                break;
                            case K_MOUSE1:
                            case K_MOUSE2:
                                if (item)
                                {
                                    if (item->type)
                                    {
                                        if (Rect_ContainsPoint(dc->localClientNum, &item->window.rect, dc->cursor.x, dc->cursor.y))
                                        {
                                            if (Item_IsTextField(item))
                                                Item_TextField_BeginEdit(dc->localClientNum, item);
                                            else
                                                Item_Action(dc, item);
                                        }
                                    }
                                    else
                                    {
                                        y = dc->cursor.y;
                                        x = dc->cursor.x;
                                        v5 = Item_CorrectedTextRect(dc->localClientNum, item);
                                        if (Rect_ContainsPoint(dc->localClientNum, v5, x, y))
                                            Item_Action(dc, item);
                                    }
                                }
                                break;
                            default:
                                return;
                            }
                        }
                    }
                    else
                    {
                        inHandler = 0;
                    }
                }
            }
            else
            {
                inHandleKey = 1;
                Menus_HandleOOBClick(dc, menu, key, down);
                inHandleKey = 0;
                inHandler = 0;
            }
        }
        else
        {
            inHandler = 0;
        }
    }
}

void __cdecl Item_StartCapture(UiContext *dc, itemDef_s *item, int key)
{
    int type; // [esp+8h] [ebp-8h]
    __int16 flags; // [esp+Ch] [ebp-4h]

    type = item->type;
    if (type == 6)
    {
        flags = Item_ListBox_OverLB(dc->localClientNum, item, dc->cursor.x, dc->cursor.y);
        if ((flags & 0x300) != 0)
        {
            scrollInfo.nextScrollTime = dc->realTime + 500;
            scrollInfo.nextAdjustTime = dc->realTime + 150;
            scrollInfo.adjustValue = 500;
            scrollInfo.scrollKey = key;
            scrollInfo.scrollDir = (flags & 0x100) != 0;
            scrollInfo.item = item;
            captureData = &scrollInfo;
            captureFunc = Scroll_ListBox_AutoFunc;
            itemCapture = item;
        }
        else if ((flags & 0x400) != 0)
        {
            scrollInfo.scrollKey = key;
            scrollInfo.item = item;
            scrollInfo.xStart = dc->cursor.x;
            scrollInfo.yStart = dc->cursor.y;
            captureData = &scrollInfo;
            captureFunc = Scroll_ListBox_ThumbFunc;
            itemCapture = item;
        }
    }
    else if (type == 10 && (Item_Slider_OverSlider(dc->localClientNum, item, dc->cursor.x, dc->cursor.y) & 0x400) != 0)
    {
        scrollInfo.scrollKey = key;
        scrollInfo.item = item;
        scrollInfo.xStart = dc->cursor.x;
        scrollInfo.yStart = dc->cursor.y;
        captureData = &scrollInfo;
        captureFunc = (void(__cdecl *)(UiContext *, void *))Scroll_Slider_ThumbFunc;
        itemCapture = item;
    }
}

int Item_HandleKey(UiContext *dc, itemDef_s *item, int key, int down)
{
    int result; // r3

    if (itemCapture)
    {
        itemCapture = 0;
        captureFunc = 0;
        captureData = 0;
    }
    else
    {
        if (!down)
            return 0;
        if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_MOUSE3)
            Item_StartCapture(dc, item, key);
    }
    if (!down)
        return 0;
    switch (item->type)
    {
    case 6:
        result = Item_ListBox_HandleKey(dc, item, key, down, 0);
        break;
    case 8:
        result = UI_OwnerDrawHandleKey(item->window.ownerDraw, item->window.ownerDrawFlags, &item->special, key);
        break;
    case 0xA:
        result = Item_Slider_HandleKey(dc, item, key, down);
        break;
    case 0xB:
        result = Item_YesNo_HandleKey(dc, item, key);
        break;
    case 0xC:
        result = Item_Multi_HandleKey(dc, item, key);
        break;
    case 0xD:
        result = Item_DvarEnum_HandleKey(dc, item, key);
        break;
    case 0xE:
        result = Item_Bind_HandleKey(dc, item, key, down);
        break;
    default:
        return 0;
    }
    return result;
}

int __cdecl Item_YesNo_HandleKey(UiContext *dc, itemDef_s *item, int key)
{
    char *VariantString; // eax
    int v5; // eax
    char *v6; // eax
    bool v7; // [esp+0h] [ebp-28h]
    char v8; // [esp+7h] [ebp-21h]
    char dvarString[28]; // [esp+8h] [ebp-20h] BYREF

    if (!item->dvar)
        return 0;
    if (!Item_ShouldHandleKey(dc, item, key))
        return 0;
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_MOUSE3)
    {
        v8 = 1;
    }
    else
    {
        v7 = key == K_ENTER || key == K_LEFTARROW || key == K_RIGHTARROW || key == K_PGUP || key == K_PGDN;
        v8 = v7;
    }
    if (!v8)
        return 0;
    VariantString = (char *)Dvar_GetVariantString(item->dvar);
    I_strncpyz(dvarString, VariantString, 25);
    v5 = atoi(dvarString);
    v6 = va("%i", v5 == 0);
    Dvar_SetFromStringByName(item->dvar, v6);
    return 1;
}

BOOL __cdecl Item_ContainsMouse(UiContext *dc, itemDef_s *item)
{
    if (!dc->isCursorVisible)
        return 0;
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    return Rect_ContainsPoint(dc->localClientNum, &item->window.rect, dc->cursor.x, dc->cursor.y);
}

bool __cdecl Item_ShouldHandleKey(UiContext *dc, itemDef_s *item, int key)
{
    if (!Window_HasFocus(dc->localClientNum, &item->window))
        return 0;
    return key != K_MOUSE1 && key != K_MOUSE2 && key != K_MOUSE3 || Item_ContainsMouse(dc, item);
}

void __cdecl Item_Action(UiContext *dc, itemDef_s *item)
{
    if (item)
        Item_RunScript(dc, item, (char*)item->action);
}

itemDef_s *__cdecl Menu_SetPrevCursorItem(UiContext *dc, menuDef_t *menu)
{
    int v3; // [esp+Ch] [ebp-40h]
    int v4; // [esp+14h] [ebp-38h]
    int v5; // [esp+1Ch] [ebp-30h]
    int v6; // [esp+24h] [ebp-28h]
    int v7; // [esp+2Ch] [ebp-20h]
    int v8; // [esp+34h] [ebp-18h]
    int v9; // [esp+3Ch] [ebp-10h]
    int localClientNum; // [esp+40h] [ebp-Ch]
    int oldCursor; // [esp+44h] [ebp-8h]
    int wrapped; // [esp+48h] [ebp-4h]

    wrapped = 0;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    oldCursor = menu->cursorItem[localClientNum];
    v9 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v9,
            1);
    if (menu->cursorItem[v9] < 0)
    {
        Menu_SetCursorItem(dc->localClientNum, menu, menu->itemCount - 1);
        wrapped = 1;
    }
    do
    {
        v8 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v8,
                1);
        if (menu->cursorItem[v8] <= -1)
            goto LABEL_27;
        v7 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v7,
                1);
        Menu_SetCursorItem(dc->localClientNum, menu, menu->cursorItem[v7] - 1);
        v6 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v6,
                1);
        if (menu->cursorItem[v6] < 0 && !wrapped)
        {
            wrapped = 1;
            Menu_SetCursorItem(dc->localClientNum, menu, menu->itemCount - 1);
        }
        v5 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v5,
                1);
        if (menu->cursorItem[v5] < 0)
        {
        LABEL_27:
            Menu_SetCursorItem(dc->localClientNum, menu, oldCursor);
            return 0;
        }
        v4 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v4,
                1);
    } while (!Item_SetFocus(dc, menu->items[menu->cursorItem[v4]], dc->cursor.x, dc->cursor.y));
    v3 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v3,
            1);
    return menu->items[menu->cursorItem[v3]];
}

itemDef_s *__cdecl Menu_SetNextCursorItem(UiContext *dc, menuDef_t *menu)
{
    int v3; // [esp+Ch] [ebp-38h]
    int v4; // [esp+14h] [ebp-30h]
    int v5; // [esp+1Ch] [ebp-28h]
    int v6; // [esp+24h] [ebp-20h]
    int v7; // [esp+2Ch] [ebp-18h]
    int v8; // [esp+34h] [ebp-10h]
    int localClientNum; // [esp+38h] [ebp-Ch]
    int oldCursor; // [esp+3Ch] [ebp-8h]
    int wrapped; // [esp+40h] [ebp-4h]

    wrapped = 0;
    localClientNum = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            localClientNum,
            1);
    oldCursor = menu->cursorItem[localClientNum];
    v8 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v8,
            1);
    if (menu->cursorItem[v8] == -1)
    {
        Menu_SetCursorItem(dc->localClientNum, menu, 0);
        wrapped = 1;
    }
    do
    {
        v7 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v7,
                1);
        if (menu->cursorItem[v7] >= menu->itemCount)
        {
            Menu_SetCursorItem(dc->localClientNum, menu, oldCursor);
            return 0;
        }
        v6 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v6,
                1);
        Menu_SetCursorItem(dc->localClientNum, menu, menu->cursorItem[v6] + 1);
        v5 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v5,
                1);
        if (menu->cursorItem[v5] >= menu->itemCount)
        {
            if (wrapped)
                return menu->items[oldCursor];
            wrapped = 1;
            Menu_SetCursorItem(dc->localClientNum, menu, 0);
        }
        v4 = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                v4,
                1);
    } while (!Item_SetFocus(dc, menu->items[menu->cursorItem[v4]], dc->cursor.x, dc->cursor.y));
    v3 = dc->localClientNum;
    if (dc->localClientNum)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
            36,
            0,
            "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
            v3,
            1);
    return menu->items[menu->cursorItem[v3]];
}

rectDef_s *__cdecl Item_CorrectedTextRect(int localClientNum, itemDef_s *item)
{
    rect.x = 0.0;
    rect.y = 0.0;
    rect.w = 0.0;
    rect.h = 0.0;
    rect.horzAlign = 0;
    rect.vertAlign = 0;
    if (item)
    {
        rect = *Item_GetTextRect(localClientNum, item);
        if (rect.w != 0.0)
            rect.y = rect.y - rect.h;
    }
    return &rect;
}

int __cdecl Menu_CheckOnKey(UiContext *dc, menuDef_t *menu, int key)
{
    itemDef_s *item; // [esp+14h] [ebp-184h]
    int i; // [esp+18h] [ebp-180h]
    ItemKeyHandler *handler; // [esp+1Ch] [ebp-17Ch]
    itemDef_s it; // [esp+20h] [ebp-178h] BYREF

    for (handler = menu->onKey; handler; handler = handler->next)
    {
        if (handler->key == key)
        {
        LABEL_5:
            it.parent = menu;
            Item_RunScript(dc, &it, (char*)handler->action);
            return 1;
        }
    }
    for (i = 0; i < menu->itemCount; ++i)
    {
        item = menu->items[i];
        if (Window_IsVisible(dc->localClientNum, &item->window)
            && (Window_HasFocus(dc->localClientNum, &item->window) || (item->window.staticFlags & 0x100000) != 0)
            && Item_IsVisible(dc->localClientNum, item)
            && ((item->dvarFlags & 3) == 0 || Item_EnableShowViaDvar(item, 1)))
        {
            for (handler = item->onKey; handler; handler = handler->next)
            {
                if (handler->key == key)
                    goto LABEL_5;
            }
        }
    }
    return 0;
}

menuDef_t *__cdecl Menu_GetFocused(UiContext *dc)
{
    int i; // [esp+10h] [ebp-4h]

    for (i = dc->openMenuCount - 1; i >= 0; --i)
    {
        if (Window_HasFocus(dc->localClientNum, &dc->menuStack[i]->window)
            && Window_IsVisible(dc->localClientNum, &dc->menuStack[i]->window))
        {
            return dc->menuStack[i];
        }
    }
    return 0;
}

int __cdecl Menus_MenuIsInStack(UiContext *dc, menuDef_t *menu)
{
    PROF_SCOPED("Menus_MenuIsInStack");

    int menuIndex; // [esp+0h] [ebp-4h]

    iassert(dc);
    iassert(menu);

    for (menuIndex = dc->openMenuCount - 1; menuIndex >= 0; --menuIndex)
    {
        if (dc->menuStack[menuIndex] == menu)
            return 1;
    }
    return 0;
}

menuDef_t *__cdecl Menus_FindByName(const UiContext *dc, const char *p)
{
    int i; // [esp+0h] [ebp-4h]

    for (i = 0; i < dc->menuCount; ++i)
    {
        if (!I_stricmp(dc->Menus[i]->window.name, p))
            return dc->Menus[i];
    }
    return 0;
}

void __cdecl Menus_CloseByName(UiContext *dc, const char *p)
{
    menuDef_t *menu; // [esp+0h] [ebp-4h]

    menu = Menus_FindByName(dc, p);
    if (menu)
        Menus_Close(dc, menu);
}

void __cdecl Menus_Close(UiContext *dc, menuDef_t *menu)
{
    bool noFocus; // [esp+1Eh] [ebp-Ah]
    bool hadFocus; // [esp+1Fh] [ebp-9h]
    int menuNum; // [esp+20h] [ebp-8h]
    int menuNuma; // [esp+20h] [ebp-8h]
    int menuNumb; // [esp+20h] [ebp-8h]
    menuDef_t *menuDef; // [esp+24h] [ebp-4h]
    menuDef_t *menuDefa; // [esp+24h] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 934, 0, "%s", "dc");
    if (!menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 935, 0, "%s", "menu");
    if (Menus_MenuIsInStack(dc, menu))
    {
        Menu_RunCloseScript(dc, menu);
        hadFocus = Window_HasFocus(dc->localClientNum, &menu->window);
        Menus_RemoveFromStack(dc, menu);
        if (hadFocus)
        {
            for (menuNum = dc->openMenuCount - 1; menuNum >= 0; --menuNum)
            {
                menuDef = dc->menuStack[menuNum];
                if (Window_IsVisible(dc->localClientNum, &menuDef->window))
                {
                    Menu_GainFocusDueToClose(dc, menuDef);
                    break;
                }
            }
        }
        if (dc->openMenuCount > 0)
        {
            noFocus = 1;
            for (menuNuma = 0; menuNuma < dc->openMenuCount; ++menuNuma)
            {
                menuDefa = dc->menuStack[menuNuma];
                if (Window_IsVisible(dc->localClientNum, &menuDefa->window)
                    && Window_HasFocus(dc->localClientNum, &menuDefa->window))
                {
                    noFocus = 0;
                    break;
                }
            }
            if (noFocus)
            {
                if (!menu->window.name)
                    MyAssertHandler(".\\ui\\ui_shared.cpp", 976, 0, "%s", "menu->window.name");
                Com_PrintWarning(CON_CHANNEL_UI, "WARNING: No menu has focus after closing %s.\n Active menus: \n", menu->window.name);
                for (menuNumb = 0; menuNumb < dc->openMenuCount; ++menuNumb)
                    Com_PrintWarning(CON_CHANNEL_UI, "  %d:  %s\n", menuNumb, dc->menuStack[menuNumb]->window.name);
            }
        }
    }
    Window_RemoveDynamicFlags(dc->localClientNum, &menu->window, 6);
}

int __cdecl Menus_RemoveFromStack(UiContext *dc, menuDef_t *pMenu)
{
    int i; // [esp+4h] [ebp-4h]

    for (i = dc->openMenuCount - 1; ; --i)
    {
        if (i < 0)
            return 0;
        if (dc->menuStack[i] == pMenu)
            break;
    }
    --dc->openMenuCount;
    while (i < dc->openMenuCount)
    {
        dc->menuStack[i] = dc->menuStack[i + 1];
        ++i;
    }
    return 1;
}

void __cdecl Menu_GainFocusDueToClose(UiContext *dc, menuDef_t *menu)
{
    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 904, 0, "%s", "dc");
    if (!menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 905, 0, "%s", "menu");
    if (Window_HasFocus(dc->localClientNum, &menu->window))
        MyAssertHandler(".\\ui\\ui_shared.cpp", 907, 0, "%s", "!Window_HasFocus( dc->localClientNum, &menu->window )");
    Window_AddDynamicFlags(dc->localClientNum, &menu->window, 2);
    Menu_CallOnFocusDueToOpen(dc, menu);
}

void __cdecl Menu_CallOnFocusDueToOpen(UiContext *dc, menuDef_t *menu)
{
    bool anyFound; // [esp+Fh] [ebp-5h]
    int i; // [esp+10h] [ebp-4h]

    anyFound = 0;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
        {
            if (menu->items[i]->onFocus)
                Item_RunScript(dc, menu->items[i], (char *)menu->items[i]->onFocus);
            if (anyFound)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 893, 0, "%s", "!anyFound");
            anyFound = 1;
        }
    }
}

void __cdecl Menu_RunCloseScript(UiContext *dc, menuDef_t *menu)
{
    itemDef_s item; // [esp+4h] [ebp-178h] BYREF

    if (!menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 916, 0, "%s", "menu");
    if (Window_IsVisible(dc->localClientNum, &menu->window))
    {
        if (menu->onClose)
        {
            item.parent = menu;
            Item_RunScript(dc, &item, (char*)menu->onClose);
        }
    }
}

void __cdecl Menus_Open(UiContext *dc, menuDef_t *menu)
{
    menuDef_t *v2; // [esp+4h] [ebp-188h]
    int localClientNum; // [esp+8h] [ebp-184h]
    itemDef_s item; // [esp+Ch] [ebp-180h] BYREF
    int i; // [esp+188h] [ebp-4h]

    for (i = dc->openMenuCount - 1; i >= 0; --i)
        Menu_LoseFocusDueToOpen(dc, dc->menuStack[i]);
    for (i = 0; i < dc->menuCount; ++i)
    {
        v2 = dc->Menus[i];
        localClientNum = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                23,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        if ((v2->window.dynamicFlags[localClientNum] & 2) != 0)
            MyAssertHandler(
                ".\\ui\\ui_shared.cpp",
                4007,
                0,
                "%s",
                "!(Window_GetDynamicFlags( dc->localClientNum, &dc->Menus[i]->window ) & WINDOWDYNAMIC_HASFOCUS)");
    }
    Menus_AddToStack(dc, menu);
    Window_AddDynamicFlags(dc->localClientNum, &menu->window, 6);
    Menu_CallOnFocusDueToOpen(dc, menu);
    if (dc->isCursorVisible)
        Menu_HandleMouseMove(dc, menu);
    if (menu->onOpen)
    {
        item.parent = menu;
        Item_RunScript(dc, &item, (char*)menu->onOpen);
    }
    if (menu->soundName)
        UI_PlayLocalSoundAliasByName(dc->localClientNum, menu->soundName);
}

void __cdecl Menus_AddToStack(UiContext *dc, menuDef_t *pMenu)
{
    Menus_RemoveFromStack(dc, pMenu);
    if (dc->openMenuCount == 16)
        Com_Error(ERR_DROP, "Too many menus opened");
    dc->menuStack[dc->openMenuCount++] = pMenu;
}

void __cdecl Menu_LoseFocusDueToOpen(UiContext *dc, menuDef_t *menu)
{
    bool anyFound; // [esp+1Bh] [ebp-5h]
    int i; // [esp+1Ch] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 849, 0, "%s", "dc");
    if (!menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 850, 0, "%s", "menu");
    if (Window_HasFocus(dc->localClientNum, &menu->window))
    {
        Window_RemoveDynamicFlags(dc->localClientNum, &menu->window, 2);
        anyFound = 0;
        for (i = 0; i < menu->itemCount; ++i)
        {
            if (Window_HasFocus(dc->localClientNum, &menu->items[i]->window))
            {
                if (menu->items[i]->leaveFocus)
                    Item_RunScript(dc, menu->items[i], (char*)menu->items[i]->leaveFocus);
                if (anyFound)
                    MyAssertHandler(".\\ui\\ui_shared.cpp", 868, 0, "%s", "!anyFound");
                anyFound = 1;
            }
        }
    }
}
