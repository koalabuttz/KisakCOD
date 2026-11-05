#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <universal/assertive.h>

#include <pthread.h>
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

// KISAK_ASSERT_CONTINUE=1 in the environment: report each failing assertion
// site once and carry on, as the Windows assert dialog's "Ignore" does (and as a
// Windows Release build, which compiles iassert out, never stops at all). It is
// for surveying many levels in one run, not for judging a build: whatever runs
// after a failed assertion has state the code did not expect, so a crash or
// misbehaviour after one is not evidence of anything. Off unless set; the
// checks and CI never set it.
namespace
{
    struct ContinuedAssertSite
    {
        const char *filename;
        int line;
        unsigned count;
    };

    constexpr int kMaxContinuedAssertSites = 256;
    ContinuedAssertSite s_continuedSites[kMaxContinuedAssertSites];
    int s_continuedSiteCount;
    pthread_mutex_t s_continuedLock = PTHREAD_MUTEX_INITIALIZER;

    bool AssertContinueRequested()
    {
        static const bool requested = []
        {
            const char *value = getenv("KISAK_ASSERT_CONTINUE");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        return requested;
    }

    bool SameFile(const char *a, const char *b)
    {
        return a == b || (a && b && strcmp(a, b) == 0);
    }

    // How many times this site (file and line) has failed, this time included.
    // A full table counts further sites under its last entry.
    unsigned NoteContinuedAssert(const char *filename, int line)
    {
        pthread_mutex_lock(&s_continuedLock);
        ContinuedAssertSite *site = nullptr;
        for (int i = 0; i < s_continuedSiteCount; ++i)
        {
            if (SameFile(s_continuedSites[i].filename, filename) && s_continuedSites[i].line == line)
            {
                site = &s_continuedSites[i];
                break;
            }
        }
        if (!site)
        {
            site = s_continuedSiteCount < kMaxContinuedAssertSites
                ? &s_continuedSites[s_continuedSiteCount++]
                : &s_continuedSites[kMaxContinuedAssertSites - 1];
            if (site->count == 0)
            {
                site->filename = filename;
                site->line = line;
            }
        }
        const unsigned count = ++site->count;
        pthread_mutex_unlock(&s_continuedLock);
        return count;
    }
}

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

    if (AssertContinueRequested())
    {
        // Report the first failure of a site and every power of two after it,
        // so a site that fails each frame neither floods the log nor goes quiet.
        const unsigned count = NoteContinuedAssert(filename, line);
        if ((count & (count - 1)) == 0)
            Com_PrintError(CON_CHANNEL_SYSTEM, "%s\tcontinuing (KISAK_ASSERT_CONTINUE), failure %u of this site\n",
                message, count);
        return;
    }

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
