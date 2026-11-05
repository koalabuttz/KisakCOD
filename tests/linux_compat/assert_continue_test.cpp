// Checks src/linux/sys_assert.cpp's MyAssertHandler, linked on its own with a
// stand-in Com_PrintError that records what the handler reports.
//
//   * Without KISAK_ASSERT_CONTINUE, a failed assertion stops the process
//     (SIGABRT), checked in a forked child.
//   * With KISAK_ASSERT_CONTINUE=1 the handler reports the first failure of a
//     site and each power of two after it, and returns. Sites are file and
//     line; a different line, or the same file text at another address, is
//     the same site only when both match.
//   * Any other value of the variable leaves assertions fatal.
//
// The variable is read once per process, so each case runs in its own child.

#include <universal/assertive.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static std::string g_reported;

void Com_PrintError(int channel, const char *fmt, ...)
{
    (void)channel;
    char text[8192];
    va_list arguments;
    va_start(arguments, fmt);
    vsnprintf(text, sizeof(text), fmt, arguments);
    va_end(arguments);
    g_reported += text;
}

namespace
{
int g_failures;

void Check(bool condition, const char *what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

int CountOf(const std::string &text, const char *needle)
{
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++count;
    return count;
}

// Runs body in a child with KISAK_ASSERT_CONTINUE set to value (or unset when
// null). Returns the child's wait status.
template <typename Body>
int InChild(const char *value, Body body)
{
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0)
    {
        if (value)
            setenv("KISAK_ASSERT_CONTINUE", value, 1);
        else
            unsetenv("KISAK_ASSERT_CONTINUE");
        _exit(body() ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}

bool AbortsOnAssert()
{
    MyAssertHandler("fatal.cpp", 10, 0, "%s", "x != 0");
    return false; // not reached when the handler stops the process
}

bool ContinuesAndThrottles()
{
    // The same site from two copies of the file name text: one site.
    static const char kFileA[] = "level.cpp";
    char fileB[sizeof(kFileA)];
    std::memcpy(fileB, kFileA, sizeof(kFileA));
    for (int i = 0; i < 9; ++i)
        MyAssertHandler(i % 2 ? fileB : kFileA, 42, 0, "%s", "ent->health > 0");
    MyAssertHandler("level.cpp", 43, 0, "%s", "other");

    bool ok = true;
    // Failures 1, 2, 4 and 8 of line 42 are reported; 3, 5, 6, 7 and 9 are not.
    ok &= CountOf(g_reported, "ASSERT FAILED: ent->health > 0") == 4;
    ok &= g_reported.find("failure 1 of this site") != std::string::npos;
    ok &= g_reported.find("failure 8 of this site") != std::string::npos;
    ok &= g_reported.find("failure 9 of this site") == std::string::npos;
    ok &= CountOf(g_reported, "level.cpp:42") == 4;
    ok &= CountOf(g_reported, "ASSERT FAILED: other\n\tlevel.cpp:43") == 1;
    ok &= CountOf(g_reported, "continuing (KISAK_ASSERT_CONTINUE)") == 5;
    if (!ok)
        std::printf("reported:\n%s\n", g_reported.c_str());
    return ok;
}
} // namespace

int main()
{
    int status = InChild(nullptr, AbortsOnAssert);
    Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "an assertion aborts without KISAK_ASSERT_CONTINUE");

    status = InChild("0", AbortsOnAssert);
    Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "KISAK_ASSERT_CONTINUE=0 keeps assertions fatal");

    status = InChild("yes", AbortsOnAssert);
    Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "only the value 1 turns continuing on");

    status = InChild("1", ContinuesAndThrottles);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "KISAK_ASSERT_CONTINUE=1 reports each site at powers of two and returns");

    if (g_failures)
        return 1;
    std::printf("PASS: assert continue mode\n");
    return 0;
}
