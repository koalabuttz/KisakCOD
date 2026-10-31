#include "ui_script_core.h"
#include "ui_input_core.h"
#include <universal/q_shared.h>
#include "ui_shared.h"
#include <universal/q_parse.h>
#include <client/client.h>
#include <qcommon/cmd.h>
#include <stringed/stringed_hooks.h>
#include <win32/win_storage.h>
#include <qcommon/mem_track.h>
#include <database/database.h>
#include <cgame/cg_local.h>

#include <algorithm>
#include <universal/profile.h>

#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif KISAK_SP
#include "ui.h"
#include <cgame/cg_main.h>
#include <gfx_d3d/r_cinematic.h>
#include <game/savedevice.h>
#include <game/savememory.h>
#include <game/g_local.h>

#ifndef KISAK_XBOX
#include <server/server.h>
#endif

#endif





#ifdef KISAK_MP
const commandDef_t commandList[42] =
{
  { "fadein", Script_FadeIn },
  { "fadeout", Script_FadeOut },
  { "show", Script_Show },
  { "hide", Script_Hide },
  { "showMenu", Script_ShowMenu },
  { "hideMenu", Script_HideMenu },
  { "setcolor", Script_SetColor },
  { "open", Script_Open },
  { "close", Script_Close },
  { "ingameopen", Script_InGameOpen },
  { "ingameclose", Script_InGameClose },
  { "setbackground", Script_SetBackground },
  { "setitemcolor", Script_SetItemColor },
  { "focusfirst", Script_FocusFirstInMenu},
  { "setfocus", Script_SetFocus },
  { "setfocusbydvar", Script_SetFocusByDvar },
  { "setdvar", Script_SetDvar },
  { "exec", Script_Exec },
  { "execnow", Script_ExecNow },
  { "execOnDvarStringValue", Script_ExecOnDvarStringValue },
  { "execOnDvarIntValue", Script_ExecOnDvarIntValue },
  { "execOnDvarFloatValue", Script_ExecOnDvarFloatValue },
  { "execNowOnDvarStringValue", Script_ExecNowOnDvarStringValue },
  { "execNowOnDvarIntValue", Script_ExecNowOnDvarIntValue },
  { "execNowOnDvarFloatValue", Script_ExecNowOnDvarFloatValue },
  { "play", Script_Play },
  { "scriptmenuresponse", Script_ScriptMenuResponse },
  { "scriptMenuRespondOnDvarStringValue", Script_RespondOnDvarStringValue },
  { "scriptMenuRespondOnDvarIntValue", Script_RespondOnDvarIntValue },
  { "scriptMenuRespondOnDvarFloatValue", Script_RespondOnDvarFloatValue },
  { "setLocalVarBool", Script_SetLocalVarBool },
  { "setLocalVarInt", Script_SetLocalVarInt },
  { "setLocalVarFloat", Script_SetLocalVarFloat },
  { "setLocalVarString", Script_SetLocalVarString },
  { "feederTop",  Script_FeederTop },
  { "feederBottom",  Script_FeederBottom },
  { "openforgametype", Script_OpenForGameType },
  { "closeforgametype", Script_CloseForGameType },
  { "statclearperknew", Script_StatClearPerkNew },
  { "statsetusingtable", Script_StatSetUsingStatsTable },
  { "statclearbitmask", Script_StatClearBitMask },
  { "getautoupdate", Script_GetAutoUpdate }
}; // idb
#elif KISAK_SP
void Script_SaveGameHide(UiContext *dc, itemDef_s *item, const char **args)
{
    //const char **v6; // r3
    //uint32_t v7; // r11
    //int v8; // r31
    //char v9[1056]; // [sp+50h] [-420h] BYREF
    //
    //v7 = _cntlzw(SaveExists(CONSOLE_DEFAULT_SAVE_NAME));
    //v6 = args;
    //v8 = (v7 >> 5)  1;
    //if (String_Parse(v6, v9, 1024))
    //    Menu_ShowItemByName(dc->localClientNum, item->parent, v9, (uint8_t)v8);

    char parsedName[1024];
#ifdef KISAK_XBOX
    int saveExists = SaveExists(CONSOLE_DEFAULT_SAVE_NAME);
#else
    int saveExists = sv_lastSaveGame->current.string[0] && SaveExists(sv_lastSaveGame->current.string);
#endif
    int shouldHide = !saveExists;  // 1 if save does not exist, 0 otherwise

    if (String_Parse(args, parsedName, sizeof(parsedName))) {
        Menu_ShowItemByName(dc->localClientNum, item->parent, parsedName, (unsigned char)shouldHide);
    }
}
void Script_SaveGameShow(UiContext *dc, itemDef_s *item, const char **args)
{
    const char **v6; // r3
    int v7; // r11
    bool v8; // r31
    char v9[1056]; // [sp+50h] [-420h] BYREF

#ifdef KISAK_XBOX
    v7 = SaveExists(CONSOLE_DEFAULT_SAVE_NAME) - 1;
#else
    v7 = (sv_lastSaveGame->current.string[0] && SaveExists(sv_lastSaveGame->current.string)) - 1;
#endif
    v6 = args;
    v8 = v7 == 0;
    if (String_Parse(v6, v9, 1024))
        Menu_ShowItemByName(dc->localClientNum, item->parent, v9, v8);
}
void Script_ProfileHide(UiContext *dc, itemDef_s *item, const char **args)
{
    //int v6; // r3
    //const char **v7; // r3
    //uint32_t v8; // r11
    //int v9; // r30
    //char v10[1056]; // [sp+50h] [-420h] BYREF
    //
    //v6 = CL_ControllerIndexFromClientNum(dc->localClientNum);
    //v8 = _cntlzw(GamerProfile_IsProfileLoggedIn(v6));
    //v7 = args;
    //v9 = (v8 >> 5) & 1;
    //if (String_Parse(v7, v10, 1024))
    //    Menu_ShowItemByName(dc->localClientNum, item->parent, v10, (uint8_t)v9);
}
void Script_ProfileShow(UiContext *dc, itemDef_s *item, const char **args)
{
    //int v6; // r3
    //bool IsProfileLoggedIn; // r28
    //char v8[1072]; // [sp+50h] [-430h] BYREF
    //
    //v6 = CL_ControllerIndexFromClientNum(dc->localClientNum);
    //IsProfileLoggedIn = GamerProfile_IsProfileLoggedIn(v6);
    //if (String_Parse(args, v8, 1024))
    //    Menu_ShowItemByName(dc->localClientNum, item->parent, v8, IsProfileLoggedIn);
}

void Script_NoSaveHide(UiContext *dc, itemDef_s *item, const char **args)
{
    bool IsProfileLoggedIn; // r30
    int v8; // r3
    char v9[1072]; // [sp+50h] [-430h] BYREF

#ifdef KISAK_XBOX
	int controllerIndex;

	IsProfileLoggedIn = false;

	// leaving this commented out.
    //if (SaveMemory_IsCurrentCommittedSaveValid())
    //{
    //    controllerIndex = CL_ControllerIndexFromClientNum(dc->localClientNum);
    //    IsProfileLoggedIn = GamerProfile_IsProfileLoggedIn(controllerIndex);
    //}
#else
	bool hasSave;

	hasSave = SaveMemory_IsCurrentCommittedSaveValid();
	if (!hasSave && sv_lastSaveGame->current.string[0])
		hasSave = SaveExists(sv_lastSaveGame->current.string);

	IsProfileLoggedIn = hasSave;
#endif

    if (String_Parse(args, v9, 1024))
        Menu_ShowItemByName(dc->localClientNum, item->parent, v9, IsProfileLoggedIn);
}

void Script_SaveAvailableHide(UiContext *dc, itemDef_s *item, const char **args)
{
    bool IsProfileLoggedIn; // r30
    int v8; // r3
    char v9[1072]; // [sp+50h] [-430h] BYREF

#ifdef KISAK_XBOX
	int controllerIndex;

	IsProfileLoggedIn = false;

	// leaving this commented out.
    //if (SaveMemory_IsCurrentCommittedSaveValid())
    //{
    //    v8 = CL_ControllerIndexFromClientNum(dc->localClientNum);
    //    IsProfileLoggedIn = GamerProfile_IsProfileLoggedIn(v8);
    //}
#else
	bool hasSave;

	hasSave = SaveMemory_IsCurrentCommittedSaveValid();
	if (!hasSave && sv_lastSaveGame->current.string[0])
		hasSave = SaveExists(sv_lastSaveGame->current.string);

	IsProfileLoggedIn = hasSave;
#endif

    if (String_Parse(args, v9, 1024))
        Menu_ShowItemByName(dc->localClientNum, item->parent, v9, !IsProfileLoggedIn);
}

void Script_DisplaySaveMessage(UiContext *dc, itemDef_s *item, const char **args)
{
    iassert(item);
    iassert(item->parent);
    iassert(item->parent->window.name);

    Dvar_SetBool(ui_isSaving, 1);
    ui_saveTimeGlob.isSaving = 1;
    ui_saveTimeGlob.saveTime = Sys_Milliseconds();
    ui_saveTimeGlob.hasfirstFrameShown = 0;
    ui_saveTimeGlob.saveMenuName = item->parent->window.name;
    Com_Printf(CON_CHANNEL_UI, "Save message opened: %i\n", ui_saveTimeGlob.saveTime);
}

void Script_WriteSave(UiContext *dc, itemDef_s *item, const char **args)
{
    ui_saveTimeGlob.callWrite = 1;
}

void Script_SetSaveExecOnSuccess(UiContext *dc, itemDef_s *item, const char **args)
{
    ui_saveTimeGlob.hasExecOnSuccess = 1;
    String_Parse(args, ui_saveTimeGlob.execOnSuccess, 256);
}

void Script_ScriptNextLevel(UiContext *dc, itemDef_s *item, const char **args)
{
    G_LoadNextMap();
}

const commandDef_t commandList[46] =
{
  { "fadein", Script_FadeIn },
  { "fadeout", Script_FadeOut },
  { "show", Script_Show },
  { "hide", Script_Hide },
  { "showMenu", Script_ShowMenu },
  { "hideMenu", Script_HideMenu },
  { "setcolor", Script_SetColor },
  { "open", Script_Open },
  { "close", Script_Close },
  { "ingameopen", Script_InGameOpen },
  { "ingameclose", Script_InGameClose },
  { "setbackground", Script_SetBackground },
  { "setitemcolor", Script_SetItemColor },
  { "focusfirst", Script_FocusFirstInMenu },
  { "setfocus", Script_SetFocus },
  { "setfocusbydvar", Script_SetFocusByDvar },
  { "setdvar", Script_SetDvar },
  { "exec", Script_Exec },
  { "execnow", Script_ExecNow },
  { "execOnDvarStringValue", Script_ExecOnDvarStringValue },
  { "execOnDvarIntValue", Script_ExecOnDvarIntValue },
  { "execOnDvarFloatValue", Script_ExecOnDvarFloatValue },
  { "execNowOnDvarStringValue", Script_ExecNowOnDvarStringValue },
  { "execNowOnDvarIntValue", Script_ExecNowOnDvarIntValue },
  { "execNowOnDvarFloatValue", Script_ExecNowOnDvarFloatValue },
  { "play", Script_Play },
  { "scriptmenuresponse", Script_ScriptMenuResponse },
  { "scriptMenuRespondOnDvarStringValue", Script_RespondOnDvarStringValue },
  { "scriptMenuRespondOnDvarIntValue", Script_RespondOnDvarIntValue },
  { "scriptMenuRespondOnDvarFloatValue", Script_RespondOnDvarFloatValue },
  { "setLocalVarBool", Script_SetLocalVarBool },
  { "setLocalVarInt", Script_SetLocalVarInt },
  { "setLocalVarFloat", Script_SetLocalVarFloat },
  { "setLocalVarString", Script_SetLocalVarString },
  { "feederTop", Script_FeederTop },
  { "feederBottom", Script_FeederBottom },
  { "savegamehide", Script_SaveGameHide },
  { "savegameshow", Script_SaveGameShow },
  { "profilehide", Script_ProfileHide },
  { "profileshow", Script_ProfileShow },
  { "nosavehide", Script_NoSaveHide },
  { "saveAvailableHide", Script_SaveAvailableHide },
  { "saveDelay", Script_DisplaySaveMessage },
  { "writeSave", Script_WriteSave },
  { "setSaveExecOnSuccess", Script_SetSaveExecOnSuccess },
  { "nextlevel", Script_ScriptNextLevel }
};
#endif

#ifdef KISAK_MP
void __cdecl Script_GetAutoUpdate(UiContext *dc, itemDef_s *item, const char **args)
{
    CL_GetAutoUpdate();
}
#endif

void __cdecl Script_StatClearPerkGetArg(
    UiContext *dc,
    itemDef_s *item,
    const char **args,
    char *refString,
    int refStringLen)
{
    char arg[1028]; // [esp+0h] [ebp-408h] BYREF

    if (!refString)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 1257, 0, "%s", "refString");
    String_Parse(args, arg, 1024);
    if (!I_stricmp(arg, "("))
        String_Parse(args, arg, 1024);
    if (!I_stricmp(arg, "\""))
        String_Parse(args, arg, 1024);
    I_strncpyz(refString, arg, refStringLen);
    if (!I_stricmp(arg, "\""))
        String_Parse(args, arg, 1024);
    if (!I_stricmp(arg, ")"))
        Com_UngetToken();
}

void __cdecl Script_StatClearPerkNew(UiContext *dc, itemDef_s *item, const char **args)
{
    const char *v3; // eax
    int v4; // eax
    int v5; // eax
    int v6; // [esp-8h] [ebp-420h]
    int v7; // [esp-4h] [ebp-41Ch]
    uint32_t v8; // [esp-4h] [ebp-41Ch]
    StringTable *table; // [esp+4h] [ebp-414h] BYREF
    int perkIndex; // [esp+8h] [ebp-410h]
    int statValue; // [esp+Ch] [ebp-40Ch]
    char refString[1028]; // [esp+10h] [ebp-408h] BYREF

#ifdef KISAK_NO_FASTFILES
    if (true)
#else
    if (IsFastFileLoad())
#endif
    {
        Script_StatClearPerkGetArg(dc, item, args, refString, 1024);
        StringTable_GetAsset("mp/statstable.csv", &table);
        v3 = StringTable_Lookup(table, 4, refString, 1);
        perkIndex = atoi(v3);
        if (perkIndex >= 150 && perkIndex <= 298)
        {
            v7 = perkIndex;
            v4 = CL_ControllerIndexFromClientNum(dc->localClientNum);
            statValue = LiveStorage_GetStat(v4, v7);
            if ((statValue & 3) != 0)
            {
                statValue &= ~2u;
                statValue |= 1u;
                v8 = statValue;
                v6 = perkIndex;
                v5 = CL_ControllerIndexFromClientNum(dc->localClientNum);
                LiveStorage_SetStat(v5, v6, v8);
            }
            else
            {
                Com_Error(ERR_DROP, "statClearPerkNew: perk %s[%d] isn't unlocked.\n", refString, perkIndex);
            }
        }
        else
        {
            Com_Error(ERR_DROP, "statClearPerkNew: Invalid perk index %d for %s\n", perkIndex, refString);
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_UI, "You can only do table lookups when using fastfiles.\n");
    }
}

void __cdecl Menu_Setup(UiContext *dc)
{
    dc->menuCount = 0;
    dc->openMenuCount = 0;
    Item_SetupKeywordHash();
    Menu_SetupKeywordHash();
    UILocalVar_Init(&dc->localVars);
}

void __cdecl Menu_FreeMemory(menuDef_t *menu)
{
    int item; // [esp+0h] [ebp-4h]

    for (item = 0; item < menu->itemCount; ++item)
        Menu_FreeItemMemory(menu->items[item]);
    free_expression(&menu->visibleExp);
    free_expression(&menu->rectXExp);
    free_expression(&menu->rectYExp);
}

void __cdecl Menus_FreeAllMemory(UiContext *dc)
{
    int menu; // [esp+0h] [ebp-4h]

    for (menu = 0; menu < dc->menuCount; ++menu)
        Menu_FreeMemory(dc->Menus[menu]);
}



int __cdecl Color_Parse(const char **p, float (*c)[4])
{
    float f; // [esp+0h] [ebp-8h] BYREF
    int i; // [esp+4h] [ebp-4h]

    for (i = 0; i < 4; ++i)
    {
        if (!Float_Parse(p, &f))
            return 0;
        (*c)[i] = f;
    }
    return 1;
}


void __cdecl Script_SetColor(UiContext *dc, itemDef_s *item, const char **args)
{
    float *out; // [esp+0h] [ebp-414h]
    char name[1028]; // [esp+4h] [ebp-410h] BYREF
    float f; // [esp+40Ch] [ebp-8h] BYREF
    int i; // [esp+410h] [ebp-4h]

    if (String_Parse(args, name, 1024))
    {
        out = 0;
        if (I_stricmp(name, "backcolor"))
        {
            if (I_stricmp(name, "forecolor"))
            {
                if (!I_stricmp(name, "bordercolor"))
                    out = item->window.borderColor;
            }
            else
            {
                out = item->window.foreColor;
                Window_AddDynamicFlags(dc->localClientNum, &item->window, 0x10000);
            }
        }
        else
        {
            out = item->window.backColor;
            Window_AddDynamicFlags(dc->localClientNum, &item->window, 0x8000);
        }
        if (out)
        {
            for (i = 0; i < 4 && Float_Parse(args, &f); ++i)
                out[i] = f;
        }
    }
}

void __cdecl Script_SetBackground(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        item->window.background = Material_RegisterHandle(name, item->imageTrack);
}

