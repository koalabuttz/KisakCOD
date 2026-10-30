#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <universal/assertive.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Assertion handling.
//
// Replaces universal/assertive.cpp for the native Linux target. The Windows
// implementation walks minidump map files and opens message boxes; neither
// exists here. What matters is that an assertion reports the expression, file
// and line and then stops the server rather than continuing with bad state.
// ---------------------------------------------------------------------------

// Optional callback the engine can install to observe assertions.
void (*AssertCallback)(const char *) = nullptr;

void MyAssertHandler(const char *filename, int line, int type, const char *fmt, ...)
{
    char detail[4096];
    detail[0] = '\0';

    if (fmt)
    {
        va_list arguments;
        va_start(arguments, fmt);
        vsnprintf(detail, sizeof(detail), fmt, arguments);
        va_end(arguments);
    }

    char message[4600];
    snprintf(message, sizeof(message), "ASSERT FAILED: %s\n\t%s:%d\n",
        detail, filename ? filename : "(unknown)", line);

    // Goes through Sys_Error so the message reaches the console and the process
    // stops; the engine has no coherent state to continue from.
    Com_PrintError(CON_CHANNEL_SYSTEM, "%s\n", message);

    if (AssertCallback)
        AssertCallback(message);

    fflush(stdout);

    // type is non-zero for "fatal" assertions in the engine's convention; every
    // assertion is treated as fatal here because continuing past one is how the
    // original build produced silent corruption.
    (void)type;
    abort();
}
