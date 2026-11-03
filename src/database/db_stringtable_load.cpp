#include <universal/q_shared.h>
#include "database.h"

void Load_ScriptStringCustom(uint16_t *var)
{
    *var = (uint16_t)(uintptr_t)varXAssetList->stringList.strings[*var];
}

void Mark_ScriptStringCustom(uint16_t *var)
{
    if (*var)
        SL_AddUser(*var, 4u);
}