void __cdecl Script_SetItemColor(UiContext *dc, itemDef_s *item, const char **args)
{
    int j; // [esp+0h] [ebp-828h]
    itemDef_s *item2; // [esp+4h] [ebp-824h]
    int count; // [esp+8h] [ebp-820h]
    float *out; // [esp+Ch] [ebp-81Ch]
    char itemname[1024]; // [esp+10h] [ebp-818h] BYREF
    char name[1024]; // [esp+410h] [ebp-418h] BYREF
    int i; // [esp+814h] [ebp-14h]
    float color[4]; // [esp+818h] [ebp-10h] BYREF

    if (String_Parse(args, itemname, 1024))
    {
        if (String_Parse(args, name, 1024))
        {
            count = Menu_ItemsMatchingGroup(item->parent, itemname);
            if (Color_Parse(args, (float (*)[4])color))
            {
                for (j = 0; j < count; ++j)
                {
                    item2 = Menu_GetMatchingItemByNumber(item->parent, j, itemname);
                    if (item2)
                    {
                        out = 0;
                        if (I_stricmp(name, "backcolor"))
                        {
                            if (I_stricmp(name, "forecolor"))
                            {
                                if (!I_stricmp(name, "bordercolor"))
                                    out = item2->window.borderColor;
                            }
                            else
                            {
                                out = item2->window.foreColor;
                                Window_AddDynamicFlags(dc->localClientNum, &item2->window, 0x10000);
                            }
                        }
                        else
                        {
                            out = item2->window.backColor;
                        }
                        if (out)
                        {
                            for (i = 0; i < 4; ++i)
                                out[i] = color[i];
                        }
                    }
                }
            }
        }
    }
}

int __cdecl Menu_ItemsMatchingGroup(menuDef_t *menu, char *name)
{
    int v2; // eax
    int wildcard; // [esp+4h] [ebp-Ch]
    int i; // [esp+8h] [ebp-8h]
    int count; // [esp+Ch] [ebp-4h]

    count = 0;
    wildcard = -1;
    v2 = (int)strstr(name, "*");
    if (v2)
        wildcard = v2 - (uint32_t)name;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (wildcard == -1)
        {
            if (menu->items[i]->window.name && !I_stricmp(menu->items[i]->window.name, name)
                || menu->items[i]->window.group && !I_stricmp(menu->items[i]->window.group, name))
            {
                ++count;
            }
        }
        else if (menu->items[i]->window.name && !I_strncmp(menu->items[i]->window.name, name, wildcard)
            || menu->items[i]->window.group && !I_strncmp(menu->items[i]->window.group, name, wildcard))
        {
            ++count;
        }
    }
    return count;
}

itemDef_s *__cdecl Menu_GetMatchingItemByNumber(menuDef_t *menu, int index, char *name)
{
    int v3; // eax
    int wildcard; // [esp+4h] [ebp-Ch]
    int i; // [esp+8h] [ebp-8h]
    int count; // [esp+Ch] [ebp-4h]

    count = 0;
    wildcard = -1;
    v3 = (int)strstr(name, "*");
    if (v3)
        wildcard = v3 - (uint32_t)name;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (wildcard == -1)
        {
            if (menu->items[i]->window.name && !I_stricmp(menu->items[i]->window.name, name)
                || menu->items[i]->window.group && !I_stricmp(menu->items[i]->window.group, name))
            {
                if (count == index)
                    return menu->items[i];
                ++count;
            }
        }
        else if (menu->items[i]->window.name && !I_strncmp(menu->items[i]->window.name, name, wildcard)
            || menu->items[i]->window.group && !I_strncmp(menu->items[i]->window.group, name, wildcard))
        {
            if (count == index)
                return menu->items[i];
            ++count;
        }
    }
    return 0;
}

void __cdecl Menus_HideByName(const UiContext *dc, const char *menuName)
{
    menuDef_t *menu; // [esp+0h] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 820, 0, "%s", "dc");
    if (!menuName)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 821, 0, "%s", "menuName");
    menu = Menus_FindByName(dc, menuName);
    if (menu)
        Window_RemoveDynamicFlags(dc->localClientNum, &menu->window, 4);
}

void __cdecl Menus_ShowByName(const UiContext *dc, const char *windowName)
{
    menuDef_t *menu; // [esp+0h] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 833, 0, "%s", "dc");
    if (!windowName)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 834, 0, "%s", "windowName");
    menu = Menus_FindByName(dc, windowName);
    if (menu)
        Window_AddDynamicFlags(dc->localClientNum, &menu->window, 4);
}

itemDef_s *__cdecl Menu_FindItemByName(menuDef_t *menu, const char *p)
{
    int i; // [esp+0h] [ebp-4h]

    if (!menu || !p)
        return 0;
    for (i = 0; i < menu->itemCount; ++i)
    {
        if (menu->items[i]->window.name && !I_stricmp(p, menu->items[i]->window.name))
            return menu->items[i];
    }
    return 0;
}

void __cdecl Menus_CloseAll(UiContext *dc)
{
    int i; // [esp+0h] [ebp-4h]

    for (i = 0; i < dc->menuCount; ++i)
        Menus_Close(dc, dc->Menus[i]);
}

void __cdecl Script_Show(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menu_ShowItemByName(dc->localClientNum, item->parent, name, 1);
}

void __cdecl Menu_ShowItemByName(int localClientNum, menuDef_t *menu, char *p, int bShow)
{
    itemDef_s *item; // [esp+0h] [ebp-Ch]
    int i; // [esp+4h] [ebp-8h]
    int count; // [esp+8h] [ebp-4h]

    count = Menu_ItemsMatchingGroup(menu, p);
    for (i = 0; i < count; ++i)
    {
        item = Menu_GetMatchingItemByNumber(menu, i, p);
        if (item)
        {
            if (bShow)
                Window_AddDynamicFlags(localClientNum, &item->window, 4);
            else
                Window_RemoveDynamicFlags(localClientNum, &item->window, 4);
        }
    }
}

void __cdecl Script_Hide(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menu_ShowItemByName(dc->localClientNum, item->parent, name, 0);
}

void __cdecl Script_FadeIn(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menu_FadeItemByName(dc->localClientNum, item->parent, name, 0);
}

void __cdecl Menu_FadeItemByName(int localClientNum, menuDef_t *menu, char *p, int fadeOut)
{
    itemDef_s *item; // [esp+0h] [ebp-Ch]
    int i; // [esp+4h] [ebp-8h]
    int count; // [esp+8h] [ebp-4h]

    count = Menu_ItemsMatchingGroup(menu, p);
    for (i = 0; i < count; ++i)
    {
        item = Menu_GetMatchingItemByNumber(menu, i, p);
        if (item)
        {
            if (fadeOut)
            {
                Window_AddDynamicFlags(localClientNum, &item->window, 20);
                Window_RemoveDynamicFlags(localClientNum, &item->window, 32);
            }
            else
            {
                Window_AddDynamicFlags(localClientNum, &item->window, 36);
                Window_RemoveDynamicFlags(localClientNum, &item->window, 16);
            }
        }
    }
}

void __cdecl Script_FadeOut(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menu_FadeItemByName(dc->localClientNum, item->parent, name, 1);
}

void __cdecl Script_ShowMenu(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menus_ShowByName(dc, name);
}

void __cdecl Script_HideMenu(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menus_HideByName(dc, name);
}

void __cdecl Script_Open(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
        Menus_OpenByName(dc, name);
}

void __cdecl Script_OpenForGameType(UiContext *dc, itemDef_s *item, const char **args)
{
    const char *String; // eax
    const char *v4; // eax
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
    {
        String = Dvar_GetString(item->dvar);
        v4 = va(name, String);
        Menus_OpenByName(dc, v4);
    }
}

void __cdecl Script_CloseForGameType(UiContext *dc, itemDef_s *item, const char **args)
{
    const char *String; // eax
    const char *v4; // eax
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
    {
        String = Dvar_GetString(item->dvar);
        v4 = va(name, String);
        Menus_CloseByName(dc, v4);
    }
}

void __cdecl Script_ValidateStat(int controllerIndex, int index, int value)
{
    if (index < 2400 || index > 2402)
    {
        if (index < 200 || index >= 250)
            Com_Error(ERR_DROP, "statsetusingtable should only be used for create-a-class.  You can't set stat %d\n", index);
        LiveStorage_ValidateCaCStat(controllerIndex, index, value);
    }
}

void __cdecl Script_StatSetUsingStatsTable(UiContext *dc, itemDef_s *item, const char **args)
{
    const char *v3; // eax
    int v4; // eax
    int v5; // eax
    int v6; // [esp-8h] [ebp-824h]
    int v7; // [esp-8h] [ebp-824h]
    int v8; // [esp-4h] [ebp-820h]
    uint32_t v9; // [esp-4h] [ebp-820h]
    char searchValue[1024]; // [esp+4h] [ebp-818h] BYREF
    int statNum; // [esp+404h] [ebp-418h]
    StringTable *table; // [esp+408h] [ebp-414h] BYREF
    int returnValueColumn; // [esp+40Ch] [ebp-410h]
    int comparisonColumn; // [esp+410h] [ebp-40Ch]
    char arg[1024]; // [esp+414h] [ebp-408h] BYREF
    int newStatValue; // [esp+818h] [ebp-4h]

#ifdef KISAK_NO_FASTFILES
    if (true)
#else
    if (IsFastFileLoad())
#endif
    {
        String_Parse(args, arg, 1024);
        if (!I_stricmp(arg, "("))
            String_Parse(args, arg, 1024);
        statNum = atoi(arg);
        String_Parse(args, arg, 1024);
        while (!I_stricmp(arg, "tablelookup") || !I_stricmp(arg, ",") || !I_stricmp(arg, "("))
            String_Parse(args, arg, 1024);
        StringTable_GetAsset(arg, &table);
        String_Parse(args, arg, 1024);
        while (!I_stricmp(arg, ","))
            String_Parse(args, arg, 1024);
        comparisonColumn = atoi(arg);
        String_Parse(args, arg, 1024);
        while (!I_stricmp(arg, ","))
            String_Parse(args, arg, 1024);
        I_strncpyz(searchValue, arg, 1024);
        String_Parse(args, arg, 1024);
        while (!I_stricmp(arg, ","))
            String_Parse(args, arg, 1024);
        returnValueColumn = atoi(arg);
        v3 = StringTable_Lookup(table, comparisonColumn, searchValue, returnValueColumn);
        newStatValue = atoi(v3);
        v8 = newStatValue;
        v6 = statNum;
        v4 = CL_ControllerIndexFromClientNum(dc->localClientNum);
        Script_ValidateStat(v4, v6, v8);
        v9 = newStatValue;
        v7 = statNum;
        v5 = CL_ControllerIndexFromClientNum(dc->localClientNum);
        LiveStorage_SetStat(v5, v7, v9);
        String_Parse(args, arg, 1024);
        while (!I_stricmp(arg, ")"))
            String_Parse(args, arg, 1024);
        Com_UngetToken();
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_UI, "You can only do table lookups when using fastfiles.\n");
    }
}

void __cdecl Script_StatClearBitMask(UiContext *dc, itemDef_s *item, const char **args)
{
    int bitMask; // [esp+0h] [ebp-14h] BYREF
    int oldStatValue; // [esp+4h] [ebp-10h]
    int controllerIndex; // [esp+8h] [ebp-Ch]
    int statNum; // [esp+Ch] [ebp-8h] BYREF
    int newStatValue; // [esp+10h] [ebp-4h]

    Script_StatBitMaskGetArgs(dc, item, args, &statNum, &bitMask);
    controllerIndex = CL_ControllerIndexFromClientNum(dc->localClientNum);
    oldStatValue = LiveStorage_GetStat(controllerIndex, statNum);
    newStatValue = oldStatValue & ~bitMask;
    LiveStorage_SetStat(controllerIndex, statNum, newStatValue);
}

void __cdecl Script_StatBitMaskGetArgs(UiContext *dc, itemDef_s *item, const char **args, int *statNum, int *bitMask)
{
    char arg[1028]; // [esp+0h] [ebp-408h] BYREF

    if (!statNum)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 1229, 0, "%s", "statNum");
    if (!bitMask)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 1230, 0, "%s", "bitMask");
    String_Parse(args, arg, 1024);
    if (!I_stricmp(arg, "("))
        String_Parse(args, arg, 1024);
    *statNum = Dvar_GetInt(arg);
    String_Parse(args, arg, 1024);
    while (!I_stricmp(arg, ","))
        String_Parse(args, arg, 1024);
    *bitMask = Dvar_GetInt(arg);
    String_Parse(args, arg, 1024);
    while (!I_stricmp(arg, ")"))
        String_Parse(args, arg, 1024);
    Com_UngetToken();
}

void __cdecl Script_InGameOpen(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
    {
        if (CL_IsLocalClientInGame(dc->localClientNum))
            Menus_OpenByName(dc, name);
    }
}

void __cdecl Script_InGameClose(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
    {
        if (CL_IsLocalClientInGame(dc->localClientNum))
            Menus_CloseByName(dc, name);
    }
}

void __cdecl Script_FocusFirstInMenu(UiContext *dc, itemDef_s *item, const char** args)
{
    const char *name; // [esp+0h] [ebp-Ch]
    itemDef_s *focusItem; // [esp+8h] [ebp-4h]

    focusItem = Menu_FocusFirstSelectableItem(dc, item->parent);
    if (focusItem)
    {
        if (Item_IsTextField(focusItem))
            Item_TextField_BeginEdit(dc->localClientNum, focusItem);
    }
    else if (item->parent->window.name)
    {
        Com_Printf(CON_CHANNEL_UI, "focusFirst: no itemDefs in %s were selectable\n", item->parent->window.name);
    }
    else
    {
        if (item->window.name)
            name = item->window.name;
        else
            name = "itemDef's unnamed menu";
        Com_Printf(CON_CHANNEL_UI, "focusFirst: no itemDefs in %s were selectable\n", name);
    }
}

int __cdecl Item_ListBox_OverLB(int localClientNum, itemDef_s *item, float x, float y)
{
    float thumbstart; // [esp+Ch] [ebp-28h]
    float thumbstarta; // [esp+Ch] [ebp-28h]
    rectDef_s r; // [esp+10h] [ebp-24h] BYREF
    const rectDef_s *rect; // [esp+2Ch] [ebp-8h]
    int count; // [esp+30h] [ebp-4h]

    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2467, 0, "%s", "item");
    count = UI_FeederCount(localClientNum, item->special);
    if (!Item_GetListBoxDef(item))
        return 0;
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    r.horzAlign = item->window.rect.horzAlign;
    r.vertAlign = item->window.rect.vertAlign;
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
    if ((item->window.staticFlags & 0x200000) != 0)
    {
        r.x = rect->x;
        r.y = rect->y + rect->h - 16.0;
        r.w = 16.0;
        r.h = 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 256;
        r.x = rect->x + rect->w - 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 512;
        thumbstart = Item_ListBox_ThumbPosition(localClientNum, item);
        r.x = thumbstart;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 1024;
        r.x = rect->x + 16.0;
        r.w = thumbstart - r.x;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 2048;
        r.x = thumbstart + 16.0;
        r.w = rect->x + rect->w - 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 4096;
    }
    else
    {
        r.x = rect->x + rect->w - 16.0;
        r.y = rect->y;
        r.w = 16.0;
        r.h = 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 256;
        r.y = rect->y + rect->h - 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 512;
        thumbstarta = Item_ListBox_ThumbPosition(localClientNum, item);
        r.y = thumbstarta;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 1024;
        r.y = rect->y + 16.0;
        r.h = thumbstarta - r.y;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 2048;
        r.y = thumbstarta + 16.0;
        r.h = rect->y + rect->h - 16.0;
        if (Rect_ContainsPoint(localClientNum, &r, x, y))
            return 4096;
    }
    return 0;
}

void __cdecl Item_ListBox_MouseEnter(int localClientNum, itemDef_s *item, float x, float y)
{
    int v4; // eax
    int v5; // [esp+8h] [ebp-68h]
    int v6; // [esp+Ch] [ebp-64h]
    float v7; // [esp+18h] [ebp-58h]
    float v8; // [esp+34h] [ebp-3Ch]
    rectDef_s r; // [esp+4Ch] [ebp-24h] BYREF
    listBoxDef_s *listPtr; // [esp+64h] [ebp-Ch]
    int mousePos; // [esp+68h] [ebp-8h]
    const rectDef_s *rect; // [esp+6Ch] [ebp-4h]

    listPtr = Item_GetListBoxDef(item);
    if (listPtr)
    {
        Window_RemoveDynamicFlags(localClientNum, &item->window, 7936);
        v4 = Item_ListBox_OverLB(localClientNum, item, x, y);
        Window_AddDynamicFlags(localClientNum, &item->window, v4);
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        rect = &item->window.rect;
        r.horzAlign = item->window.rect.horzAlign;
        r.vertAlign = item->window.rect.vertAlign;
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
        if ((item->window.staticFlags & 0x200000) != 0)
        {
            if (localClientNum)
                MyAssertHandler(
                    "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                    23,
                    0,
                    "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                    localClientNum,
                    1);
            if ((item->window.dynamicFlags[localClientNum] & 0x1F00) == 0 && listPtr->elementStyle == 1)
            {
                r.x = rect->x;
                r.y = rect->y;
                r.h = rect->h - 16.0;
                r.w = rect->w - (double)listPtr->drawPadding;
                if (Rect_ContainsPoint(localClientNum, &r, x, y))
                {
                    v8 = (x - r.x) / listPtr->elementWidth;
                    mousePos = (int)(v8 - 0.4999999990686774) + listPtr->startPos[localClientNum];
                    if (listPtr->endPos[localClientNum] < mousePos)
                        v6 = listPtr->endPos[localClientNum];
                    else
                        v6 = mousePos;
                    listPtr->mousePos = v6;
                }
            }
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
            if ((item->window.dynamicFlags[localClientNum] & 0x1F00) == 0)
            {
                r.x = rect->x;
                r.y = rect->y + 2.0;
                r.w = rect->w - 16.0;
                r.h = rect->h - (double)listPtr->drawPadding;
                if (Rect_ContainsPoint(localClientNum, &r, x, y))
                {
                    v7 = (y - r.y) / listPtr->elementHeight;
                    mousePos = (int)(v7 - 0.4999999990686774) + listPtr->startPos[localClientNum];
                    if (listPtr->endPos[localClientNum] < mousePos)
                        v5 = listPtr->endPos[localClientNum];
                    else
                        v5 = mousePos;
                    listPtr->mousePos = v5;
                }
            }
        }
    }
}

