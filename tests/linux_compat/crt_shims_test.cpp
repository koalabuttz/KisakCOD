// Asset-free checks for the MSVC CRT shims in src/linux/compat.h that the
// client renderer, UI and effects sources call directly: _itoa, the array
// strcpy_s and _time64/_localtime64.
//
// The fail-closed paths (bad radix, null buffer, oversized strcpy_s source)
// terminate the process. They are checked by forking a child per case and
// requiring that it dies from a signal, so the parent's results stay intact.

#include <linux/compat.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void CheckItoa(int value, int radix, const char *expected)
{
    char buffer[40];
    std::memset(buffer, 'x', sizeof(buffer));
    char *returned = _itoa(value, buffer, radix);
    char what[160];
    std::snprintf(what, sizeof(what), "_itoa(%d, radix %d) == \"%s\" (got \"%s\")", value, radix, expected, buffer);
    Check(returned == buffer && std::strcmp(buffer, expected) == 0, what);
}

template <typename Body>
bool DiesFromSignal(Body body)
{
    std::fflush(stdout);
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0)
    {
        body();
        _exit(0); // reaching here means the shim accepted the bad input
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return false;
    return WIFSIGNALED(status);
}

char g_sink[64];
} // namespace

int main()
{
    // _itoa: the rb_stats call shape (radix 10 into char[32]) plus the edges.
    CheckItoa(0, 10, "0");
    CheckItoa(7, 10, "7");
    CheckItoa(1234567, 10, "1234567");
    CheckItoa(-42, 10, "-42");
    CheckItoa(INT_MAX, 10, "2147483647");
    CheckItoa(INT_MIN, 10, "-2147483648");
    CheckItoa(255, 16, "ff");
    CheckItoa(-1, 16, "ffffffff");  // non-decimal radices format the bit pattern
    CheckItoa(-1, 2, "11111111111111111111111111111111");
    CheckItoa(35, 36, "z");
    CheckItoa(5, 2, "101");
    {
        char exact[12]; // "-2147483648" plus NUL fits exactly
        Check(std::strcmp(_itoa(INT_MIN, exact, 10), "-2147483648") == 0, "_itoa fills an exactly sized buffer");
    }
    Check(DiesFromSignal([] { _itoa(1, g_sink, 1); }), "_itoa radix 1 terminates");
    Check(DiesFromSignal([] { _itoa(1, g_sink, 37); }), "_itoa radix 37 terminates");
    Check(DiesFromSignal([] { _itoa(1, nullptr, 10); }), "_itoa null buffer terminates");

    // strcpy_s: the fx_load_obj call shape copies a name into a fixed array.
    {
        char name[8];
        std::memset(name, 'x', sizeof(name));
        Check(strcpy_s(name, "abc") == 0 && std::strcmp(name, "abc") == 0, "strcpy_s copies a short name");
        Check(strcpy_s(name, "1234567") == 0 && std::strcmp(name, "1234567") == 0,
              "strcpy_s copies a name that exactly fills the array");
        Check(strcpy_s(name, "") == 0 && name[0] == '\0', "strcpy_s copies an empty name");
    }
    Check(DiesFromSignal([] {
              char name[8];
              strcpy_s(name, "12345678");
          }),
          "strcpy_s with no room for the terminator terminates instead of truncating");
    Check(DiesFromSignal([] {
              char name[8];
              strcpy_s(name, static_cast<const char *>(nullptr));
          }),
          "strcpy_s with a null source terminates");

    // _time64/_localtime64: the rb_logfile call shape.
    {
        const long long before = static_cast<long long>(std::time(nullptr));
        long long stamp = -1;
        const long long returned = _time64(&stamp);
        const long long after = static_cast<long long>(std::time(nullptr));
        Check(stamp == returned, "_time64 writes the value it returns");
        Check(before <= returned && returned <= after, "_time64 reports the current time");
        Check(_time64(nullptr) >= before, "_time64 accepts a null destination");

        struct tm *local = _localtime64(&stamp);
        time_t narrowed = static_cast<time_t>(stamp);
        struct tm expected;
        Check(local != nullptr && localtime_r(&narrowed, &expected) != nullptr && local->tm_year == expected.tm_year &&
                  local->tm_yday == expected.tm_yday && local->tm_hour == expected.tm_hour &&
                  local->tm_min == expected.tm_min,
              "_localtime64 matches localtime_r for the current time");
        Check(asctime(local) != nullptr, "the rb_logfile asctime consumer accepts the result");

        const long long epoch = 0;
        Check(_localtime64(&epoch) != nullptr, "_localtime64 accepts the epoch");
        const long long negative = -1;
        Check(_localtime64(&negative) == nullptr, "_localtime64 rejects a time before the epoch, as MSVC does");
        Check(_localtime64(nullptr) == nullptr, "_localtime64 rejects a null source");
        if (sizeof(time_t) == 4)
        {
            const long long beyond = static_cast<long long>(INT_MAX) + 1;
            Check(_localtime64(&beyond) == nullptr, "_localtime64 refuses a value 32-bit time_t cannot hold");
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("PASS: CRT shims\n");
    return g_failures == 0 ? 0 : 1;
}
