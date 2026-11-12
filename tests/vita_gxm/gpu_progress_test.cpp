// Host test of src/gfx_gxm/gxm_gpu_progress.h: which frames the GPU has
// finished, from the notifications of each frame's last scene. Run by
// scripts/tests/test_vita_gpu_progress.py.

#include <gfx_gxm/gxm_gpu_progress.h>

#include <cstdio>
#include <cstdlib>

namespace
{
int g_failures = 0;

void Check(bool ok, const char *what, int line)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using gfx_gxm::GpuProgress;

// The notification region as the GPU would write it.
struct Region
{
    uint32_t words[GpuProgress::kSlots] = {};
    uint32_t operator()(uint32_t word) const { return words[word]; }
};

// Sends frame `stamp` with a notification and returns its word; `value` is
// what the GPU writes when it is done.
uint32_t Send(GpuProgress &progress, Region &region, uint64_t stamp, uint32_t &value)
{
    const uint32_t word = progress.Submit(stamp, value);
    region.words[word] = 0;
    return word;
}

void TestNothingDrawn()
{
    GpuProgress progress;
    CHECK(progress.Done(0)); // never drawn
    CHECK(!progress.Done(1));
    CHECK(!progress.Expected(1));
}

void TestOneFrameAhead()
{
    GpuProgress progress;
    Region region;
    uint32_t value1 = 0, value2 = 0;
    const uint32_t word1 = Send(progress, region, 1, value1);
    const uint32_t word2 = Send(progress, region, 2, value2);
    CHECK(value1 != 0 && value2 != 0 && value1 != value2);
    CHECK(word1 != word2);
    progress.Poll(region);
    CHECK(!progress.Done(1) && !progress.Done(2));
    CHECK(progress.Expected(1) && progress.Expected(2) && !progress.Expected(3));
    // The GPU ends frame 1.
    region.words[word1] = value1;
    progress.Poll(region);
    CHECK(progress.Done(1) && !progress.Done(2));
    CHECK(progress.Expected(2));
    region.words[word2] = value2;
    progress.Poll(region);
    CHECK(progress.Done(2) && !progress.Expected(2));
}

void TestLaterFrameSpeaksForEarlier()
{
    // Frame 3 ended without a scene, so it has no notification: frame 4's
    // says it is done too.
    GpuProgress progress;
    Region region;
    uint32_t value = 0;
    progress.Finished(2);
    CHECK(!progress.Done(3) && !progress.Expected(3));
    const uint32_t word = Send(progress, region, 4, value);
    CHECK(progress.Expected(3) && progress.Expected(4) && !progress.Expected(5));
    region.words[word] = value;
    progress.Poll(region);
    CHECK(progress.Done(3) && progress.Done(4) && !progress.Done(5));
}

void TestWholeWait()
{
    // A wait for the whole GPU ends every notification still out.
    GpuProgress progress;
    Region region;
    uint32_t value5 = 0, value6 = 0;
    Send(progress, region, 5, value5);
    const uint32_t word6 = Send(progress, region, 6, value6);
    progress.Finished(6);
    CHECK(progress.Done(5) && progress.Done(6) && !progress.Expected(5) && !progress.Expected(6));
    // The late write changes nothing, and progress never goes back.
    region.words[word6] = value6;
    progress.Poll(region);
    progress.Finished(4);
    CHECK(progress.Done(6) && !progress.Done(7));
}

void TestSlotsComeRound()
{
    // A word is used again kSlots frames later, with another value: the old
    // value still standing there does not end the new frame.
    GpuProgress progress;
    Region region;
    uint64_t stamp = 1;
    uint32_t value = 0;
    uint32_t word = Send(progress, region, stamp, value);
    region.words[word] = value;
    progress.Poll(region);
    CHECK(progress.Done(1));
    const uint32_t stale = value;
    for (stamp = 2; stamp <= 1 + GpuProgress::kSlots; ++stamp)
    {
        const uint32_t at = progress.Submit(stamp, value); // the region is not cleared: the worst case
        if (stamp < 1 + GpuProgress::kSlots)
        {
            region.words[at] = value;
            progress.Poll(region);
            CHECK(progress.Done(stamp));
        }
        else
        {
            CHECK(at == word && value != stale);
            progress.Poll(region);
            CHECK(!progress.Done(stamp));
        }
    }
}

void TestValueNeverZero()
{
    GpuProgress progress;
    progress.lastValue = 0xffffffffu;
    uint32_t value = 0;
    progress.Submit(1, value);
    CHECK(value != 0);
}
} // namespace

int main()
{
    TestNothingDrawn();
    TestOneFrameAhead();
    TestLaterFrameSpeaksForEarlier();
    TestWholeWait();
    TestSlotsComeRound();
    TestValueNeverZero();
    if (g_failures)
    {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("gpu progress: all checks passed");
    return EXIT_SUCCESS;
}