itemDef_s *__cdecl Menu_FocusFirstSelectableItem(UiContext *dc, menuDef_t *menu)
{
    int localClientNum; // [esp+8h] [ebp-8h]
    uint32_t cursor; // [esp+Ch] [ebp-4h]
    int cursora; // [esp+Ch] [ebp-4h]

    if (Menu_HandleMouseMove(dc, menu))
    {
        localClientNum = dc->localClientNum;
        if (dc->localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                36,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        cursor = menu->cursorItem[localClientNum];
        if (cursor >= menu->itemCount)
            MyAssertHandler(
                ".\\ui\\ui_shared.cpp",
                1387,
                0,
                "cursor doesn't index menu->itemCount\n\t%i not in [0, %i)",
                cursor,
                menu->itemCount);
        return menu->items[cursor];
    }
    else
    {
        cursora = 0;
        Menu_SetCursorItem(dc->localClientNum, menu, 0);
        while (cursora < menu->itemCount)
        {
            if (Item_SetFocus(dc, menu->items[cursora], dc->cursor.x, dc->cursor.y))
                return menu->items[cursora];
            Menu_SetCursorItem(dc->localClientNum, menu, ++cursora);
        }
        Menu_SetCursorItem(dc->localClientNum, menu, 0);
        return 0;
    }
}

void __cdecl Script_SetFocus(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+8h] [ebp-410h] BYREF
    itemDef_s *focusItem; // [esp+410h] [ebp-8h]
    const rectDef_s *rect; // [esp+414h] [ebp-4h]

    if (String_Parse(args, name, 1024))
    {
        focusItem = Menu_FindItemByName(item->parent, name);
        if (focusItem)
        {
            rect = &focusItem->window.rect;
            if (Item_SetFocus(dc, focusItem, focusItem->window.rect.x, focusItem->window.rect.y))
            {
                if (Item_IsTextField(focusItem))
                    Item_TextField_BeginEdit(dc->localClientNum, focusItem);
            }
            else
            {
                Com_PrintError(CON_CHANNEL_UI, "setFocus: error focusing widget '%s' (widget was found but could not accept focus)\n", name);
            }
        }
        else
        {
            Com_Printf(CON_CHANNEL_UI, "setFocus: could not find widget named '%s'\n", name);
        }
    }
}

void __cdecl Script_SetFocusByDvar(UiContext *dc, itemDef_s *item, const char **args)
{
    itemDef_s *focusItem; // [esp+8h] [ebp-418h]
    menuDef_t *parent; // [esp+Ch] [ebp-414h]
    char dvarName[1028]; // [esp+10h] [ebp-410h] BYREF
    int i; // [esp+418h] [ebp-8h]
    const rectDef_s *rect; // [esp+41Ch] [ebp-4h]

    if (String_Parse(args, dvarName, 1024))
    {
        parent = item->parent;
        if (!parent)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 1463, 0, "%s", "parent");
        for (i = 0; i < parent->itemCount; ++i)
        {
            focusItem = parent->items[i];
            if ((focusItem->dvarFlags & 0x10) != 0)
            {
                if (!focusItem->dvarTest)
                    Com_Error(ERR_DROP, "cript_SetFocusByDvar: Item's dvarTest field is empty.");
                if (!I_stricmp(focusItem->dvarTest, dvarName) && Item_EnableShowViaDvar(focusItem, 16))
                {
                    if (!focusItem)
                        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
                    rect = &focusItem->window.rect;
                    if (Item_SetFocus(dc, focusItem, focusItem->window.rect.x, focusItem->window.rect.y))
                        break;
                }
            }
        }
    }
}

void __cdecl Script_SetDvar(UiContext *dc, itemDef_s *item, const char **args)
{
    char val[1024]; // [esp+0h] [ebp-808h] BYREF
    char dvarName[1028]; // [esp+400h] [ebp-408h] BYREF

    if (String_Parse(args, dvarName, 1024))
    {
        if (String_Parse(args, val, 1024))
            Dvar_SetFromStringByName(dvarName, val);
    }
}

void __cdecl Script_Exec(UiContext *dc, itemDef_s *item, const char **args)
{
    int v3; // eax

    v3 = CL_ControllerIndexFromClientNum(dc->localClientNum);
    Script_ExecHandler(dc->localClientNum, v3, item, args, Script_AddTextWrapper);
}

void __cdecl Script_ExecHandler(
    int localClientNum,
    int controllerIndex,
    itemDef_s *item,
    const char **args,
    void(__cdecl *textCallback)(int, int, const char *))
{
    char val[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, val, 1023))
    {
        I_strncat(val, 1024, "\n");
        textCallback(localClientNum, controllerIndex, val);
    }
}

void __cdecl Script_AddTextWrapper(int clientNum, int controllerIndex, const char *text)
{
    Cbuf_AddText(clientNum, text);
}

void __cdecl Script_ExecNow(UiContext *dc, itemDef_s *item, const char **args)
{
    int v3; // eax

    v3 = CL_ControllerIndexFromClientNum(dc->localClientNum);
    Script_ExecHandler(dc->localClientNum, v3, item, args, (void(__cdecl *)(int, int, const char *))Cbuf_ExecuteBuffer);
}

void __cdecl Script_ExecOnDvarStringValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(dc->localClientNum, item, args, Script_ExecIfStringsEqual, Script_AddTextWrapper);
}

void __cdecl Script_ConditionalExecHandler(
    int localClientNum,
    itemDef_s *item,
    const char **args,
    bool(__cdecl *shouldExec)(const char *, const char *),
    void(__cdecl *textCallback)(int, int, const char *))
{
    int v5; // eax
    char testValue[1024]; // [esp+0h] [ebp-C08h] BYREF
    char dvarName[1024]; // [esp+400h] [ebp-808h] BYREF
    char command[1024]; // [esp+800h] [ebp-408h] BYREF
    const char *dvarValue; // [esp+C04h] [ebp-4h]

    if (String_Parse(args, dvarName, 1024) && String_Parse(args, testValue, 1024) && String_Parse(args, command, 1023))
    {
        dvarValue = Dvar_GetVariantString(dvarName);
        if (shouldExec(dvarValue, testValue))
        {
            I_strncat(command, 1024, "\n");
            v5 = CL_ControllerIndexFromClientNum(localClientNum);
            textCallback(localClientNum, v5, command);
        }
    }
}

bool __cdecl Script_ExecIfStringsEqual(const char *dvarValue, const char *testValue)
{
    return I_stricmp(dvarValue, testValue) == 0;
}

void __cdecl Script_ExecOnDvarIntValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(dc->localClientNum, item, args, Script_ExecIfIntsEqual, Script_AddTextWrapper);
}

bool __cdecl Script_ExecIfIntsEqual(const char *dvarValue, const char *testValue)
{
    int v2; // esi

    v2 = atoi(dvarValue);
    return v2 == atoi(testValue);
}

void __cdecl Script_ExecOnDvarFloatValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(dc->localClientNum, item, args, Script_ExecIfFloatsEqual, Script_AddTextWrapper);
}

bool __cdecl Script_ExecIfFloatsEqual(const char *dvarValue, const char *testValue)
{
    float v3; // [esp+4h] [ebp-10h]
    long double v4; // [esp+8h] [ebp-Ch]
    float v5; // [esp+10h] [ebp-4h]

    v4 = atof(testValue);
    v5 = atof(dvarValue) - v4;
    v3 = I_fabs(v5);
    return v3 < 0.000009999999747378752;
}

void __cdecl Script_ExecNowOnDvarStringValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(
        dc->localClientNum,
        item,
        args,
        Script_ExecIfStringsEqual,
        (void(__cdecl *)(int, int, const char *))Cbuf_ExecuteBuffer);
}

void __cdecl Script_ExecNowOnDvarIntValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(
        dc->localClientNum,
        item,
        args,
        Script_ExecIfIntsEqual,
        (void(__cdecl *)(int, int, const char *))Cbuf_ExecuteBuffer);
}

void __cdecl Script_ExecNowOnDvarFloatValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalExecHandler(
        dc->localClientNum,
        item,
        args,
        Script_ExecIfFloatsEqual,
        (void(__cdecl *)(int, int, const char *))Cbuf_ExecuteBuffer);
}

void __cdecl Script_RespondOnDvarStringValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalResponseHandler(dc->localClientNum, item, args, Script_ExecIfStringsEqual);
}

void __cdecl Script_ConditionalResponseHandler(
    int localClientNum,
    itemDef_s *item,
    const char **args,
    bool(__cdecl *shouldRespond)(const char *, const char *))
{
    const char *v5; // eax
    int iIndex; // [esp+0h] [ebp-C10h]
    const char *pszName; // [esp+4h] [ebp-C0Ch]
    char testValue[1024]; // [esp+8h] [ebp-C08h] BYREF
    char dvarName[1024]; // [esp+408h] [ebp-808h] BYREF
    char command[1024]; // [esp+808h] [ebp-408h] BYREF
    const char *dvarValue; // [esp+C0Ch] [ebp-4h]

    if (String_Parse(args, dvarName, 1024))
    {
        if (String_Parse(args, testValue, 1024))
        {
            if (String_Parse(args, command, 1024))
            {
                dvarValue = Dvar_GetVariantString(dvarName);
                if (shouldRespond(dvarValue, testValue))
                {
                    for (iIndex = 0; iIndex < 32; ++iIndex)
                    {
                        pszName = CL_GetConfigString(localClientNum, iIndex + CS_SCRIPT_MENUS);
                        if (*pszName)
                        {
                            if (!I_stricmp(item->parent->window.name, pszName))
                                break;
                        }
                    }
                    if (iIndex == 32)
                        iIndex = -1;
#ifdef KISAK_MP
                    v5 = va("cmd mr %i %i %s\n", Dvar_GetInt("sv_serverId"), iIndex, command);
#else
                    v5 = va("cmd mr %i %s\n", iIndex, command);
#endif
                    Cbuf_AddText(localClientNum, v5);
                }
            }
        }
    }
}

void __cdecl Script_RespondOnDvarIntValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalResponseHandler(dc->localClientNum, item, args, Script_ExecIfIntsEqual);
}

void __cdecl Script_RespondOnDvarFloatValue(UiContext *dc, itemDef_s *item, const char **args)
{
    Script_ConditionalResponseHandler(dc->localClientNum, item, args, Script_ExecIfFloatsEqual);
}

void __cdecl Script_SetLocalVarFloat(UiContext *dc, itemDef_s *item, const char **args)
{
    float f; // [esp+4h] [ebp-110h]
    UILocalVarContext *var; // [esp+8h] [ebp-10Ch]
    char string[260]; // [esp+Ch] [ebp-108h] BYREF

    var = Script_ParseLocalVar(dc, args);
    if (String_Parse(args, string, 256))
    {
        if (var)
        {
            f = atof(string);
            UILocalVar_SetFloat(var->table, f);
        }
    }
}

void __cdecl Script_FeederTop(UiContext *dc, itemDef_s *item, const char **args)
{
    listBoxDef_s *listPtr; // [esp+8h] [ebp-4h]

    listPtr = Item_GetListBoxDef(item);
    if (listPtr)
    {
        if (listPtr->notselectable)
        {
            listPtr->startPos[dc->localClientNum] = 0;
        }
        else
        {
            item->cursorPos[dc->localClientNum] = 0;
            listPtr->startPos[dc->localClientNum] = 0;
            UI_FeederSelection(dc->localClientNum, item->special, 0);
        }
    }
}

void __cdecl Script_FeederBottom(UiContext *dc, itemDef_s *item, const char **args)
{
    int v2; // [esp+4h] [ebp-14h]
    int v3; // [esp+8h] [ebp-10h]
    int max; // [esp+Ch] [ebp-Ch]
    listBoxDef_s *listPtr; // [esp+10h] [ebp-8h]

    listPtr = Item_GetListBoxDef(item);
    if (listPtr)
    {
        if (listPtr->notselectable)
        {
            listPtr->startPos[dc->localClientNum] = Item_ListBox_MaxScroll(dc->localClientNum, item);
        }
        else
        {
            max = Item_ListBox_Viewmax(item);
            v3 = UI_FeederCount(dc->localClientNum, item->special) - 1;
            if (v3 > 0)
                v2 = v3;
            else
                v2 = 0;
            Item_ListBox_SetCursorPos(dc->localClientNum, item, max, v2);
        }
    }
}

void __cdecl Script_ScriptMenuResponse(UiContext *dc, itemDef_s *item, const char **args)
{
    const char *v4; // eax
    int iIndex; // [esp+0h] [ebp-410h]
    const char *pszName; // [esp+4h] [ebp-40Ch]
    char val[1028]; // [esp+8h] [ebp-408h] BYREF

    if (UI_AllowScriptMenuResponse(dc->localClientNum) && String_Parse(args, val, 1024))
    {
        for (iIndex = 0; iIndex < 32; ++iIndex)
        {
            pszName = CL_GetConfigString(dc->localClientNum, iIndex + CS_SCRIPT_MENUS);
            if (*pszName)
            {
                if (!I_stricmp(item->parent->window.name, pszName))
                    break;
            }
        }
        if (iIndex == 32)
            iIndex = -1;
#ifdef KISAK_MP
        v4 = va("cmd mr %i %i %s\n", Dvar_GetInt("sv_serverId"), iIndex, val);
#else
        v4 = va("cmd mr %i %s\n", iIndex, val);
#endif
        Cbuf_AddText(dc->localClientNum, v4);
    }
}

int __cdecl Item_ListBox_MaxScroll(int localClientNum, itemDef_s *item)
{
    int v2; // esi
    int v5; // [esp+Ch] [ebp-4h]

    v2 = Item_ListBox_Viewmax(item);
    v5 = UI_FeederCount(localClientNum, item->special) - v2 + 1;
    if (v5 > 0)
        return v5;
    else
        return 0;
}

int __cdecl Item_ListBox_Viewmax(itemDef_s *item)
{
    float v2; // [esp+8h] [ebp-2Ch]
    float v3; // [esp+Ch] [ebp-28h]
    float v4; // [esp+10h] [ebp-24h]
    float v5; // [esp+14h] [ebp-20h]
    float v6; // [esp+18h] [ebp-1Ch]
    float v7; // [esp+1Ch] [ebp-18h]
    listBoxDef_s *listPtr; // [esp+24h] [ebp-10h]
    float unitSize; // [esp+28h] [ebp-Ch]
    float totalSize; // [esp+30h] [ebp-4h]

    if (!item)
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2307, 0, "%s", "item");
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    }
    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2311, 0, "%s", "listPtr");
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
    if ((item->window.staticFlags & 0x200000) != 0)
    {
        v7 = item->window.rect.w - 2.0;
        v5 = 0.0 - v7;
        if (v5 < 0.0)
            v4 = item->window.rect.w - 2.0;
        else
            v4 = 0.0;
        totalSize = v4;
        unitSize = listPtr->elementWidth;
    }
    else
    {
        v6 = item->window.rect.h - 2.0;
        v3 = 0.0 - v6;
        if (v3 < 0.0)
            v2 = item->window.rect.h - 2.0;
        else
            v2 = 0.0;
        totalSize = v2;
        unitSize = listPtr->elementHeight;
    }
    if (totalSize < 0.0)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2324, 0, "%s\n\t(totalSize) = %g", "(totalSize >= 0)", totalSize);
    if (unitSize < 0.0)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2325, 0, "%s\n\t(unitSize) = %g", "(unitSize >= 0)", unitSize);
    return (int)(totalSize / unitSize);
}

void __cdecl Item_ListBox_SetCursorPos(int localClientNum, itemDef_s *item, int viewmax, int newCursorPos)
{
    listBoxDef_s *listPtr; // [esp+8h] [ebp-4h]

    listPtr = Item_GetListBoxDef(item);
    item->cursorPos[localClientNum] = newCursorPos;
    if (listPtr->startPos[localClientNum] > newCursorPos)
        listPtr->startPos[localClientNum] = newCursorPos;
    if (listPtr->startPos[localClientNum] <= newCursorPos - viewmax)
        listPtr->startPos[localClientNum] = newCursorPos - viewmax + 1;
    UI_FeederSelection(localClientNum, item->special, newCursorPos);
}

int __cdecl Menu_OverActiveItem(int localClientNum, menuDef_t *menu, float x, float y)
{
    const rectDef_s *v4; // eax
    itemDef_s *v6; // [esp+8h] [ebp-1Ch]
    itemDef_s *v7; // [esp+10h] [ebp-14h]
    itemDef_s *overItem; // [esp+18h] [ebp-Ch]
    int i; // [esp+1Ch] [ebp-8h]

    if (menu)
    {
        if (localClientNum)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                23,
                0,
                "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                localClientNum,
                1);
        if ((menu->window.dynamicFlags[localClientNum] & 0x4004) != 0
            && Rect_ContainsPoint(localClientNum, &menu->window.rect, x, y))
        {
            for (i = 0; i < menu->itemCount; ++i)
            {
                v7 = menu->items[i];
                if (localClientNum)
                    MyAssertHandler(
                        "c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h",
                        23,
                        0,
                        "localClientNum doesn't index MAX_POSSIBLE_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                        localClientNum,
                        1);
                if ((v7->window.dynamicFlags[localClientNum] & 0x4004) != 0
                    && (menu->items[i]->window.staticFlags & 0x100000) == 0)
                {
                    v6 = menu->items[i];
                    if (!v6)
                        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
                    if (Rect_ContainsPoint(localClientNum, &v6->window.rect, x, y))
                    {
                        overItem = menu->items[i];
                        if (overItem->type || !overItem->text)
                            return 1;
                        v4 = Item_CorrectedTextRect(localClientNum, overItem);
                        if (Rect_ContainsPoint(localClientNum, v4, x, y))
                            return 1;
                    }
                }
            }
        }
    }
    else
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 6219, 0, "%s", "menu");
    }
    return 0;
}

int __cdecl Display_VisibleMenuCount(UiContext *dc)
{
    menuDef_t *v2; // [esp+4h] [ebp-10h]
    int localClientNum; // [esp+8h] [ebp-Ch]
    int i; // [esp+Ch] [ebp-8h]
    int count; // [esp+10h] [ebp-4h]

    count = 0;
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
        if ((v2->window.dynamicFlags[localClientNum] & 0x4004) != 0)
            ++count;
    }
    return count;
}

