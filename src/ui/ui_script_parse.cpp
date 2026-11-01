#include <universal/q_shared.h>
#include "ui_shared.h"
#include <universal/q_parse.h>
#include <qcommon/qcommon.h>
#include <stringed/stringed_hooks.h>

int __cdecl String_Parse(const char **p, char *out, int len)
{
    char *pszTranslated; // [esp+0h] [ebp-8h]
    parseInfo_t *token; // [esp+4h] [ebp-4h]

    token = Com_ParseOnLine(p);
    if (!token)
        MyAssertHandler(".\\ui\\ui_shared.cpp", 275, 0, "%s", "token");
    if (!*p)
        return 0;
    if (token->token[0] == 64)
    {
        pszTranslated = (char *)SEH_StringEd_GetString(&token->token[1]);
        if (pszTranslated)
        {
            I_strncpyz(out, pszTranslated, len);
            return 1;
        }
        if (Dvar_GetBool("loc_warnings"))
        {
            if (Dvar_GetBool("loc_warningsAsErrors"))
                Com_Error(ERR_LOCALIZATION, "Could not translate menu string reference %s", token->token);
            else
                Com_PrintWarning(CON_CHANNEL_UI, "WARNING: Could not translate menu string reference %s\n", token->token);
        }
    }
    I_strncpyz(out, token->token, len);
    return 1;
}
