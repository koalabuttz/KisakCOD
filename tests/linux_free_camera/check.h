// tests/linux_free_camera/check.h
//
// Minimal check helpers shared by the free-camera tests. Every check is counted;
// the process exits non-zero when any check failed or when no check ran.

#pragma once

#include <cmath>
#include <cstdio>
#include <exception>

namespace free_camera_check {

inline int g_checks = 0;
inline int g_failures = 0;

inline void Check(bool condition, const char *label)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", label);
    }
}

inline void Near(double actual, double expected, double tolerance, const char *label)
{
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::printf("FAIL  %s: got %.9g expected %.9g (tolerance %.3g)\n", label, actual, expected, tolerance);
    }
}

template <class Exception, class Function>
void Throws(Function &&function, const char *label)
{
    ++g_checks;
    try {
        function();
    } catch (const Exception &) {
        return;
    } catch (const std::exception &other) {
        ++g_failures;
        std::printf("FAIL  %s: threw a different exception: %s\n", label, other.what());
        return;
    }
    ++g_failures;
    std::printf("FAIL  %s: did not throw\n", label);
}

inline int Finish(const char *suite)
{
    if (g_checks == 0) {
        std::printf("%s: no checks ran\n", suite);
        return 1;
    }
    std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

} // namespace free_camera_check