void __cdecl Menus_HandleOOBClick(UiContext *dc, menuDef_t *menu, int key, int down)
{
    int j; // [esp+Ch] [ebp-8h]
    int i; // [esp+10h] [ebp-4h]

    if (menu)
    {
        if (down && (menu->window.staticFlags & 0x2000000) != 0)
        {
            Menu_RunCloseScript(dc, menu);
            Window_RemoveDynamicFlags(dc->localClientNum, &menu->window, 6);
        }
        for (i = dc->openMenuCount - 1; i >= 0; --i)
        {
            if (Menu_OverActiveItem(dc->localClientNum, dc->menuStack[i], dc->cursor.x, dc->cursor.y))
            {
                for (j = dc->openMenuCount - 1; j >= 0; --j)
                    Window_RemoveDynamicFlags(dc->localClientNum, &dc->menuStack[j]->window, 2);
                Window_AddDynamicFlags(dc->localClientNum, &dc->menuStack[i]->window, 6);
                Display_MouseMove(dc);
                Menu_HandleMouseMove(dc, dc->menuStack[i]);
                Menu_HandleKey(dc, dc->menuStack[i], key, down);
                break;
            }
        }
        if (!Display_VisibleMenuCount(dc))
            UI_Pause(dc->localClientNum, 0);
    }
    else
    {
        MyAssertHandler(".\\ui\\ui_shared.cpp", 4093, 0, "%s", "menu");
    }
}

void __cdecl Item_TextField_BeginEdit(int localClientNum, itemDef_s *item)
{
    uint32_t v2; // [esp+0h] [ebp-20h]
    editFieldDef_s *editPtr; // [esp+18h] [ebp-8h]
    int i; // [esp+1Ch] [ebp-4h]

    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3290, 0, "%s", "item");
    editPtr = Item_GetEditFieldDef(item);
    if (editPtr)
        editPtr->paintOffset = 0;
    if (item->dvar)
        v2 = strlen(Dvar_GetVariantString(item->dvar));
    else
        v2 = 0;
    item->cursorPos[localClientNum] = v2;
    g_editingField = 1;
    g_editItem = item;
    Key_SetOverstrikeMode(localClientNum, 1);
    for (i = 0; i < item->parent->itemCount; ++i)
    {
        if (item->parent->items[i] == item)
        {
            Menu_SetCursorItem(localClientNum, item->parent, i);
            return;
        }
    }
}

int __cdecl Menus_OpenByName(UiContext *dc, const char *p)
{
    menuDef_t *pMenu; // [esp+0h] [ebp-4h]

    pMenu = Menus_FindByName(dc, p);
    if (pMenu)
    {
        Menus_Open(dc, pMenu);
        return 1;
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_UI, "Could not find menu '%s'\n", p);
        return 0;
    }
}

void __cdecl Menus_PrintAllLoadedMenus(UiContext *dc)
{
    int i; // [esp+0h] [ebp-4h]

    Com_Printf(CON_CHANNEL_SYSTEM, "Currently loaded UI menus (CG menus not included):\n");
    for (i = 0; i < dc->menuCount; ++i)
        Com_Printf(CON_CHANNEL_SYSTEM, "%i. %s\n", i, dc->Menus[i]->window.name);
    Com_Printf(CON_CHANNEL_SYSTEM, "\n%i menus total\n", dc->menuCount);
}

int __cdecl Display_MouseMove(UiContext *dc)
{
    menuDef_t *menu; // [esp+4h] [ebp-8h]
    int i; // [esp+8h] [ebp-4h]

    menu = Menu_GetFocused(dc);
    if (menu && (menu->window.staticFlags & 0x1000000) != 0)
    {
        Menu_HandleMouseMove(dc, menu);
        return 1;
    }
    else
    {
        for (i = dc->openMenuCount - 1;
            i >= 0 && !Menu_HandleMouseMove(dc, dc->menuStack[i]) && !dc->menuStack[i]->fullScreen;
            --i)
        {
            ;
        }
        return 1;
    }
}

bool __cdecl Item_TextField_HandleKey(UiContext *dc, itemDef_s *item, int key)
{
    char *VariantString; // eax
    char *v5; // eax
    char *v6; // eax
    int OverstrikeMode; // eax
    bool v8; // [esp+10h] [ebp-430h]
    editFieldDef_s *editPtr; // [esp+24h] [ebp-41Ch]
    int len; // [esp+28h] [ebp-418h]
    itemDef_s *newItem; // [esp+2Ch] [ebp-414h]
    itemDef_s *newItema; // [esp+2Ch] [ebp-414h]
    itemDef_s *newItemb; // [esp+2Ch] [ebp-414h]
    itemDef_s *newItemc; // [esp+2Ch] [ebp-414h]
    int cursorPos; // [esp+30h] [ebp-410h]
    int cursorPosa; // [esp+30h] [ebp-410h]
    int cursorPosb; // [esp+30h] [ebp-410h]
    bool validInput; // [esp+37h] [ebp-409h] BYREF
    char buff[1024]; // [esp+38h] [ebp-408h] BYREF
    int memMoveCount; // [esp+43Ch] [ebp-4h]

    editPtr = Item_GetEditFieldDef(item);
    if (!editPtr)
        return 0;
    if (!item->dvar)
        return 0;
    memset((uint8_t *)buff, 0, sizeof(buff));
    VariantString = (char *)Dvar_GetVariantString(item->dvar);
    I_strncpyz(buff, VariantString, 1024);
    len = &buff[strlen(buff) + 1] - &buff[1];
    if (editPtr->maxChars)
    {
        if (len > editPtr->maxChars)
        {
            len = editPtr->maxChars;
            buff[len] = 0;
            if (item->cursorPos[dc->localClientNum] > editPtr->maxChars)
                item->cursorPos[dc->localClientNum] = editPtr->maxChars;
        }
    }
    if ((key & K_CHAR_FLAG) != 0)
    {
        key &= ~K_CHAR_FLAG;
        if (key == '\b')
        {
            if (item->cursorPos[dc->localClientNum] > 0)
            {
                cursorPos = item->cursorPos[dc->localClientNum];
                memMoveCount = len + 1 - cursorPos;
                if (memMoveCount <= 0 || memMoveCount > len)
                    MyAssertHandler(
                        ".\\ui\\ui_shared.cpp",
                        3375,
                        0,
                        "%s\n\t(memMoveCount) = %i",
                        "(memMoveCount > 0 && memMoveCount <= len)",
                        memMoveCount);
                memmove((uint8_t *)&buff[cursorPos - 1], (uint8_t *)&buff[cursorPos], memMoveCount);
            }
            Dvar_SetFromStringByName(item->dvar, buff);
            v5 = (char *)Dvar_GetVariantString(item->dvar);
            I_strncpyz(buff, v5, 1024);
            item->cursorPos[dc->localClientNum] = Item_GetCursorPosOffset(dc->localClientNum, item, buff, -1);
            Item_TextField_EnsureCursorVisible(dc->localClientNum, item, buff);
            return 1;
        }
        if (item->type == 16 && !I_isforfilename(key))
            return 1;
        if (key < K_SPACE || !item->dvar)
            return 1;
        if (key == '@')
            return 1;
        if (item->type == 9 && !I_isdigit(key))
            return 0;
        if (item->type == 17)
        {
            v8 = I_isdigit(key) || key == Com_GetDecimalDelimiter();
            validInput = v8;
            if (!v8)
                return 0;
            if (key == Com_GetDecimalDelimiter())
                key = 46;
        }
        if (item->type == 18)
            key = toupper(key);
        if (!Key_GetOverstrikeMode(dc->localClientNum))
        {
            if (len == 255 || editPtr->maxChars && len >= editPtr->maxChars)
                return 1;
            cursorPosa = item->cursorPos[dc->localClientNum];
            memMoveCount = len + 1 - cursorPosa;
            if (memMoveCount <= 0 || memMoveCount > len + 1)
                MyAssertHandler(
                    ".\\ui\\ui_shared.cpp",
                    3439,
                    0,
                    "%s\n\t(memMoveCount) = %i",
                    "(memMoveCount > 0 && memMoveCount <= (len + 1))",
                    memMoveCount);
            memmove((uint8_t *)&buff[cursorPosa + 1], (uint8_t *)&buff[cursorPosa], memMoveCount);
            goto LABEL_54;
        }
        if (!editPtr->maxChars || item->cursorPos[dc->localClientNum] < editPtr->maxChars)
        {
        LABEL_54:
            buff[item->cursorPos[dc->localClientNum]] = key;
            Dvar_SetFromStringByName(item->dvar, buff);
            v6 = (char *)Dvar_GetVariantString(item->dvar);
            I_strncpyz(buff, v6, 1024);
            strlen(buff);
            item->cursorPos[dc->localClientNum] = Item_GetCursorPosOffset(dc->localClientNum, item, buff, 1);
            Item_TextField_EnsureCursorVisible(dc->localClientNum, item, buff);
            if (editPtr->maxChars)
            {
                if (item->cursorPos[dc->localClientNum] >= editPtr->maxChars)
                {
                    if (editPtr->maxCharsGotoNext)
                    {
                        newItema = Menu_SetNextCursorItem(dc, item->parent);
                        newItema->cursorPos[dc->localClientNum] = 0;
                        if (newItema)
                        {
                            if (Item_IsTextField(newItema))
                                g_editItem = newItema;
                        }
                    }
                }
            }
        LABEL_84:
            if (key == K_TAB || key == K_DOWNARROW || key == K_KP_DOWNARROW)
            {
                newItemb = Menu_SetNextCursorItem(dc, item->parent);
                if (newItemb)
                {
                    if (Item_IsTextField(newItemb))
                        g_editItem = newItemb;
                }
            }
            if (key == K_UPARROW || key == K_KP_UPARROW)
            {
                newItemc = Menu_SetPrevCursorItem(dc, item->parent);
                if (newItemc)
                {
                    if (Item_IsTextField(newItemc))
                        g_editItem = newItemc;
                }
            }
            if ((key == K_ENTER || key == K_KP_ENTER) && item->onAccept)
                Item_RunScript(dc, item, (char*)item->onAccept);
            return key != K_ENTER && key != K_KP_ENTER && key != K_ESCAPE;
        }
        if (editPtr->maxCharsGotoNext)
        {
            newItem = Menu_SetNextCursorItem(dc, item->parent);
            if (newItem)
            {
                if (Item_IsTextField(newItem))
                    g_editItem = newItem;
            }
        }
        return 1;
    }
    else
    {
        if (key != K_DEL && key != K_KP_DEL)
        {
            switch (key)
            {
            case K_RIGHTARROW:
            case K_KP_RIGHTARROW:
                item->cursorPos[dc->localClientNum] = Item_GetCursorPosOffset(dc->localClientNum, item, buff, 1);
                Item_TextField_EnsureCursorVisible(dc->localClientNum, item, buff);
                return 1;
            case K_LEFTARROW:
            case K_KP_LEFTARROW:
                item->cursorPos[dc->localClientNum] = Item_GetCursorPosOffset(dc->localClientNum, item, buff, -1);
                Item_TextField_EnsureCursorVisible(dc->localClientNum, item, buff);
                return 1;
            case K_HOME:
            case K_KP_HOME:
                item->cursorPos[dc->localClientNum] = 0;
                editPtr->paintOffset = 0;
                return 1;
            case K_END:
            case K_KP_END:
                item->cursorPos[dc->localClientNum] = len;
                Item_TextField_EnsureCursorVisible(dc->localClientNum, item, buff);
                return 1;
            case K_INS:
            case K_KP_INS:
                OverstrikeMode = Key_GetOverstrikeMode(dc->localClientNum);
                Key_SetOverstrikeMode(dc->localClientNum, OverstrikeMode == 0);
                return 1;
            }
            goto LABEL_84;
        }
        if (item->cursorPos[dc->localClientNum] < len)
        {
            cursorPosb = item->cursorPos[dc->localClientNum];
            memMoveCount = len - cursorPosb;
            if (len - cursorPosb <= 0 || memMoveCount > len)
                MyAssertHandler(
                    ".\\ui\\ui_shared.cpp",
                    3490,
                    0,
                    "%s\n\t(memMoveCount) = %i",
                    "(memMoveCount > 0 && memMoveCount <= len)",
                    memMoveCount);
            memmove((uint8_t *)&buff[cursorPosb], (uint8_t *)&buff[cursorPosb + 1], memMoveCount);
            Dvar_SetFromStringByName(item->dvar, buff);
        }
        return 1;
    }
}

void __cdecl Item_TextField_EnsureCursorVisible(int localClientNum, itemDef_s *item, const char *text)
{
    editFieldDef_s *editPtr; // [esp+0h] [ebp-Ch]
    int paintOffsetMin; // [esp+4h] [ebp-8h]
    int cursorPos; // [esp+8h] [ebp-4h]

    editPtr = Item_GetEditFieldDef(item);
    cursorPos = item->cursorPos[localClientNum];
    if (editPtr->paintOffset <= cursorPos)
    {
        if (editPtr->maxPaintChars)
        {
            paintOffsetMin = Item_GetCursorPosOffset(localClientNum, item, text, -editPtr->maxPaintChars);
            if (editPtr->paintOffset < paintOffsetMin)
                editPtr->paintOffset = paintOffsetMin;
        }
    }
    else
    {
        editPtr->paintOffset = cursorPos;
    }
}

void __cdecl Scroll_ListBox_AutoFunc(UiContext *dc, void *p)
{
    if (dc->realTime > *(_DWORD *)p)
    {
        Item_ListBox_HandleKey(dc, *((itemDef_s **)p + 6), *((_DWORD *)p + 3), 1, 0);
        *(_DWORD *)p = *((_DWORD *)p + 2) + dc->realTime;
    }
    if (dc->realTime > *((_DWORD *)p + 1))
    {
        *((_DWORD *)p + 1) = dc->realTime + 150;
        if (*((int *)p + 2) > 20)
            *((_DWORD *)p + 2) -= 40;
    }
}

void __cdecl Scroll_ListBox_ThumbFunc(UiContext *dc, void *p)
{
    int v2; // [esp+0h] [ebp-3Ch]
    int v3; // [esp+4h] [ebp-38h]
    int v4; // [esp+8h] [ebp-34h]
    int pos; // [esp+10h] [ebp-2Ch]
    int posa; // [esp+10h] [ebp-2Ch]
    int max; // [esp+14h] [ebp-28h]
    int maxa; // [esp+14h] [ebp-28h]
    float r; // [esp+18h] [ebp-24h]
    float r_4; // [esp+1Ch] [ebp-20h]
    float r_8; // [esp+20h] [ebp-1Ch]
    float r_12; // [esp+24h] [ebp-18h]
    listBoxDef_s *listPtr; // [esp+34h] [ebp-8h]

    if (dc->isCursorVisible)
    {
        listPtr = Item_GetListBoxDef(*((itemDef_s **)p + 6));
        if (listPtr)
        {
            v4 = *((_DWORD *)p + 6);
            if (!v4)
                MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
            if ((*(_DWORD *)(v4 + 76) & 0x200000) != 0)
            {
                if (*((float *)p + 4) == dc->cursor.x)
                    return;
                v3 = *((_DWORD *)p + 6);
                if (!v3)
                    MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
                r = *(float *)(v3 + 4) + 16.0 + 1.0;
                r_8 = *(float *)(v3 + 12) - 32.0 - 2.0;
                max = Item_ListBox_MaxScroll(dc->localClientNum, *((itemDef_s **)p + 6));
                pos = (int)((dc->cursor.x - r - 8.0) * (double)max / (r_8 - 16.0));
                if (pos >= 0)
                {
                    if (pos > max)
                        pos = max;
                }
                else
                {
                    pos = 0;
                }
                listPtr->startPos[dc->localClientNum] = pos;
                *((float *)p + 4) = dc->cursor.x;
            }
            else if (*((float *)p + 5) != dc->cursor.y)
            {
                v2 = *((_DWORD *)p + 6);
                if (!v2)
                    MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
                r_4 = *(float *)(v2 + 8) + 16.0 + 1.0;
                r_12 = *(float *)(v2 + 16) - 32.0 - 2.0;
                maxa = Item_ListBox_MaxScroll(dc->localClientNum, *((itemDef_s **)p + 6));
                posa = (int)((dc->cursor.y - r_4 - 8.0) * (double)maxa / (r_12 - 16.0));
                if (posa >= 0)
                {
                    if (posa > maxa)
                        posa = maxa;
                }
                else
                {
                    posa = 0;
                }
                listPtr->startPos[dc->localClientNum] = posa;
                *((float *)p + 5) = dc->cursor.y;
            }
            if (dc->realTime > *(_DWORD *)p)
            {
                Item_ListBox_HandleKey(dc, *((itemDef_s **)p + 6), *((_DWORD *)p + 3), 1, 0);
                *(_DWORD *)p = *((_DWORD *)p + 2) + dc->realTime;
            }
            if (dc->realTime > *((_DWORD *)p + 1))
            {
                *((_DWORD *)p + 1) = dc->realTime + 150;
                if (*((int *)p + 2) > 20)
                    *((_DWORD *)p + 2) -= 40;
            }
        }
    }
}

int __cdecl Item_Slider_OverSlider(int localClientNum, itemDef_s *item, float x, float y)
{
    rectDef_s r; // [esp+8h] [ebp-1Ch] BYREF
    const rectDef_s *rect; // [esp+20h] [ebp-4h]

    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    r.x = Item_Slider_ThumbPosition(localClientNum, item) - 5.0;
    r.y = item->window.rect.y - 2.0;
    r.w = 10.0;
    r.h = 20.0;
    r.horzAlign = item->window.rect.horzAlign;
    r.vertAlign = item->window.rect.vertAlign;
    if (Rect_ContainsPoint(localClientNum, &r, x, y))
        return 1024;
    else
        return 0;
}

void __cdecl Scroll_Slider_SetThumbPos(UiContext *dc, itemDef_s *item);

void __cdecl Scroll_Slider_ThumbFunc(UiContext *dc, itemDef_s **p)
{
    Scroll_Slider_SetThumbPos(dc, p[6]);
}

int __cdecl Item_OwnerDraw_HandleKey(itemDef_s *item, int key)
{
    return UI_OwnerDrawHandleKey(item->window.ownerDraw, item->window.ownerDrawFlags, &item->special, key);
}

