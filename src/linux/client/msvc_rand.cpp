// rand() and srand() as the Microsoft C runtime defines them, for Linux targets
// that run the engine's client code.
//
// The engine was written against the Microsoft runtime, whose rand() returns
// 15 bits (RAND_MAX is 0x7FFF). Its callers depend on that: fx_system.cpp seeds
// an effect with 479 * rand() / 0x8000 and indexes a 479-entry table with it,
// fx_update.cpp and r_buffers.cpp scale rand() by 1/32768 or 1/32767, and
// cg_weapons.cpp compares it against a chance times 32768. glibc's rand() returns
// 31 bits, so on Linux those index past the table or land far outside the range.
//
// The definitions below follow the runtime: each thread keeps its own state, a
// thread starts as if srand(1) had been called, and each call advances the state
// with the runtime's constants and returns bits 16 to 30. Seeded the same way,
// Linux and Windows draw the same numbers.
//
// The executable's own definitions take the place of glibc's for every call the
// executable's objects make, without editing a production source. They are
// hidden, so the executable does not export them: shared libraries (DXVK, SDL,
// the C++ runtime) keep glibc's rand() together with glibc's RAND_MAX. No
// source the probe compiles reads RAND_MAX.
//
// Linked into: KisakCOD-world-draw-probe (scripts/platform/linux/world_draw_probe.cmake).
// The dedicated server does not link it: its callers mask or take a remainder,
// which is right with either rand(), and this file sits under src/linux/client,
// outside the dedicated target's src/linux/*.cpp glob. Test: tests/linux_compat/msvc_rand_test.cpp,
// check msvc_rand in scripts/tests/run_world_surface_checks.py.

#include <cstdint>

namespace
{
thread_local uint32_t t_holdrand = 1;
}

extern "C"
{
__attribute__((visibility("hidden"))) void srand(unsigned int seed) noexcept
{
    t_holdrand = seed;
}

__attribute__((visibility("hidden"))) int rand(void) noexcept
{
    t_holdrand = t_holdrand * 214013u + 2531011u;
    return int((t_holdrand >> 16) & 0x7FFFu);
}
}
