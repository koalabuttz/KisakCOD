#include <universal/q_shared.h>
#include "ui_script_core.h"
#include "ui_input_core.h"

void UI_RunScriptCommands(UiContext *dc,itemDef_s *item,char *s,const commandDef_t *commandList,unsigned count,void(*unknown)(int,const char**,const char*))
{
    int v3; // [esp+0h] [ebp-1814h]
    uint8_t dst[5120]; // [esp+4h] [ebp-1810h] BYREF
    char out[1028]; // [esp+1404h] [ebp-410h] BYREF
    uint32_t i; // [esp+180Ch] [ebp-8h]
    const char *p; // [esp+1810h] [ebp-4h] BYREF

    memset(dst, 0, sizeof(dst));
    if (item && s && *s)
    {
        I_strncat((char *)dst, 5120, s);
        p = (char *)dst;
        while (String_Parse((const char **)&p, out, 1024))
        {
            if (out[0] != 59 || out[1])
            {
                v3 = 0;
                for(i=0;i<count;++i)
                {
                    if (!I_stricmp(out, commandList[i].name))
                    {
                        commandList[i].handler(dc, item, (const char **)&p);
                        v3 = 1;
                        break;
                    }
                }
                if (!v3)
                    unknown(dc->localClientNum, &p, s);
            }
        }
    }
}

void __cdecl Script_Close(UiContext *dc, itemDef_s *item, const char **args)
{
    char name[1028]; // [esp+14h] [ebp-408h] BYREF

    if (String_Parse(args, name, 1024))
    {
        if (!strcmp(name, "self"))
            Menus_Close(dc, item->parent);
        else
            Menus_CloseByName(dc, name);
    }
}

void __cdecl Script_SetLocalVarBool(UiContext *dc, itemDef_s *item, const char **args)
{
    int v3; // eax
    UILocalVarContext *var; // [esp+0h] [ebp-10Ch]
    char string[260]; // [esp+4h] [ebp-108h] BYREF

    var = Script_ParseLocalVar(dc, args);
    if (String_Parse(args, string, 256))
    {
        if (var)
        {
            v3 = atoi(string);
            UILocalVar_SetBool(var->table, v3 != 0);
        }
    }
}

UILocalVarContext *__cdecl Script_ParseLocalVar(UiContext *dc, const char **args)
{
    char varName[260]; // [esp+0h] [ebp-108h] BYREF

    if (String_Parse(args, varName, 256))
        return UILocalVar_FindOrCreate(&dc->localVars, varName);
    else
        return 0;
}

void __cdecl Script_SetLocalVarInt(UiContext *dc, itemDef_s *item, const char **args)
{
    int v3; // eax
    UILocalVarContext *var; // [esp+0h] [ebp-10Ch]
    char string[260]; // [esp+4h] [ebp-108h] BYREF

    var = Script_ParseLocalVar(dc, args);
    if (String_Parse(args, string, 256))
    {
        if (var)
        {
            v3 = atoi(string);
            UILocalVar_SetInt(var->table, v3);
        }
    }
}

void __cdecl Script_SetLocalVarString(UiContext *dc, itemDef_s *item, const char **args)
{
    UILocalVarContext *var; // [esp+0h] [ebp-10Ch]
    char string[260]; // [esp+4h] [ebp-108h] BYREF

    var = Script_ParseLocalVar(dc, args);
    if (String_Parse(args, string, 256))
    {
        if (var)
            UILocalVar_SetString(var->table, string);
    }
}

void __cdecl Script_Play(UiContext *dc, itemDef_s *item, const char **args)
{
    char val[1028]; // [esp+0h] [ebp-408h] BYREF

    if (String_Parse(args, val, 1024))
        UI_PlayLocalSoundAliasByName(dc->localClientNum, val);
}