int lastListBoxClickTime;
int __cdecl Item_ListBox_HandleKey(UiContext *dc, itemDef_s *item, int key, int down, int force)
{
    int result; // eax
    int v6; // [esp+8h] [ebp-84h]
    int v7; // [esp+Ch] [ebp-80h]
    int v8; // [esp+10h] [ebp-7Ch]
    int v9; // [esp+14h] [ebp-78h]
    int v10; // [esp+1Ch] [ebp-70h]
    int v11; // [esp+24h] [ebp-68h]
    int v12; // [esp+2Ch] [ebp-60h]
    bool v13; // [esp+30h] [ebp-5Ch]
    int v15; // [esp+4Ch] [ebp-40h]
    int v16; // [esp+54h] [ebp-38h]
    int localClientNum; // [esp+5Ch] [ebp-30h]
    int v18; // [esp+60h] [ebp-2Ch]
    int max; // [esp+64h] [ebp-28h]
    listBoxDef_s *listPtr; // [esp+70h] [ebp-1Ch]
    int viewmax; // [esp+74h] [ebp-18h]
    int flags; // [esp+78h] [ebp-14h]
    ItemKeyHandler *handler; // [esp+84h] [ebp-8h]

    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    v13 = dc->isCursorVisible && Rect_ContainsPoint(dc->localClientNum, &item->window.rect, dc->cursor.x, dc->cursor.y);
    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        return 0;
    v18 = UI_FeederCount(dc->localClientNum, item->special) - 1;
    if (v18 > 0)
        v12 = v18;
    else
        v12 = 0;
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
    if (!force && (!v13 || (flags & 2) == 0))
        return 0;
    UI_OverrideCursorPos(dc->localClientNum, item);
    max = Item_ListBox_MaxScroll(dc->localClientNum, item);
    viewmax = Item_ListBox_Viewmax(item);
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
    if ((item->window.staticFlags & 0x200000) != 0)
    {
        if (key == K_LEFTARROW || key == K_KP_LEFTARROW)
        {
            if (listPtr->notselectable)
            {
                if (listPtr->startPos[dc->localClientNum] - 1 > 0)
                    v11 = listPtr->startPos[dc->localClientNum] - 1;
                else
                    v11 = 0;
                listPtr->startPos[dc->localClientNum] = v11;
            }
            else
            {
                v16 = item->cursorPos[dc->localClientNum] - 1;
                if (v16 > 0)
                    Item_ListBox_SetCursorPos(dc->localClientNum, item, viewmax, v16);
                else
                    Item_ListBox_SetCursorPos(dc->localClientNum, item, viewmax, 0);
            }
            return 1;
        }
        if (key == K_RIGHTARROW || key == K_KP_RIGHTARROW)
        {
            if (listPtr->notselectable)
            {
                if (listPtr->startPos[dc->localClientNum] + 1 < max)
                    v10 = listPtr->startPos[dc->localClientNum] + 1;
                else
                    v10 = max;
                listPtr->startPos[dc->localClientNum] = v10;
            }
            else
            {
                v15 = item->cursorPos[dc->localClientNum] + 1;
                if (v15 < v12)
                    Item_ListBox_SetCursorPos(dc->localClientNum, item, viewmax, v15);
                else
                    Item_ListBox_SetCursorPos(dc->localClientNum, item, viewmax, v12);
            }
            return 1;
        }
    LABEL_48:
        switch (key)
        {
        case K_MOUSE1:
        case K_MOUSE2:
            if ((flags & 0x100) != 0)
            {
                if (listPtr->startPos[dc->localClientNum] - 1 > 0)
                    v9 = listPtr->startPos[dc->localClientNum] - 1;
                else
                    v9 = 0;
                listPtr->startPos[dc->localClientNum] = v9;
            }
            else if ((flags & 0x200) != 0)
            {
                if (listPtr->startPos[dc->localClientNum] + 1 < max)
                    v8 = listPtr->startPos[dc->localClientNum] + 1;
                else
                    v8 = max;
                listPtr->startPos[dc->localClientNum] = v8;
            }
            else if ((flags & 0x800) != 0)
            {
                if (listPtr->startPos[dc->localClientNum] - viewmax > 0)
                    v7 = listPtr->startPos[dc->localClientNum] - viewmax;
                else
                    v7 = 0;
                listPtr->startPos[dc->localClientNum] = v7;
            }
            else if ((flags & 0x1000) != 0)
            {
                if (viewmax + listPtr->startPos[dc->localClientNum] < max)
                    v6 = viewmax + listPtr->startPos[dc->localClientNum];
                else
                    v6 = max;
                listPtr->startPos[dc->localClientNum] = v6;
            }
            else if ((flags & 0x400) == 0)
            {
                if (item->special == 25.0)
                {
                    UI_OverrideCursorPos(dc->localClientNum, item);
                    UI_FeederSelection(dc->localClientNum, item->special, item->cursorPos[dc->localClientNum]);
                }
                if (CL_GetLocalClientActiveCount())
                {
                    if (dc->realTime < lastListBoxClickTime
                        && listPtr->doubleClick
                        && item->cursorPos[dc->localClientNum] == listPtr->mousePos
                        && ListBox_HasValidCursorPos(dc->localClientNum, item))
                    {
                        Item_RunScript(dc, item, (char*)listPtr->doubleClick);
                    }
                    lastListBoxClickTime = dc->realTime + 300;
                    if (item->cursorPos[dc->localClientNum] != listPtr->mousePos)
                    {
                        if (item->cursorPos[dc->localClientNum] < 0
                            || listPtr->mousePos < UI_FeederCount(dc->localClientNum, item->special))
                        {
                            item->cursorPos[dc->localClientNum] = listPtr->mousePos;
                        }
                        UI_FeederSelection(dc->localClientNum, item->special, item->cursorPos[dc->localClientNum]);
                    }
                }
            }
            return 1;
        case K_HOME:
        case K_KP_HOME:
            Script_FeederTop(dc, item);
            return 1;
        case K_END:
        case K_KP_END:
            Script_FeederBottom(dc, item);
            return 1;
        case K_PGUP:
        case K_KP_PGUP:
            Item_ListBox_Page(dc->localClientNum, item, v12, max, viewmax, -viewmax);
            return 1;
        case K_PGDN:
        case K_KP_PGDN:
            Item_ListBox_Page(dc->localClientNum, item, v12, max, viewmax, viewmax);
            return 1;
        }
        for (handler = item->onKey; handler; handler = handler->next)
        {
            if (handler->key == key)
            {
                Item_RunScript(dc, item, (char*)handler->action);
                return 1;
            }
        }
        return 0;
    }
    switch (key)
    {
    case K_UPARROW:
    case K_KP_UPARROW:
    case K_MWHEELUP:
        Item_ListBox_Scroll(dc->localClientNum, item, v12, max, viewmax, -1);
        result = 1;
        break;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
    case K_MWHEELDOWN:
        Item_ListBox_Scroll(dc->localClientNum, item, v12, max, viewmax, 1);
        result = 1;
        break;
    default:
        goto LABEL_48;
    }
    return result;
}

void __cdecl Item_ListBox_Page(int localClientNum, itemDef_s *item, int max, int scrollmax, int viewmax, int delta)
{
    int v6; // [esp+4h] [ebp-18h]
    int v7; // [esp+Ch] [ebp-10h]
    int v8; // [esp+14h] [ebp-8h]
    listBoxDef_s *listPtr; // [esp+18h] [ebp-4h]

    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2735, 0, "%s", "listPtr");
    if (delta + listPtr->startPos[localClientNum] < scrollmax)
        v8 = delta + listPtr->startPos[localClientNum];
    else
        v8 = scrollmax;
    if (v8 > 0)
        v6 = v8;
    else
        v6 = 0;
    listPtr->startPos[localClientNum] = v6;
    if (!listPtr->notselectable)
    {
        if (delta + item->cursorPos[localClientNum] < max)
            v7 = delta + item->cursorPos[localClientNum];
        else
            v7 = max;
        if (v7 > 0)
            Item_ListBox_SetCursorPos(localClientNum, item, viewmax, v7);
        else
            Item_ListBox_SetCursorPos(localClientNum, item, viewmax, 0);
    }
}

void __cdecl Item_ListBox_Scroll(int localClientNum, itemDef_s *item, int max, int scrollmax, int viewmax, int delta)
{
    int v6; // [esp+0h] [ebp-1Ch]
    int v7; // [esp+Ch] [ebp-10h]
    int v8; // [esp+14h] [ebp-8h]
    listBoxDef_s *listPtr; // [esp+18h] [ebp-4h]

    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 2748, 0, "%s", "listPtr");
    if (listPtr->notselectable)
    {
        if (delta + listPtr->startPos[localClientNum] < scrollmax)
            v7 = delta + listPtr->startPos[localClientNum];
        else
            v7 = scrollmax;
        if (v7 > 0)
            v6 = v7;
        else
            v6 = 0;
        listPtr->startPos[localClientNum] = v6;
    }
    else
    {
        if (delta + item->cursorPos[localClientNum] < max)
            v8 = delta + item->cursorPos[localClientNum];
        else
            v8 = max;
        if (v8 > 0)
            Item_ListBox_SetCursorPos(localClientNum, item, viewmax, v8);
        else
            Item_ListBox_SetCursorPos(localClientNum, item, viewmax, 0);
    }
}

int __cdecl Item_Multi_HandleKey(UiContext *dc, itemDef_s *item, int key)
{
    char *v4; // eax
    multiDef_s *multiPtr; // [esp+8h] [ebp-10h]
    int next; // [esp+Ch] [ebp-Ch]
    int count; // [esp+10h] [ebp-8h]
    int current; // [esp+14h] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3219, 0, "%s", "dc");
    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3220, 0, "%s", "item");
    if (!item->dvar)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3221, 0, "%s", "item->dvar");
    multiPtr = Item_GetMultiDef(item);
    if (!multiPtr)
        return 0;
    if (!Item_ShouldHandleKey(dc, item, key))
        return 0;
    current = Item_Multi_FindDvarByValue(item);
    count = Item_Multi_CountSettings(item);
    next = Item_List_NextEntryForKey(key, current, count);
    if (next == current)
        return 0;
    if (multiPtr->strDef)
    {
        Dvar_SetFromStringByName(item->dvar, (char*)multiPtr->dvarStr[next]);
    }
    else
    {
        v4 = va("%g", multiPtr->dvarValue[next]);
        Dvar_SetFromStringByName(item->dvar, v4);
    }
    return 1;
}

int __cdecl Item_Multi_CountSettings(itemDef_s *item)
{
    multiDef_s *multiPtr; // [esp+0h] [ebp-4h]

    multiPtr = Item_GetMultiDef(item);
    if (multiPtr)
        return multiPtr->count;
    else
        return 0;
}

int __cdecl Item_Multi_FindDvarByValue(itemDef_s *item)
{
    const char *VariantString; // eax
    multiDef_s *multiPtr; // [esp+0h] [ebp-10h]
    const char *string; // [esp+4h] [ebp-Ch]
    int i; // [esp+8h] [ebp-8h]
    int ia; // [esp+8h] [ebp-8h]
    float value; // [esp+Ch] [ebp-4h]

    multiPtr = Item_GetMultiDef(item);
    if (!multiPtr)
        return 0;
    if (multiPtr->strDef)
    {
        string = Dvar_GetVariantString(item->dvar);
        for (i = 0; i < multiPtr->count; ++i)
        {
            if (!I_stricmp(string, multiPtr->dvarStr[i]))
                return i;
        }
    }
    else
    {
        VariantString = Dvar_GetVariantString(item->dvar);
        value = atof(VariantString);
        for (ia = 0; ia < multiPtr->count; ++ia)
        {
            if (value == multiPtr->dvarValue[ia])
                return ia;
        }
    }
    return 0;
}

int __cdecl Item_List_NextEntryForKey(int key, int current, int count)
{
    bool v4; // [esp+0h] [ebp-Ch]
    bool v5; // [esp+4h] [ebp-8h]
    char v6; // [esp+Ah] [ebp-2h]
    char v7; // [esp+Bh] [ebp-1h]

    if (count < 0)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3199, 0, "%s\n\t(count) = %i", "(count >= 0)", count);
    if (current < 0 || current >= count && count)
        MyAssertHandler(
            ".\\ui\\ui_shared.cpp",
            3200,
            0,
            "%s\n\t(current) = %i",
            "(current >= 0 && (current < count || count == 0))",
            current);
    if (!count)
        return 0;
    if (key == K_MOUSE1 || key == K_MOUSE3)
    {
        v7 = 1;
    }
    else
    {
        v5 = key == K_ENTER || key == K_PGDN || key == K_RIGHTARROW;
        v7 = v5;
    }
    if (v7)
        return (current + 1) % count;
    if (key == K_MOUSE2)
    {
        v6 = 1;
    }
    else
    {
        v4 = key == K_PGUP || key == K_LEFTARROW;
        v6 = v4;
    }
    if (v6)
        return (current + count - 1) % count;
    else
        return current;
}

int __cdecl Item_DvarEnum_HandleKey(UiContext *dc, itemDef_s *item, int key)
{
    char *v4; // eax
    int next; // [esp+0h] [ebp-Ch]
    int count; // [esp+4h] [ebp-8h]
    int current; // [esp+8h] [ebp-4h]

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3250, 0, "%s", "dc");
    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3251, 0, "%s", "item");
    if (!item->dvar)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3252, 0, "%s", "item->dvar");
    if (!Item_ShouldHandleKey(dc, item, key))
        return 0;
    current = Item_DvarEnum_EnumIndex(item);
    count = Item_DvarEnum_CountSettings(item);
    next = Item_List_NextEntryForKey(key, current, count);
    if (next == current)
        return 0;
    v4 = va("%i", next);
    Dvar_SetFromStringByName(item->dvar, v4);
    return 1;
}

int __cdecl Item_DvarEnum_CountSettings(itemDef_s *item)
{
    const dvar_s *enumDvar; // [esp+0h] [ebp-4h]

    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3052, 0, "%s", "item");
    if (item->type != 13)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3053, 0, "%s\n\t(item->type) = %i", "(item->type == 13)", item->type);
    if (!item->typeData.listBox)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3054, 0, "%s", "item->typeData.enumDvarName");
    enumDvar = Dvar_FindVar(item->typeData.enumDvarName);
    if (enumDvar->type == 6)
        return enumDvar->domain.enumeration.stringCount;
    else
        return 0;
}

int __cdecl Item_DvarEnum_EnumIndex(itemDef_s *item)
{
    int enumIndex; // [esp+0h] [ebp-Ch]
    int enumIndexa; // [esp+0h] [ebp-Ch]
    const dvar_s *enumDvar; // [esp+4h] [ebp-8h]
    const char *enumString; // [esp+8h] [ebp-4h]

    enumDvar = Dvar_FindVar(item->typeData.enumDvarName);
    if (enumDvar->type != 6)
        return 0;
    enumString = Dvar_GetVariantString(item->dvar);
    enumIndex = atoi(enumString);
    if (enumIndex >= 0 && enumIndex < enumDvar->domain.enumeration.stringCount)
        return enumIndex;
    for (enumIndexa = 0; enumIndexa < enumDvar->domain.enumeration.stringCount; ++enumIndexa)
    {
        if (!I_stricmp(enumString, *(const char **)(enumDvar->domain.integer.max + 4 * enumIndexa)))
            return enumIndexa;
    }
    return 0;
}

double __cdecl Item_Slider_ThumbPosition(int localClientNum, itemDef_s *item)
{
    const char *VariantString; // eax
    float x0; // [esp+Ch] [ebp-18h]
    float range; // [esp+10h] [ebp-14h]
    editFieldDef_s *editDef; // [esp+14h] [ebp-10h]
    float x; // [esp+18h] [ebp-Ch]
    float value; // [esp+20h] [ebp-4h]
    float valuea; // [esp+20h] [ebp-4h]
    float valueb; // [esp+20h] [ebp-4h]
    float valuec; // [esp+20h] [ebp-4h]

    editDef = Item_GetEditFieldDef(item);
    if (!editDef)
        return 0.0;
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    x0 = item->window.rect.x + item->textalignx;
    x = Item_GetRectPlacementX(item->textAlignMode & 3, x0, item->window.rect.w, 96.0);
    if (!editDef && item->dvar)
        return x;
    VariantString = Dvar_GetVariantString(item->dvar);
    value = atof(VariantString);
    if (editDef->minVal <= (double)value)
    {
        if (editDef->maxVal < (double)value)
            value = editDef->maxVal;
    }
    else
    {
        value = editDef->minVal;
    }
    range = editDef->maxVal - editDef->minVal;
    valuea = value - editDef->minVal;
    valueb = valuea / range;
    valuec = valueb * 84.0;
    return (float)(valuec + 5.0 + 1.0 + x);
}

double __cdecl Item_GetRectPlacementX(int alignX, float x0, float containerWidth, float selfWidth)
{
    if (!alignX)
        return x0;
    if (alignX == 1)
    {
        return (float)((containerWidth - selfWidth) * 0.5 + x0);
    }
    else
    {
        if (alignX != 2)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 180, 0, "alignX == ITEM_ALIGN_RIGHT\n\t%i, %i", alignX, 2);
        return (float)(containerWidth - selfWidth + x0);
    }
}

double __cdecl Item_ListBox_ThumbPosition(int localClientNum, itemDef_s *item)
{
    int max; // [esp+Ch] [ebp-14h]
    float pos; // [esp+10h] [ebp-10h]
    float posb; // [esp+10h] [ebp-10h]
    float posa; // [esp+10h] [ebp-10h]
    float posc; // [esp+10h] [ebp-10h]
    listBoxDef_s *listPtr; // [esp+14h] [ebp-Ch]
    float size; // [esp+18h] [ebp-8h]
    float sizea; // [esp+18h] [ebp-8h]

    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        return 0.0;
    max = Item_ListBox_MaxScroll(localClientNum, item);
    if (!item)
    {
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
    }
    if ((item->window.staticFlags & 0x200000) != 0)
    {
        if (max <= 0)
        {
            pos = 0.0;
        }
        else
        {
            size = item->window.rect.w - 32.0 - 2.0;
            pos = (size - 16.0) / (double)max;
        }
        posb = (double)listPtr->startPos[localClientNum] * pos;
        return (float)(item->window.rect.x + 1.0 + 16.0 + posb);
    }
    else
    {
        if (max <= 0)
        {
            posa = 0.0;
        }
        else
        {
            sizea = item->window.rect.h - 32.0 - 2.0;
            posa = (sizea - 16.0) / (double)max;
        }
        posc = (double)listPtr->startPos[localClientNum] * posa;
        return (float)(item->window.rect.y + 1.0 + 16.0 + posc);
    }
}

