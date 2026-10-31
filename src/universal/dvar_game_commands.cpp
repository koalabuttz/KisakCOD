#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

extern cmd_function_s Dvar_Toggle_f_VAR;
extern cmd_function_s Dvar_TogglePrint_f_VAR;
extern cmd_function_s Dvar_Set_f_VAR;
extern cmd_function_s Dvar_SetS_f_VAR;
extern cmd_function_s Dvar_SetA_f_VAR;
extern cmd_function_s Dvar_SetFromDvar_f_VAR;
extern cmd_function_s Dvar_SetFromLocalizedStr_f_VAR;
extern cmd_function_s Dvar_SetToTime_f_VAR;
extern cmd_function_s Dvar_Reset_f_VAR;
extern cmd_function_s Dvar_List_f_VAR;
extern cmd_function_s Dvar_Dump_f_VAR;
extern cmd_function_s Dvar_RegisterBool_f_VAR;
extern cmd_function_s Dvar_RegisterInt_f_VAR;
extern cmd_function_s Dvar_RegisterFloat_f_VAR;
extern cmd_function_s Dvar_SetU_f_VAR;

void __cdecl Dvar_AddCommands()
{
    Cmd_AddCommandInternal("toggle", Dvar_Toggle_f, &Dvar_Toggle_f_VAR);
    Cmd_AddCommandInternal("togglep", Dvar_TogglePrint_f, &Dvar_TogglePrint_f_VAR);
    Cmd_AddCommandInternal("set", Dvar_Set_f, &Dvar_Set_f_VAR);
    Cmd_AddCommandInternal("sets", Dvar_SetS_f, &Dvar_SetS_f_VAR);
    Cmd_AddCommandInternal("seta", Dvar_SetA_f, &Dvar_SetA_f_VAR);
    Cmd_AddCommandInternal("setfromdvar", Dvar_SetFromDvar_f, &Dvar_SetFromDvar_f_VAR);
    Cmd_AddCommandInternal("setfromlocString", Dvar_SetFromLocalizedStr_f, &Dvar_SetFromLocalizedStr_f_VAR);
    Cmd_AddCommandInternal("setdvartotime", Dvar_SetToTime_f, &Dvar_SetToTime_f_VAR);
    Cmd_AddCommandInternal("reset", Dvar_Reset_f, &Dvar_Reset_f_VAR);
    Cmd_AddCommandInternal("dvarlist", Dvar_List_f, &Dvar_List_f_VAR);
    Cmd_AddCommandInternal("dvardump", Dvar_Dump_f, &Dvar_Dump_f_VAR);
    Cmd_AddCommandInternal("dvar_bool", Dvar_RegisterBool_f, &Dvar_RegisterBool_f_VAR);
    Cmd_AddCommandInternal("dvar_int", Dvar_RegisterInt_f, &Dvar_RegisterInt_f_VAR);
    Cmd_AddCommandInternal("dvar_float", Dvar_RegisterFloat_f, &Dvar_RegisterFloat_f_VAR);
    Cmd_AddCommandInternal("setu", Dvar_SetU_f, &Dvar_SetU_f_VAR);
}
