#include <universal/q_shared.h>
#include "ui_expression_text.h"
#include <qcommon/qcommon.h>
int lastWarnTime;
bool UI_TryLiteralExpression(const statement_s *statement,Operand *result) {
    if(!statement || !result || statement->numEntries!=2 || !statement->entries || !statement->entries[0] || !statement->entries[1])return false;
    const auto &op=*statement->entries[0];const auto &value=*statement->entries[1];
    if(op.type!=0 || op.data.op!=OP_LEFTPAREN || value.type!=1 || value.data.operand.dataType!=VAL_STRING || !value.data.operand.internals.string)return false;
    *result=value.data.operand;return true;
}

char *__cdecl GetExpressionResultString(int localClientNum, const statement_s *statement)
{
    static char resultString_2[256];

    const char *v3; // eax
    Operand result; // [esp+Ch] [ebp-Ch] BYREF
    int len; // [esp+14h] [ebp-4h]

    if (!EvaluateExpression(localClientNum, statement, &result))
        return (char *)"";
    len = 0;
    if (result.dataType)
    {
        if (result.dataType == VAL_FLOAT)
        {
            len = Com_sprintf(resultString_2, 0x100u, "%f", result.internals.floatVal);
        }
        else
        {
            if (result.dataType != VAL_STRING)
            {
                if (!alwaysfails)
                {
                    v3 = va("Unknown result datatype of %i", result.dataType);
                    MyAssertHandler(".\\ui\\ui_expressions.cpp", 2466, 0, v3);
                }
                return (char *)"";
            }
            len = Com_sprintf(resultString_2, 0x100u, "%s", result.internals.string);
        }
    }
    else
    {
        len = Com_sprintf(resultString_2, 0x100u, "%i", result.internals.intVal);
    }
    if (len < 0 && (int)(Sys_Milliseconds() - lastWarnTime) > 5000)
    {
        lastWarnTime = Sys_Milliseconds();
        Com_PrintWarning(
            CON_CHANNEL_UI,
            "Warning: Expression result string has been truncated, longer than %d characters: %s...\n",
            256,
            resultString_2);
    }
    return resultString_2;
}