void __cdecl Scroll_Slider_SetThumbPos(UiContext *dc, itemDef_s *item)
{
    char *v2; // eax
    float v3; // [esp+Ch] [ebp-40h]
    float v4; // [esp+10h] [ebp-3Ch]
    float v5; // [esp+14h] [ebp-38h]
    float x0; // [esp+18h] [ebp-34h]
    float v7; // [esp+1Ch] [ebp-30h]
    float v8; // [esp+20h] [ebp-2Ch]
    float hIgnored; // [esp+24h] [ebp-28h] BYREF
    float yIgnored; // [esp+28h] [ebp-24h] BYREF
    const ScreenPlacement *scrPlace; // [esp+2Ch] [ebp-20h]
    float usableStart; // [esp+30h] [ebp-1Ch] BYREF
    editFieldDef_s *editDef; // [esp+34h] [ebp-18h]
    float cursorx; // [esp+38h] [ebp-14h]
    float usableWidth; // [esp+3Ch] [ebp-10h] BYREF
    float x; // [esp+40h] [ebp-Ch]
    const rectDef_s *rect; // [esp+44h] [ebp-8h]
    float value; // [esp+48h] [ebp-4h]

    editDef = Item_GetEditFieldDef(item);
    if (editDef)
    {
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        rect = &item->window.rect;
        x0 = item->window.rect.x + item->textalignx;
        x = Item_GetRectPlacementX(item->textAlignMode & 3, x0, item->window.rect.w, 96.0);
        scrPlace = &scrPlaceView[dc->localClientNum];
        cursorx = ScrPlace_ApplyX(scrPlace, dc->cursor.x, 4);
        usableStart = x + 5.0 + 1.0;
        usableWidth = 84.0;
        yIgnored = 0.0;
        hIgnored = 0.0;
        ScrPlace_ApplyRect(
            scrPlace,
            &usableStart,
            &yIgnored,
            &usableWidth,
            &hIgnored,
            item->window.rect.horzAlign,
            item->window.rect.vertAlign);
        v7 = cursorx - usableStart;
        v5 = v7 - usableWidth;
        if (v5 < 0.0)
            v8 = cursorx - usableStart;
        else
            v8 = usableWidth;
        v4 = 0.0 - v7;
        if (v4 < 0.0)
            v3 = v8;
        else
            v3 = 0.0;
        value = v3 / usableWidth;
        value = (editDef->maxVal - editDef->minVal) * value;
        value = value + editDef->minVal;
        v2 = va("%g", value);
        Dvar_SetFromStringByName(item->dvar, v2);
    }
}

int __cdecl Item_Slider_HandleKey(UiContext *dc, itemDef_s *item, int key, int down)
{
    const char *VariantString; // eax
    char *v6; // eax
    char *v7; // [esp+4h] [ebp-38h]
    float v8; // [esp+8h] [ebp-34h]
    float v9; // [esp+Ch] [ebp-30h]
    float v11; // [esp+14h] [ebp-28h]
    float v12; // [esp+18h] [ebp-24h]
    float v14; // [esp+20h] [ebp-1Ch]
    float maxVal; // [esp+24h] [ebp-18h]
    float v16; // [esp+28h] [ebp-14h]
    float minVal; // [esp+2Ch] [ebp-10h]
    editFieldDef_s *editDef; // [esp+30h] [ebp-Ch]
    float step; // [esp+34h] [ebp-8h]
    float value; // [esp+38h] [ebp-4h]

    if (!item->dvar)
        return 0;
    if (!Item_ShouldHandleKey(dc, item, key))
        return 0;
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_MOUSE3)
    {
        Scroll_Slider_SetThumbPos(dc, item);
        return 1;
    }
    else
    {
        editDef = Item_GetEditFieldDef(item);
        if (editDef)
        {
            step = (editDef->maxVal - editDef->minVal) * 0.05000000074505806;
            VariantString = Dvar_GetVariantString(item->dvar);
            value = atof(VariantString);
            if (key == K_LEFTARROW || key == K_PGUP)
            {
                v16 = value - step;
                minVal = editDef->minVal;
                v12 = v16 - minVal;
                if (v12 < 0.0)
                    v11 = minVal;
                else
                    v11 = value - step;
                v6 = va("%g", v11);
                Dvar_SetFromStringByName(item->dvar, v6);
                return 1;
            }
            else if (key == K_RIGHTARROW || key == K_PGDN)
            {
                v14 = step + value;
                maxVal = editDef->maxVal;
                v9 = maxVal - v14;
                if (v9 < 0.0)
                    v8 = maxVal;
                else
                    v8 = step + value;
                v7 = va("%g", v8);
                Dvar_SetFromStringByName(item->dvar, v7);
                return 1;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            return 0;
        }
    }
}

int __cdecl UI_GetKeyBindingLocalizedString(int localClientNum, const char *command, char *keys)
{
    return GetKeyBindingLocalizedString(localClientNum, command, keys, 0);
}

int __cdecl Display_KeyBindPending()
{
    return g_waitingForKey;
}

int __cdecl Item_Bind_HandleKey(UiContext *dc, itemDef_s *item, int key, int down)
{
    bool v5; // [esp+3h] [ebp-Dh]
    int bindCount; // [esp+4h] [ebp-Ch]
    int boundKeys[2]; // [esp+8h] [ebp-8h] BYREF

    if (g_waitingForKey)
    {
        if (g_bindItem)
        {
            if ((key & K_CHAR_FLAG) != 0)
            {
                return 1;
            }
            else if (key == K_ESCAPE)
            {
                g_waitingForKey = 0;
                return 1;
            }
            else if (key == '`')
            {
                return 1;
            }
            else
            {
                bindCount = Key_GetCommandAssignment(dc->localClientNum, item->dvar, boundKeys);
                if (key == K_BACKSPACE || bindCount == 2)
                {
                    Key_SetBinding(dc->localClientNum, boundKeys[0], (char *)"");
                    Key_SetBinding(dc->localClientNum, boundKeys[1], (char *)"");
                }
                if (key != K_BACKSPACE)
                    Key_SetBinding(dc->localClientNum, key, (char *)item->dvar);
                g_waitingForKey = 0;
                return 1;
            }
        }
        else
        {
            return 0;
        }
    }
    else if (down && (key != K_MOUSE1 || !Item_ContainsMouse(dc, item) ? (v5 = key == K_ENTER) : (v5 = 1), v5))
    {
        g_waitingForKey = 1;
        g_bindItem = item;
        return 1;
    }
    else
    {
        return 0;
    }
}

void __cdecl Menu_SetFeederSelection(UiContext *dc, menuDef_t *menu, int feeder, int index, const char *name)
{
    int i; // [esp+8h] [ebp-Ch]
    itemDef_s *item; // [esp+Ch] [ebp-8h]
    listBoxDef_s *listPtr; // [esp+10h] [ebp-4h]

    if (!menu)
    {
        if (name)
            menu = Menus_FindByName(dc, name);
        else
            menu = Menu_GetFocused(dc);
    }
    if (menu)
    {
        for (i = 0; i < menu->itemCount; ++i)
        {
            item = menu->items[i];
            if (item->special == (double)feeder)
            {
                item->cursorPos[dc->localClientNum] = index;
                UI_FeederSelection(dc->localClientNum, item->special, index);
                listPtr = Item_GetListBoxDef(item);
                if (listPtr)
                {
                    if (index)
                    {
                        if (listPtr->startPos[dc->localClientNum] > index)
                            listPtr->startPos[dc->localClientNum] = index;
                    }
                    else
                    {
                        item->cursorPos[dc->localClientNum] = 0;
                        listPtr->startPos[dc->localClientNum] = 0;
                    }
                }
            }
        }
    }
}

int __cdecl Menus_AnyFullScreenVisible(UiContext *dc)
{
    int i; // [esp+4h] [ebp-4h]

    for (i = dc->openMenuCount - 1; i >= 0; --i)
    {
        if (Window_IsVisible(dc->localClientNum, &dc->menuStack[i]->window) && dc->menuStack[i]->fullScreen)
            return 1;
    }
    return 0;
}

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

#ifdef KISAK_SP
void __cdecl Item_Text_GetSubtitle(StringTable *table, char *outText)
{
	unsigned int timeMsec;
	int timeRemaining;
	int low;
	int high;
	int mid;
	int columnCount;
	const char **values;
	const char *refName;
	const char *startTimeStr;
	const char *endTimeStr;
	const char *subtitleText;
	int compare;
	char cinematicName[64];

	*outText = 0;
	timeRemaining = 1024;

	if (!table)
		return;

	if (!cg_subtitles->current.enabled)
		return;

	if (R_Cinematic_IsFinished())
		return;

	if (!cinematicGlob.currentCinematicName[0])
		return;

	timeMsec = cinematicGlob.timeInMsec;
	I_strncpyz(cinematicName, cinematicGlob.currentCinematicName, 64);

	if (ui_cinematicsTimestamp->current.enabled)
	{
		Com_sprintf(outText, 1024, "%i.%01is ", timeMsec / 1000, (timeMsec % 1000) / 100);
		timeRemaining = 1024 - (int)strlen(outText);
	}

	low = 0;
	high = table->rowCount - 1;

	while (high >= 0)
	{
		columnCount = table->columnCount;
		mid = (low + high) >> 1;
		if (columnCount > 0 && mid < table->rowCount && mid >= 0 && (values = table->values, values[mid * columnCount]))
			refName = values[mid * columnCount];
		else
			refName = "";
		
		compare = strcmp(cinematicName, refName);
		if (compare < 0)
		{
			high = mid - 1;
			continue;
		}
		
		if (compare <= 0)
		{
			startTimeStr = StringTable_GetColumnValueForRow(table, mid, 1);
			if (timeMsec < (unsigned int)(atof(startTimeStr) * 1000.0))
			{
				high = mid - 1;
				continue;
			}
			
			endTimeStr = StringTable_GetColumnValueForRow(table, mid, 2);
			if (timeMsec < (unsigned int)(atof(endTimeStr) * 1000.0))
			{
				subtitleText = StringTable_GetColumnValueForRow(table, mid, 3);
				I_strncpyz(outText, subtitleText, timeRemaining > 0 ? timeRemaining : 0);
				return;
			}
		}
		
		low = mid + 1;
		if (low > high)
			return;
		
		high = high;
	}
}
#endif

extern const float MY_SUBTITLE_GLOWCOLOR[4];
void __cdecl Item_TextField_Paint(UiContext *dc, itemDef_s *item)
{
    char *VariantString; // eax
    int maxPaintChars; // [esp+24h] [ebp-478h]
    int v4; // [esp+28h] [ebp-474h]
    float t; // [esp+2Ch] [ebp-470h]
    float v6; // [esp+30h] [ebp-46Ch]
    float v7; // [esp+3Ch] [ebp-460h]
    char cursor; // [esp+4Bh] [ebp-451h]
    editFieldDef_s *editPtr; // [esp+4Ch] [ebp-450h]
    float lowLight[4]; // [esp+50h] [ebp-44Ch] BYREF
    Font_s *font; // [esp+60h] [ebp-43Ch]
    int offset; // [esp+64h] [ebp-438h]
    menuDef_t *parent; // [esp+68h] [ebp-434h]
    char buff[1028]; // [esp+6Ch] [ebp-430h] BYREF
    int maxChars; // [esp+474h] [ebp-28h]
    float x; // [esp+478h] [ebp-24h]
    const rectDef_s *rect; // [esp+47Ch] [ebp-20h]
    const dvar_s *dvar; // [esp+480h] [ebp-1Ch]
    const char *text; // [esp+484h] [ebp-18h]
    const rectDef_s *textRect; // [esp+488h] [ebp-14h]
    float newColor[4]; // [esp+48Ch] [ebp-10h] BYREF

    PROF_SCOPED("Item_TextField_Paint");

    parent = item->parent;
    editPtr = Item_GetEditFieldDef(item);
    if (editPtr)
    {
        Item_Text_Paint(dc, item);
        buff[0] = 0;
        if (item->dvar)
        {
            if (item->type == 17)
            {
                dvar = Dvar_FindVar(item->dvar);
                if (dvar && dvar->type == DVAR_TYPE_FLOAT)
                    Com_LocalizedFloatToString(dvar->current.value, buff, 0x400u, 2u);
            }
            else
            {
                VariantString = (char *)Dvar_GetVariantString(item->dvar);
                I_strncpyz(buff, VariantString, 1024);
            }
        }
        Item_SetTextExtents(dc->localClientNum, item, buff);
        parent = item->parent;
        if (Window_HasFocus(dc->localClientNum, &item->window))
        {
            lowLight[0] = parent->focusColor[0] * 0.800000011920929;
            lowLight[1] = parent->focusColor[1] * 0.800000011920929;
            lowLight[2] = parent->focusColor[2] * 0.800000011920929;
            lowLight[3] = parent->focusColor[3] * 0.800000011920929;
            v7 = (float)(dc->realTime / 75);
            v6 = sin(v7);
            t = v6 * 0.5 + 0.5;
            LerpColor(parent->focusColor, lowLight, newColor, t);
        }
        else
        {
            newColor[0] = item->window.foreColor[0];
            newColor[1] = item->window.foreColor[1];
            newColor[2] = item->window.foreColor[2];
            newColor[3] = item->window.foreColor[3];
        }
        if (item->text && *item->text)
            v4 = 8;
        else
            v4 = 0;
        offset = v4;
        textRect = Item_GetTextRect(dc->localClientNum, item);
        x = textRect->x + textRect->w + (double)offset;
        text = &buff[editPtr->paintOffset];
        if (editPtr->maxPaintChars)
            maxPaintChars = editPtr->maxPaintChars;
        else
            maxPaintChars = 0x7FFFFFFF;
        maxChars = maxPaintChars;
        font = UI_GetFontHandle(&scrPlaceView[dc->localClientNum], item->fontEnum, item->textscale);
        if (item == g_editItem && g_editingField)
        {
            cursor = Key_GetOverstrikeMode(dc->localClientNum) != 0 ? 95 : 124;
            if (!item)
                MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
            rect = &item->window.rect;
            UI_DrawTextWithCursor(
                &scrPlaceView[dc->localClientNum],
                (char *)text,
                maxChars,
                font,
                x,
                textRect->y,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                item->textscale,
                newColor,
                item->textStyle,
                item->cursorPos[dc->localClientNum] - editPtr->paintOffset,
                cursor);
        }
        else
        {
            UI_DrawText(
                &scrPlaceView[dc->localClientNum],
                (char *)text,
                maxChars,
                font,
                x,
                textRect->y,
                textRect->horzAlign,
                textRect->vertAlign,
                item->textscale,
                newColor,
                item->textStyle);
        }
    }
}

void __cdecl Item_Multi_Paint(UiContext *dc, itemDef_s *item)
{
    PROF_SCOPED("Item_Multi_Paint");
    item->text = Item_Multi_Setting(item);
    Item_Text_Paint(dc, item);
}

const char *__cdecl Item_Multi_Setting(itemDef_s *item)
{
    const char *VariantString; // eax
    multiDef_s *multiPtr; // [esp+0h] [ebp-10h]
    const char *string; // [esp+4h] [ebp-Ch]
    int i; // [esp+8h] [ebp-8h]
    int ia; // [esp+8h] [ebp-8h]
    float value; // [esp+Ch] [ebp-4h]

    multiPtr = Item_GetMultiDef(item);
    if (!multiPtr)
        return "<dvarStrList or dvarFloatList not set>";
    if (multiPtr->strDef)
    {
        string = Dvar_GetVariantString(item->dvar);
        for (i = 0; i < multiPtr->count; ++i)
        {
            if (!I_stricmp(string, multiPtr->dvarStr[i]))
                return multiPtr->dvarList[i];
        }
    }
    else
    {
        VariantString = Dvar_GetVariantString(item->dvar);
        value = atof(VariantString);
        for (ia = 0; ia < multiPtr->count; ++ia)
        {
            if (value == multiPtr->dvarValue[ia])
                return multiPtr->dvarList[ia];
        }
    }
    return "";
}

void __cdecl Item_DvarEnum_Paint(UiContext *dc, itemDef_s *item)
{
    PROF_SCOPED("Item_DvarEnum_Paint");
    item->text = Item_DvarEnum_Setting(item);
    Item_Text_Paint(dc, item);
}

const char *__cdecl Item_DvarEnum_Setting(itemDef_s *item)
{
    const char *v2; // eax
    int enumIndex; // [esp+0h] [ebp-8h]
    const dvar_s *enumDvar; // [esp+4h] [ebp-4h]

    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3159, 0, "%s", "item");
    if (item->type != 13)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 3160, 0, "%s\n\t(item->type) = %i", "(item->type == 13)", item->type);
    if (!item->typeData.listBox)
        return "<dvarEnumList not set>";
    enumDvar = Dvar_FindVar(item->typeData.enumDvarName);
    if (enumDvar->type != 6)
        return "<not an enum dvar>";
    if (!enumDvar->domain.enumeration.stringCount)
        return "";
    enumIndex = Item_DvarEnum_EnumIndex(item);
    if (enumIndex < 0 || enumIndex >= enumDvar->domain.enumeration.stringCount)
    {
        v2 = va("%s %i:%i", item->typeData.enumDvarName, enumDvar->domain.enumeration.stringCount, enumIndex);
        MyAssertHandler(
            ".\\ui\\ui_shared.cpp",
            3172,
            0,
            "%s\n\t%s",
            "enumIndex >= 0 && enumIndex < enumDvar->domain.enumeration.stringCount",
            v2);
    }
    return *(const char **)(enumDvar->domain.integer.max + 4 * enumIndex);
}

