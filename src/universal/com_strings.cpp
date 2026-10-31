#include <universal/q_shared.h>
#include "com_memory.h"
#include "assertive.h"
#include <script/scr_stringlist.h>

const char* CopyString(const char* in)
{
	uint32_t out;

	iassert(in);

	out = SL_GetString_(in, 0, MT_TYPE_GENERIC);
	return SL_ConvertToString(out);
}

void FreeString(const char* str)
{
	iassert(str);

	uint32_t out = SL_FindString(str);

	iassert(out);

	SL_RemoveRefToString(out);
}

const char* __cdecl CopyString(char* in)
{
    iassert(in);

    return SL_ConvertToString(SL_GetString_(in, 0, MT_TYPE_GENERIC));
}

void __cdecl ReplaceString(const char** str, const char* in)
{
    const char* newStr; // [esp+0h] [ebp-4h]

    if (!str)
        MyAssertHandler(".\\universal\\com_memory.cpp", 845, 0, "%s", "str");
    if (!in)
        MyAssertHandler(".\\universal\\com_memory.cpp", 846, 0, "%s", "in");
    newStr = CopyString(in);
    if (*str)
        FreeString(*str);
    *str = newStr;
}
