// Asset-free checks for src/linux/client/msvc_rand.cpp: the Microsoft runtime's rand()
// and srand() that the world draw probe links in place of glibc's.
//
// The test calls rand() through <cstdlib> from its own translation unit, as the
// engine's sources do, so it also shows that the linked definitions replace
// glibc's for the executable's calls. The reference numbers for seed 1 are the
// runtime's well-known first ten. Shared libraries must keep glibc's rand(): the
// test looks rand up the way a shared library binds it (dlsym, RTLD_DEFAULT) and
// requires glibc's 31-bit function, not the executable's.
//
// Built by scripts/tests/run_world_surface_checks.py (check msvc_rand) with the
// unit beside it and -pthread.

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>
#include <vector>

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

// The Microsoft runtime's first ten numbers after srand(1), which is also where
// every thread starts.
constexpr int kSeedOne[] = {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};

bool DrawsSeedOne()
{
    for (const int expected : kSeedOne)
        if (std::rand() != expected)
            return false;
    return true;
}

std::vector<int> Draw(unsigned seed, int count)
{
    std::srand(seed);
    std::vector<int> numbers;
    for (int i = 0; i < count; ++i)
        numbers.push_back(std::rand());
    return numbers;
}
} // namespace

int main()
{
    // Nothing has called srand yet: the main thread starts as if srand(1) ran.
    Check(DrawsSeedOne(), "a thread that never called srand draws the runtime's seed-1 numbers");
    std::srand(1);
    Check(DrawsSeedOne(), "srand(1) draws the runtime's seed-1 numbers");

    Check(Draw(12345, 64) == Draw(12345, 64), "the same seed draws the same numbers");
    Check(Draw(12345, 64) != Draw(54321, 64), "different seeds draw different numbers");

    // Every number fits 15 bits, so the engine's arithmetic stays in range:
    // fx_system.cpp's effect seed indexes a 479-entry table, and the 1/32768
    // scalings stay below one.
    std::srand(1);
    bool fits = true;
    bool seedInTable = true;
    bool scaledBelowOne = true;
    int highest = 0;
    for (int i = 0; i < 1000000; ++i)
    {
        const int number = std::rand();
        fits = fits && number >= 0 && number <= 0x7FFF;
        seedInTable = seedInTable && 479 * number / 0x8000 < 479;
        scaledBelowOne = scaledBelowOne && double(number) * 0.000030517578125 < 1.0;
        highest = number > highest ? number : highest;
    }
    Check(fits, "every number is from 0 to 0x7FFF");
    Check(seedInTable, "479 * rand() / 0x8000 stays below 479");
    Check(scaledBelowOne, "rand() / 32768 stays below one");
    Check(highest == 0x7FFF, "the full 15-bit range is reached");

    // Each thread keeps its own state, as the runtime does: a new thread starts at
    // seed 1, and its draws do not move the main thread's sequence.
    const std::vector<int> alone = Draw(7, 16);
    std::srand(7);
    std::vector<int> interleaved;
    bool threadStartsAtOne = true;
    for (int i = 0; i < 16; ++i)
    {
        interleaved.push_back(std::rand());
        std::thread other([&threadStartsAtOne] { threadStartsAtOne = threadStartsAtOne && DrawsSeedOne(); });
        other.join();
    }
    Check(threadStartsAtOne, "a new thread draws the seed-1 numbers whatever the main thread seeded");
    Check(interleaved == alone, "another thread's draws do not move this thread's sequence");

    // A shared library binds rand through the dynamic symbol table. The
    // executable's definitions are hidden, so it finds glibc's: a different
    // function, whose numbers go past 15 bits.
    using RandFunction = int (*)();
    using SrandFunction = void (*)(unsigned);
    const auto libcRand = reinterpret_cast<RandFunction>(dlsym(RTLD_DEFAULT, "rand"));
    const auto libcSrand = reinterpret_cast<SrandFunction>(dlsym(RTLD_DEFAULT, "srand"));
    RandFunction ours = &std::rand;
    Check(libcRand != nullptr && libcSrand != nullptr, "the dynamic symbol table still has a rand and an srand");
    Check(libcRand != ours, "the executable does not export its rand");
    if (libcRand && libcSrand && libcRand != ours)
    {
        // Half of the executable's seed-1 sequence, then glibc's rand seeded and
        // drawn, then the other half: the two states are separate.
        std::srand(1);
        bool firstHalf = true;
        for (int i = 0; i < 5; ++i)
            firstHalf = firstHalf && std::rand() == kSeedOne[i];
        libcSrand(1);
        bool past15Bits = false;
        for (int i = 0; i < 64; ++i)
            past15Bits = past15Bits || libcRand() > 0x7FFF;
        bool secondHalf = true;
        for (int i = 5; i < 10; ++i)
            secondHalf = secondHalf && std::rand() == kSeedOne[i];
        Check(past15Bits, "shared libraries keep glibc's 31-bit rand");
        Check(firstHalf && secondHalf, "glibc's rand and the executable's keep separate states");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("PASS: msvc rand\n");
    return g_failures == 0 ? 0 : 1;
}