void __cdecl Item_Slider_Paint(UiContext *dc, itemDef_s *item)
{
    const char *VariantString; // eax
    float v3; // [esp+20h] [ebp-64h]
    float v4; // [esp+24h] [ebp-60h]
    float v5; // [esp+28h] [ebp-5Ch]
    float x0; // [esp+2Ch] [ebp-58h]
    float t; // [esp+30h] [ebp-54h]
    float v8; // [esp+34h] [ebp-50h]
    float v9; // [esp+3Ch] [ebp-48h]
    float v10; // [esp+40h] [ebp-44h]
    float lowLight[4]; // [esp+4Ch] [ebp-38h] BYREF
    const ScreenPlacement *scrPlace; // [esp+5Ch] [ebp-28h]
    menuDef_t *parent; // [esp+60h] [ebp-24h]
    float x; // [esp+64h] [ebp-20h]
    float y; // [esp+68h] [ebp-1Ch]
    const rectDef_s *rect; // [esp+6Ch] [ebp-18h]
    float value; // [esp+70h] [ebp-14h]
    float newColor[4]; // [esp+74h] [ebp-10h] BYREF

    PROF_SCOPED("Item_Slider_Paint");

    parent = item->parent;
    if (item->dvar)
    {
        VariantString = Dvar_GetVariantString(item->dvar);
        v9 = atof(VariantString);
    }
    else
    {
        v9 = 0.0;
    }
    value = v9;
    if (Window_HasFocus(dc->localClientNum, &item->window))
    {
        lowLight[0] = parent->focusColor[0] * 0.800000011920929;
        lowLight[1] = parent->focusColor[1] * 0.800000011920929;
        lowLight[2] = parent->focusColor[2] * 0.800000011920929;
        lowLight[3] = parent->focusColor[3] * 0.800000011920929;
        v10 = (float)(dc->realTime / 75);
        v8 = sin(v10);
        t = v8 * 0.5 + 0.5;
        LerpColor(parent->focusColor, lowLight, newColor, t);
    }
    else
    {
        newColor[0] = item->window.foreColor[0];
        newColor[1] = item->window.foreColor[1];
        newColor[2] = item->window.foreColor[2];
        newColor[3] = item->window.foreColor[3];
    }
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    x0 = item->window.rect.x + item->textalignx;
    x = Item_GetRectPlacementX(item->textAlignMode & 3, x0, item->window.rect.w, 96.0);
    v5 = rect->y + item->textaligny;
    y = Item_GetRectPlacementY(item->textAlignMode & 0xC, v5, rect->h, 16.0);
    scrPlace = &scrPlaceView[dc->localClientNum];
    UI_DrawHandlePic(
        scrPlace,
        x,
        y,
        96.0,
        16.0,
        rect->horzAlign,
        rect->vertAlign,
        newColor,
        sharedUiInfo.assets.sliderBar);
    x = Item_Slider_ThumbPosition(dc->localClientNum, item);
    v4 = y - 2.0;
    v3 = x - 5.0;
    UI_DrawHandlePic(
        scrPlace,
        v3,
        v4,
        10.0,
        20.0,
        rect->horzAlign,
        rect->vertAlign,
        newColor,
        sharedUiInfo.assets.sliderThumb);
}

double __cdecl Item_GetRectPlacementY(int alignY, float y0, float containerHeight, float selfHeight)
{
    if (alignY == 12)
    {
        return (float)(containerHeight - selfHeight + y0);
    }
    else if (alignY == 8)
    {
        return (float)((containerHeight - selfHeight) * 0.5 + y0);
    }
    else
    {
        if (alignY != 4)
        {
            if (alignY)
                MyAssertHandler(".\\ui\\ui_shared.cpp", 202, 0, "%s\n\t(alignY) = %i", "(alignY == 4 || alignY == 0)", alignY);
        }
        return y0;
    }
}

void __cdecl Item_Bind_Paint(UiContext *dc, itemDef_s *item)
{
    PROF_SCOPED("Item_Bind_Paint");
    char nameBind[260]; // [esp+0h] [ebp-108h] BYREF

    if (g_waitingForKey && g_bindItem == item)
    {
        item->text = "@MENU_BIND_KEY_PENDING";
    }
    else
    {
        UI_GetKeyBindingLocalizedString(dc->localClientNum, item->dvar, nameBind);
        item->text = nameBind;
    }
    Item_Text_Paint(dc, item);
    item->text = "";
}

void __cdecl Item_ListBox_Paint(UiContext *dc, itemDef_s *item)
{
    float width; // [esp+18h] [ebp-78h]
    float v3; // [esp+24h] [ebp-6Ch]
    float v4; // [esp+28h] [ebp-68h]
    float v5; // [esp+2Ch] [ebp-64h]
    float v6; // [esp+30h] [ebp-60h]
    float v7; // [esp+34h] [ebp-5Ch]
    float v8; // [esp+38h] [ebp-58h]
    float v9; // [esp+3Ch] [ebp-54h]
    float v10; // [esp+40h] [ebp-50h]
    float v11; // [esp+44h] [ebp-4Ch]
    float v12; // [esp+48h] [ebp-48h]
    float v13; // [esp+4Ch] [ebp-44h]
    float v14; // [esp+50h] [ebp-40h]
    float v15; // [esp+54h] [ebp-3Ch]
    float h; // [esp+58h] [ebp-38h]
    float w; // [esp+5Ch] [ebp-34h]
    Material *image; // [esp+64h] [ebp-2Ch]
    Material *imagea; // [esp+64h] [ebp-2Ch]
    int j; // [esp+68h] [ebp-28h]
    ScreenPlacement *scrPlace; // [esp+6Ch] [ebp-24h]
    float thumb; // [esp+70h] [ebp-20h]
    float thumba; // [esp+70h] [ebp-20h]
    listBoxDef_s *listPtr; // [esp+74h] [ebp-1Ch]
    float sizeb; // [esp+78h] [ebp-18h]
    float size; // [esp+78h] [ebp-18h]
    float sizec; // [esp+78h] [ebp-18h]
    float sizea; // [esp+78h] [ebp-18h]
    int i; // [esp+7Ch] [ebp-14h]
    int ia; // [esp+7Ch] [ebp-14h]
    int ib; // [esp+7Ch] [ebp-14h]
    rectDef_s *rect; // [esp+80h] [ebp-10h]
    float xe; // [esp+84h] [ebp-Ch]
    float xf; // [esp+84h] [ebp-Ch]
    float x; // [esp+84h] [ebp-Ch]
    float xa; // [esp+84h] [ebp-Ch]
    float xb; // [esp+84h] [ebp-Ch]
    float xc; // [esp+84h] [ebp-Ch]
    float xd; // [esp+84h] [ebp-Ch]
    float y; // [esp+88h] [ebp-8h]
    float ya; // [esp+88h] [ebp-8h]
    float ye; // [esp+88h] [ebp-8h]
    float yf; // [esp+88h] [ebp-8h]
    float yb; // [esp+88h] [ebp-8h]
    float yc; // [esp+88h] [ebp-8h]
    float yd; // [esp+88h] [ebp-8h]
    int count; // [esp+8Ch] [ebp-4h]

    PROF_SCOPED("Item_ListBox_Paint");

    listPtr = Item_GetListBoxDef(item);
    if (listPtr)
    {
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        rect = &item->window.rect;
        scrPlace = &scrPlaceView[dc->localClientNum];
        count = UI_FeederCount(dc->localClientNum, item->special);
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
        if ((item->window.staticFlags & 0x200000) != 0)
        {
            xe = rect->x + 1.0;
            y = item->window.rect.y + item->window.rect.h - 16.0 - 1.0;
            UI_DrawHandlePic(
                scrPlace,
                xe,
                y,
                16.0,
                16.0,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                0,
                sharedUiInfo.assets.scrollBarArrowLeft);
            xf = xe + 15.0;
            sizeb = item->window.rect.w - 32.0;
            w = sizeb + 1.0;
            UI_DrawHandlePic(
                scrPlace,
                xf,
                y,
                w,
                16.0,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                0,
                sharedUiInfo.assets.scrollBar);
            x = sizeb - 1.0 + xf;
            UI_DrawHandlePic(
                scrPlace,
                x,
                y,
                16.0,
                16.0,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                0,
                sharedUiInfo.assets.scrollBarArrowRight);
            thumb = Item_ListBox_ThumbDrawPosition(dc, item);
            if (thumb > x - 16.0 - 1.0)
                thumb = x - 16.0 - 1.0;
            UI_DrawHandlePic(
                scrPlace,
                thumb,
                y,
                16.0,
                16.0,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                0,
                sharedUiInfo.assets.scrollBarThumb);
            listPtr->endPos[dc->localClientNum] = listPtr->startPos[dc->localClientNum];
            size = item->window.rect.w - 2.0;
            if (listPtr->elementStyle == 1)
            {
                xa = rect->x + 1.0;
                ya = item->window.rect.y + 1.0;
                for (i = listPtr->startPos[dc->localClientNum]; i < count; ++i)
                {
                    image = UI_FeederItemImage(item->special, i);
                    if (image)
                    {
                        h = listPtr->elementHeight - 2.0;
                        v15 = listPtr->elementWidth - 2.0;
                        v14 = ya + 1.0;
                        v13 = xa + 1.0;
                        UI_DrawHandlePic(
                            scrPlace,
                            v13,
                            v14,
                            v15,
                            h,
                            item->window.rect.horzAlign,
                            item->window.rect.vertAlign,
                            0,
                            image);
                    }
                    if (i == item->cursorPos[dc->localClientNum])
                    {
                        v12 = listPtr->elementHeight - 1.0;
                        v11 = listPtr->elementWidth - 1.0;
                        UI_DrawRect(
                            scrPlace,
                            xa,
                            ya,
                            v11,
                            v12,
                            item->window.rect.horzAlign,
                            item->window.rect.vertAlign,
                            item->window.borderSize,
                            item->window.borderColor);
                    }
                    size = size - listPtr->elementWidth;
                    if (listPtr->elementWidth > (double)size)
                    {
                        listPtr->drawPadding = (int)size;
                        return;
                    }
                    xa = xa + listPtr->elementWidth;
                    ++listPtr->endPos[dc->localClientNum];
                }
            }
        }
        else
        {
            UI_OverrideCursorPos(dc->localClientNum, item);
            if (!listPtr->noScrollBars)
            {
                xb = item->window.rect.x + item->window.rect.w - 16.0 - 1.0;
                ye = item->window.rect.y + 1.0;
                UI_DrawHandlePic(
                    scrPlace,
                    xb,
                    ye,
                    16.0,
                    16.0,
                    item->window.rect.horzAlign,
                    item->window.rect.vertAlign,
                    0,
                    sharedUiInfo.assets.scrollBarArrowUp);
                yf = ye + 15.0;
                listPtr->endPos[dc->localClientNum] = listPtr->startPos[dc->localClientNum];
                sizec = item->window.rect.h - 32.0;
                v10 = sizec + 1.0;
                UI_DrawHandlePic(
                    scrPlace,
                    xb,
                    yf,
                    16.0,
                    v10,
                    item->window.rect.horzAlign,
                    item->window.rect.vertAlign,
                    0,
                    sharedUiInfo.assets.scrollBar);
                yb = sizec - 1.0 + yf;
                UI_DrawHandlePic(
                    scrPlace,
                    xb,
                    yb,
                    16.0,
                    16.0,
                    item->window.rect.horzAlign,
                    item->window.rect.vertAlign,
                    0,
                    sharedUiInfo.assets.scrollBarArrowDown);
                thumba = Item_ListBox_ThumbDrawPosition(dc, item);
                if (thumba > yb - 16.0 - 1.0)
                    thumba = yb - 16.0 - 1.0;
                UI_DrawHandlePic(
                    scrPlace,
                    xb,
                    thumba,
                    16.0,
                    16.0,
                    item->window.rect.horzAlign,
                    item->window.rect.vertAlign,
                    0,
                    sharedUiInfo.assets.scrollBarThumb);
            }
            sizea = item->window.rect.h - 2.0;
            if (listPtr->elementStyle == 1)
            {
                xc = rect->x + 1.0;
                yc = item->window.rect.y + 1.0;
                listPtr->endPos[dc->localClientNum] = listPtr->startPos[dc->localClientNum];
                for (ia = listPtr->startPos[dc->localClientNum]; ia < count; ++ia)
                {
                    imagea = UI_FeederItemImage(item->special, ia);
                    if (imagea)
                    {
                        v9 = listPtr->elementHeight - 2.0;
                        v8 = listPtr->elementWidth - 2.0;
                        v7 = yc + 1.0;
                        v6 = xc + 1.0;
                        UI_DrawHandlePic(
                            scrPlace,
                            v6,
                            v7,
                            v8,
                            v9,
                            item->window.rect.horzAlign,
                            item->window.rect.vertAlign,
                            0,
                            imagea);
                    }
                    if (ia == item->cursorPos[dc->localClientNum])
                    {
                        v5 = listPtr->elementHeight - 1.0;
                        v4 = listPtr->elementWidth - 1.0;
                        UI_DrawRect(
                            scrPlace,
                            xc,
                            yc,
                            v4,
                            v5,
                            item->window.rect.horzAlign,
                            item->window.rect.vertAlign,
                            item->window.borderSize,
                            item->window.borderColor);
                    }
                    ++listPtr->endPos[dc->localClientNum];
                    sizea = sizea - listPtr->elementHeight;
                    if (listPtr->elementHeight > (double)sizea)
                    {
                        listPtr->drawPadding = (int)(listPtr->elementHeight - sizea);
                        return;
                    }
                    yc = yc + listPtr->elementHeight;
                }
            }
            else
            {
                xd = rect->x + 1.0;
                yd = item->window.rect.y + 1.0;
                listPtr->endPos[dc->localClientNum] = listPtr->startPos[dc->localClientNum];
                for (ib = listPtr->startPos[dc->localClientNum]; ib < count; ++ib)
                {
                    Item_ListBox_PaintBackground(dc->localClientNum, item, xd, yd);
                    if (ib == item->cursorPos[dc->localClientNum])
                        Item_ListBox_PaintHighlight(dc->localClientNum, item, xd, yd);
                    if (listPtr->numColumns <= 0)
                    {
                        Item_ListBox_PaintTextElem(
                            dc->localClientNum,
                            item,
                            ib,
                            0,
                            0x7FFFFFFF,
                            0,
                            xd,
                            yd,
                            listPtr->elementWidth,
                            listPtr->elementHeight);
                    }
                    else
                    {
                        for (j = 0; j < listPtr->numColumns; ++j)
                        {
                            width = (float)listPtr->columnInfo[j].width;
                            v3 = (double)listPtr->columnInfo[j].pos + xd;
                            Item_ListBox_PaintTextElem(
                                dc->localClientNum,
                                item,
                                ib,
                                j,
                                listPtr->columnInfo[j].maxChars,
                                listPtr->columnInfo[j].alignment,
                                v3,
                                yd,
                                width,
                                listPtr->elementHeight);
                        }
                    }
                    sizea = sizea - listPtr->elementHeight;
                    if (listPtr->elementHeight > (double)sizea)
                    {
                        listPtr->drawPadding = (int)(listPtr->elementHeight - sizea);
                        return;
                    }
                    ++listPtr->endPos[dc->localClientNum];
                    yd = yd + listPtr->elementHeight;
                }
            }
        }
    }
}

double __cdecl Item_ListBox_ThumbDrawPosition(UiContext *dc, itemDef_s *item)
{
    float max; // [esp+Ch] [ebp-Ch]
    float maxa; // [esp+Ch] [ebp-Ch]
    float min; // [esp+10h] [ebp-8h]
    float mina; // [esp+10h] [ebp-8h]

    if (itemCapture == item)
    {
        if (!item)
        {
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\../ui/ui_utils.h", 53, 0, "%s", "w");
        }
        if ((item->window.staticFlags & 0x200000) != 0)
        {
            min = item->window.rect.x + 16.0 + 1.0;
            if (dc->cursor.x >= min + 8.0)
            {
                max = item->window.rect.x + item->window.rect.w - 32.0 - 1.0;
                if (dc->cursor.x <= max + 8.0)
                    return (float)(dc->cursor.x - 8.0);
            }
        }
        else
        {
            mina = item->window.rect.y + 16.0 + 1.0;
            if (dc->cursor.y >= mina + 8.0)
            {
                maxa = item->window.rect.y + item->window.rect.h - 32.0 - 1.0;
                if (dc->cursor.y <= maxa + 8.0)
                    return (float)(dc->cursor.y - 8.0);
            }
        }
    }
    return Item_ListBox_ThumbPosition(dc->localClientNum, item);
}

void __cdecl Item_ListBox_PaintTextElem(
    int localClientNum,
    itemDef_s *item,
    int row,
    int col,
    int maxChars,
    int alignment,
    float x,
    float y,
    float w,
    float h)
{
    float scale; // [esp+1Ch] [ebp-48h]
    float v11; // [esp+20h] [ebp-44h]
    float v12; // [esp+24h] [ebp-40h]
    float imageColor[4]; // [esp+2Ch] [ebp-38h] BYREF
    Font_s *font; // [esp+3Ch] [ebp-28h]
    const ScreenPlacement *scrPlace; // [esp+40h] [ebp-24h]
    Material *optionalImage; // [esp+44h] [ebp-20h] BYREF
    float xadj; // [esp+48h] [ebp-1Ch]
    const rectDef_s *rect; // [esp+4Ch] [ebp-18h]
    float color[4]; // [esp+50h] [ebp-14h] BYREF
    const char *text; // [esp+60h] [ebp-4h]

    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    scrPlace = &scrPlaceView[localClientNum];
    text = UI_FeederItemText(localClientNum, item, item->special, row, col, &optionalImage);
    UI_FeederItemColor(localClientNum, item, item->special, row, col, color);
    if (optionalImage)
    {
        imageColor[0] = 1.0;
        imageColor[1] = 1.0;
        imageColor[2] = 1.0;
        imageColor[3] = color[3];
        UI_DrawHandlePic(scrPlace, x, y, w, h, rect->horzAlign, rect->vertAlign, imageColor, optionalImage);
    }
    else if (text)
    {
        font = UI_GetFontHandle(&scrPlaceView[localClientNum], item->fontEnum, item->textscale);
        scale = (float)UI_TextWidth(text, maxChars, font, item->textscale);
        xadj = Item_GetTextAlignAdj(alignment, w, scale);
        v12 = y + h + item->textaligny;
        v11 = x + xadj + item->textalignx;
        UI_DrawText(
            scrPlace,
            (char *)text,
            maxChars,
            font,
            v11,
            v12,
            rect->horzAlign,
            rect->vertAlign,
            item->textscale,
            color,
            item->textStyle);
    }
}

