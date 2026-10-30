#include <universal/q_shared.h>

#include <win32/win_local.h>
#include <win32/win_localize.h>

#include <qcommon/qcommon.h>

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Localization.
//
// Replaces win32/win_localize.cpp, which loads a binary localized-strings table
// keyed by references like "WIN_OUT_OF_MEM_TITLE". There is no such asset for
// the native build, and inventing strings would hide missing content, so
// references are returned verbatim. Callers either display that reference or
// tolerate it; none branches on success.
// ---------------------------------------------------------------------------

static char s_language[] = "english";

int Win_InitLocalization()
{
    return 0;
}

void Win_ShutdownLocalization()
{
}

char *Win_LocalizeRef(const char *ref)
{
    // Returns the reference itself rather than a fabricated translation.
    return const_cast<char *>(ref ? ref : "");
}

char *Win_GetLanguage()
{
    return s_language;
}

char *Win_CopyLocalizationString(const char *string)
{
    if (!string)
        return nullptr;

    const size_t length = strlen(string);
    char *copy = (char *)malloc(length + 1);
    if (copy)
        memcpy(copy, string, length + 1);
    return copy;
}