double __cdecl Item_GetTextAlignAdj(int alignment, float width, float textWidth)
{
    float v5; // [esp+4h] [ebp-18h]
    float v7; // [esp+Ch] [ebp-10h]
    float v8; // [esp+14h] [ebp-8h]
    float v9; // [esp+18h] [ebp-4h]

    if (alignment == 1)
    {
        v9 = (width - textWidth) * 0.5;
        v7 = 0.0 - v9;
        if (v7 < 0.0)
            return (float)((width - textWidth) * 0.5);
        else
            return (float)0.0;
    }
    else if (alignment == 2)
    {
        v8 = width - textWidth;
        v5 = 0.0 - v8;
        if (v5 < 0.0)
            return (float)(width - textWidth);
        else
            return (float)0.0;
    }
    else
    {
        if (alignment)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 5243, 0, "%s\n\t(alignment) = %i", "(alignment == 0)", alignment);
        return 0.0;
    }
}

void __cdecl Item_ListBox_PaintBackground(int localClientNum, itemDef_s *item, float x, float y)
{
    float v4; // [esp+1Ch] [ebp-18h]
    float v5; // [esp+20h] [ebp-14h]
    float width; // [esp+24h] [ebp-10h]
    listBoxDef_s *listPtr; // [esp+2Ch] [ebp-8h]

    if (!item->window.style)
    {
        listPtr = Item_GetListBoxDef(item);
        if (!listPtr)
            MyAssertHandler(".\\ui\\ui_shared.cpp", 5301, 0, "%s", "listPtr");
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        width = item->window.rect.w - 16.0 - 4.0;
        v5 = y + 2.0;
        v4 = x + 2.0;
        UI_FillRect(
            &scrPlaceView[localClientNum],
            v4,
            v5,
            width,
            listPtr->elementHeight,
            item->window.rect.horzAlign,
            item->window.rect.vertAlign,
            item->window.backColor);
    }
}

void __cdecl Item_ListBox_PaintHighlight(int localClientNum, itemDef_s *item, float x, float y)
{
    float v4; // [esp+20h] [ebp-3Ch]
    float v5; // [esp+24h] [ebp-38h]
    float w; // [esp+2Ch] [ebp-30h]
    float v7; // [esp+30h] [ebp-2Ch]
    float v8; // [esp+34h] [ebp-28h]
    float v9; // [esp+38h] [ebp-24h]
    float v10; // [esp+3Ch] [ebp-20h]
    float v11; // [esp+40h] [ebp-1Ch]
    float v12; // [esp+44h] [ebp-18h]
    float width; // [esp+48h] [ebp-14h]
    ScreenPlacement *scrPlace; // [esp+4Ch] [ebp-10h]
    listBoxDef_s *listPtr; // [esp+50h] [ebp-Ch]
    Material *optionalImage; // [esp+54h] [ebp-8h]

    listPtr = Item_GetListBoxDef(item);
    if (!listPtr)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 5318, 0, "%s", "listPtr");
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    scrPlace = &scrPlaceView[localClientNum];
    width = item->window.rect.w - 16.0 - 4.0;
    v12 = y + 2.0;
    v11 = x + 2.0;
    UI_FillRect(
        scrPlace,
        v11,
        v12,
        width,
        listPtr->elementHeight,
        item->window.rect.horzAlign,
        item->window.rect.vertAlign,
        item->window.outlineColor);
    v10 = listPtr->elementHeight + 1.0;
    v9 = item->window.rect.w - 16.0 - 3.0;
    v8 = y + 1.0;
    v7 = x + 1.0;
    UI_DrawRect(
        scrPlace,
        v7,
        v8,
        v9,
        v10,
        item->window.rect.horzAlign,
        item->window.rect.vertAlign,
        1.0,
        listPtr->selectBorder);
    optionalImage = listPtr->selectIcon;
    if (optionalImage)
    {
        if (!listPtr->notselectable)
        {
            w = listPtr->elementHeight - 4.0;
            v5 = y + 4.0;
            v4 = x + 4.0;
            UI_DrawHandlePic(
                scrPlace,
                v4,
                v5,
                w,
                w,
                item->window.rect.horzAlign,
                item->window.rect.vertAlign,
                0,
                optionalImage);
        }
    }
}

void __cdecl Item_OwnerDraw_Paint(UiContext *dc, itemDef_s *item)
{
    rectDef_s clamp; // [esp+44h] [ebp-90h]
    float v3; // [esp+60h] [ebp-74h]
    float x; // [esp+64h] [ebp-70h]
    float v5; // [esp+68h] [ebp-6Ch]
    float v6; // [esp+6Ch] [ebp-68h]
    float t; // [esp+74h] [ebp-60h]
    float v8; // [esp+78h] [ebp-5Ch]
    float v9; // [esp+84h] [ebp-50h]
    float v10; // [esp+88h] [ebp-4Ch]
    int localClientNum; // [esp+9Ch] [ebp-38h]
    float lowLight[4]; // [esp+A0h] [ebp-34h] BYREF
    Font_s *font; // [esp+B0h] [ebp-24h]
    menuDef_t *parent; // [esp+B4h] [ebp-20h]
    float color[4]; // [esp+B8h] [ebp-1Ch] BYREF
    int flags; // [esp+C8h] [ebp-Ch] BYREF
    const rectDef_s *rect; // [esp+CCh] [ebp-8h]
    const rectDef_s *textRect; // [esp+D0h] [ebp-4h]

    PROF_SCOPED("Item_OwnerDraw_Paint");

    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 5521, 0, "%s", "item");
    parent = item->parent;
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
    Fade(
        &flags,
        &item->window.foreColor[3],
        parent->fadeClamp,
        &item->window.nextTime,
        parent->fadeCycle,
        1,
        parent->fadeAmount,
        parent->fadeInAmount,
        dc);
    Window_SetDynamicFlags(dc->localClientNum, &item->window, flags);
    color[0] = item->window.foreColor[0];
    color[1] = item->window.foreColor[1];
    color[2] = item->window.foreColor[2];
    color[3] = item->window.foreColor[3];
    if (Window_HasFocus(dc->localClientNum, &item->window))
    {
        lowLight[0] = parent->focusColor[0] * 0.800000011920929;
        lowLight[1] = parent->focusColor[1] * 0.800000011920929;
        lowLight[2] = parent->focusColor[2] * 0.800000011920929;
        lowLight[3] = parent->focusColor[3] * 0.800000011920929;
        v10 = (float)(dc->realTime / 75);
        v8 = sin(v10);
        t = v8 * 0.5 + 0.5;
        LerpColor(parent->focusColor, lowLight, color, t);
    }
    else if (item->textStyle == 1 && ((dc->realTime / 256) & 1) == 0)
    {
        lowLight[0] = item->window.foreColor[0] * 0.800000011920929;
        lowLight[1] = item->window.foreColor[1] * 0.800000011920929;
        lowLight[2] = item->window.foreColor[2] * 0.800000011920929;
        lowLight[3] = item->window.foreColor[3] * 0.800000011920929;
        v9 = (float)(dc->realTime / 75);
        v6 = sin(v9);
        v5 = v6 * 0.5 + 0.5;
        LerpColor(item->window.foreColor, lowLight, color, v5);
    }
    if ((item->dvarFlags & 3) != 0 && !Item_EnableShowViaDvar(item, 1))
    {
        color[0] = parent->disableColor[0];
        color[1] = parent->disableColor[1];
        color[2] = parent->disableColor[2];
        color[3] = parent->disableColor[3];
    }
    if (!item)
        MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
    rect = &item->window.rect;
    font = UI_GetFontHandle(&scrPlaceView[dc->localClientNum], item->fontEnum, item->textscale);
    if (item->text)
    {
        Item_Text_Paint(dc, item);
        textRect = Item_GetTextRect(dc->localClientNum, item);
        clamp = item->parent->window.rect;
        if (*item->text)
        {
            x = textRect->x + textRect->w + 8.0;
            UI_OwnerDraw(
                dc->localClientNum,
                x,
                rect->y,
                rect->w,
                rect->h,
                rect->horzAlign,
                rect->vertAlign,
                0.0,
                item->textaligny,
                item->window.ownerDraw,
                item->window.ownerDrawFlags,
                item->alignment,
                item->special,
                font,
                item->textscale,
                color,
                item->window.background,
                item->textStyle,
                clamp,
                item->textAlignMode);
        }
        else
        {
            v3 = textRect->x + textRect->w;
            UI_OwnerDraw(
                dc->localClientNum,
                v3,
                rect->y,
                rect->w,
                rect->h,
                rect->horzAlign,
                rect->vertAlign,
                0.0,
                item->textaligny,
                item->window.ownerDraw,
                item->window.ownerDrawFlags,
                item->alignment,
                item->special,
                font,
                item->textscale,
                color,
                item->window.background,
                item->textStyle,
                clamp,
                item->textAlignMode);
        }
    }
    else
    {
        UI_OwnerDraw(
            dc->localClientNum,
            rect->x,
            rect->y,
            rect->w,
            rect->h,
            rect->horzAlign,
            rect->vertAlign,
            item->textalignx,
            item->textaligny,
            item->window.ownerDraw,
            item->window.ownerDrawFlags,
            item->alignment,
            item->special,
            font,
            item->textscale,
            color,
            item->window.background,
            item->textStyle,
            item->parent->window.rect,
            item->textAlignMode);
    }
}

void __cdecl Item_GameMsgWindow_Paint(UiContext *dc, itemDef_s *item)
{
    const char *v2; // eax
    const char *v3; // eax
    Font_s *font; // [esp+1Ch] [ebp-18h]
    float color[5]; // [esp+20h] [ebp-14h] BYREF

    PROF_SCOPED("Item_GameMsgWindow_Paint");

    if (!dc)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 5580, 0, "%s", "dc");
    if (!item)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 5581, 0, "%s", "item");
    if (CL_IsCgameInitialized(dc->localClientNum))
    {
        if (item->gameMsgWindowIndex >= 4u)
        {
            v2 = va("Game message window index %d is not valid. Must be in [0, %d).", item->gameMsgWindowIndex, 4);
            Com_Error(ERR_DROP, v2);
        }
        if (item->gameMsgWindowMode >= 2u && item->gameMsgWindowMode != 3 && item->gameMsgWindowMode != 2)
        {
            v3 = va("Game message window mode %d is not valid.", item->gameMsgWindowMode);
            Com_Error(ERR_DROP, v3);
        }
        color[0] = item->window.foreColor[0];
        color[1] = item->window.foreColor[1];
        color[2] = item->window.foreColor[2];
        color[3] = item->window.foreColor[3];
        if (!item)
            MyAssertHandler("c:\\trees\\cod3\\src\\ui\\ui_utils_api.h", 36, 0, "%s", "w");
        //LODWORD(color[4]) = &item->window.rect;
        font = UI_GetFontHandle(&scrPlaceView[dc->localClientNum], item->fontEnum, item->textscale);
        Con_DrawGameMessageWindow(
            dc->localClientNum,
            item->gameMsgWindowIndex,
            (int)item->window.rect.x,
            (int)item->window.rect.y,
            item->window.rect.horzAlign,
            item->window.rect.vertAlign,
            font,
            item->textscale,
            color,
            item->textStyle,
            item->textAlignMode,
            (msgwnd_mode_t)item->gameMsgWindowMode);
    }
}

int __cdecl Menu_Count(UiContext *dc)
{
    return dc->menuCount;
}

void __cdecl Menu_PaintAll_BeginVisibleList(char *stringBegin, uint32_t stringSize)
{
    PROF_SCOPED("Menu_PaintAll_BeginVisibleList");

    char VISIBLE_LIST_PREFIX[14]; // [esp+0h] [ebp-14h] BYREF

    strcpy(VISIBLE_LIST_PREFIX, "ui_showlist: ");
    if (stringSize < 0xE)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 6048, 0, "%s", "stringSize >= sizeof( VISIBLE_LIST_PREFIX )");
    *(uint32_t *)stringBegin = *(uint32_t *)VISIBLE_LIST_PREFIX;
    *((uint32_t *)stringBegin + 1) = *(uint32_t *)&VISIBLE_LIST_PREFIX[4];
    *((uint32_t *)stringBegin + 2) = *(uint32_t *)&VISIBLE_LIST_PREFIX[8];
    *((_WORD *)stringBegin + 6) = *(_WORD *)&VISIBLE_LIST_PREFIX[12];
}

void __cdecl Menu_PaintAll_AppendToVisibleList(char *stringBegin, uint32_t stringSize, char *stringToAppend)
{
    PROF_SCOPED("Menu_PaintAll_AppendToVisibleList");

    uint32_t v3; // [esp+0h] [ebp-64h]
    std::reverse_iterator<char *> result; // [esp+44h] [ebp-20h] BYREF
    char _Val; // [esp+53h] [ebp-11h] BYREF
    const char *lastNewline; // [esp+54h] [ebp-10h]
    int VISIBLE_LIST_LINE_LENGTH; // [esp+58h] [ebp-Ch]
    char *stringEnd; // [esp+5Ch] [ebp-8h]
    const char *terminus; // [esp+60h] [ebp-4h]

    VISIBLE_LIST_LINE_LENGTH = 80;
    v3 = strlen(stringBegin);
    stringEnd = &stringBegin[v3];
    I_strncat(stringBegin, stringSize, stringToAppend);
    _Val = 10;

    std::reverse_iterator<char *> _First = std::reverse_iterator<char *>(stringBegin); // [esp+48h] [ebp-1Ch]
    std::reverse_iterator<char *> _Last = std::reverse_iterator<char *>(&stringBegin[v3]); // [esp+4Ch] [ebp-18h]

    std::string kisak(stringBegin);

    //_Last.current = stringBegin;
    //_First.current = &stringBegin[v3];
    //lastNewline = std::find<std::reverse_iterator<char *>, char>(
    //    &result,
    //    _Last,
    //    _First,
    //    //(std::reverse_iterator<char *>) & stringBegin[v3],
    //    //(std::reverse_iterator<char *>)stringBegin,
    //    &_Val)->current - 1;
    auto it = std::find<std::reverse_iterator<char *>, char>(_Last, _First, _Val); // KISAKTODO: i'd be surprised if this works.
    lastNewline = it.base() - 1;

    if (stringEnd - lastNewline <= 80)
        terminus = ", ";
    else
        terminus = ",\n  ";
    I_strncat(stringBegin, stringSize, (char *)terminus);
}

void __cdecl Menu_PaintAll_DrawVisibleList(char *stringBegin, UiContext *dc)
{
    PROF_SCOPED("Menu_PaintAll_DrawVisibleList");

    float color[4]; // [esp+28h] [ebp-18h] BYREF
    float y; // [esp+38h] [ebp-8h]

    float MESSAGE_SCALE = 0.2f;
    color[0] = 0.75;
    color[1] = 1.0;
    color[2] = 0.5;
    color[3] = 1.0;
    if (dc->FPS == 0.0)
        y = 320.0;
    else
        y = 400.0;
    UI_DrawText(
        &scrPlaceFull,
        stringBegin,
        0x7FFFFFFF,
        sharedUiInfo.assets.smallFont,
        0.0,
        y,
        0,
        0,
        MESSAGE_SCALE,
        color,
        0);
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

void __cdecl TRACK_ui_shared()
{
    track_static_alloc_internal(&scrollInfo, 32, "scrollInfo", 34);
    track_static_alloc_internal((void *)commandList, 336, "commandList", 34);
}

void __cdecl UI_AddMenuList(UiContext *dc, MenuList *menuList)
{
    int i; // [esp+0h] [ebp-4h]

    if (menuList)
    {
        for (i = 0; i < menuList->menuCount; ++i)
            UI_AddMenu(dc, menuList->menus[i]);
    }
}

void __cdecl UI_AddMenu(UiContext *dc, menuDef_t *menu)
{
    if (dc->menuCount >= 640)
        Com_Error(ERR_DROP, "UI_AddMenu: EXE_ERR_OUT_OF_MEMORY");
    if (!menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 6297, 0, "%s", "menu");
    if (dc->menuCount >= 0x280u)
        MyAssertHandler(
            ".\\ui\\ui_shared.cpp",
            6298,
            0,
            "dc->menuCount doesn't index ARRAY_COUNT( dc->Menus )\n\t%i not in [0, %i)",
            dc->menuCount,
            640);
    if (IsFastFileLoad() && DB_FindXAssetHeader(ASSET_TYPE_MENU, menu->window.name).menu != menu)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 6304, 0, "%s", "touchMenu == menu");
    dc->Menus[dc->menuCount++] = menu;
}

int __cdecl UI_PlayLocalSoundAliasByName(uint32_t localClientNum, const char *aliasname)
{
    return SND_PlayLocalSoundAliasByName(localClientNum, aliasname, SASYS_UI);
}

int __cdecl UI_GetMenuScreen()
{
    int menu; // [esp+0h] [ebp-4h]

    menu = UI_GetForcedMenuScreen();
    if (menu >= 0)
        return menu;
    else
        return 1;
}

int __cdecl UI_GetForcedMenuScreen()
{
    return -1;
}

int __cdecl UI_GetMenuScreenForError()
{
    int menu; // [esp+0h] [ebp-4h]

    menu = UI_GetForcedMenuScreen();
    if (menu >= 0)
        return menu;
    else
        return 0;
}

MenuList *__cdecl UI_LoadMenu(char *menuFile, int imageTrack)
{
    if (IsFastFileLoad())
        return UI_LoadMenus_FastFile(menuFile);
    else
        return UI_LoadMenu_LoadObj(menuFile, imageTrack);
}

MenuList *__cdecl UI_LoadMenus(char *menuFile, int imageTrack)
{
    if (IsFastFileLoad())
        return UI_LoadMenus_FastFile(menuFile);
    else
        return UI_LoadMenus_LoadObj(menuFile, imageTrack);
}

MenuList *__cdecl UI_LoadMenus_FastFile(const char *menuFile)
{
    return DB_FindXAssetHeader(ASSET_TYPE_MENULIST, menuFile).menuList;
}


void Item_RunScript(UiContext *dc,itemDef_s *item,char *script)
{
#if defined(KISAK_SP) && !defined(KISAK_XBOX)
    constexpr unsigned count=46;
#else
    constexpr unsigned count=42;
#endif
    UI_RunScriptCommands(dc,item,script,commandList,count,UI_RunMenuScript);
}
