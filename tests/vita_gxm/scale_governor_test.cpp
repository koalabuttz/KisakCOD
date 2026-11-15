// Host test of src/gfx_gxm/gxm_scale_governor.h: the size of the Vita's 3D
// picture, picked from each frame's time and its waits for the GPU. Run by
// scripts/tests/test_vita_scale_governor.py.
//
// The console is a model here: the CPU needs `cpuMs` for a frame, the GPU
// `fixedMs` plus `pixelMs` times the picture's area, a frame takes the
// longer of the two and the CPU waits for the GPU the difference (the
// pacing wait of gxm_legacy_draw.cpp, PaceFrame).

#include <gfx_gxm/gxm_engine_options.h>
#include <gfx_gxm/gxm_scale_governor.h>

#include <cstdio>
#include <cstdlib>

// The engine option the helpers under test read (the back end defines it).
namespace gfx_gxm
{
bool g_sceneWidthOnly = false;
}

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

using gfx_gxm::ScaleGovernor;

struct Console
{
    double cpuMs = 29.6;
    double fixedMs = 10.65;
    double pixelMs = 24.85;
    double noiseMs = 0.0;   // each frame's CPU and GPU times move by up to this
    uint32_t hitchEvery = 0; // every so many frames the CPU takes 60 ms more
    ScaleGovernor::Limits limits{0.7f, 1.0f, 0.0f};

    ScaleGovernor governor;
    uint64_t now = 1'000'000;
    uint64_t frames = 0, changes = 0;
    uint32_t seed = 12345;
    double lastFrameMs = 0.0;
    float lowest = 1.0f;

    double Noise()
    {
        seed = seed * 1664525u + 1013904223u;
        return (double(seed >> 8) / double(1u << 24) * 2.0 - 1.0) * noiseMs;
    }

    double GpuMs(float scale) const
    {
        return fixedMs + pixelMs * double(scale) * double(scale);
    }

    void Frame()
    {
        const float scale = governor.Scale();
        double cpu = cpuMs + Noise();
        if (hitchEvery && frames % hitchEvery == hitchEvery - 1)
            cpu += 60.0;
        const double gpu = GpuMs(scale) + Noise();
        const double frame = (std::max)(cpu, gpu);
        const double wait = (std::max)(gpu - cpu, 0.0);
        now += uint64_t(frame * 1000.0);
        changes += governor.FrameEnded(now, uint64_t(wait * 1000.0), limits);
        lowest = (std::min)(lowest, governor.Scale());
        lastFrameMs = frame;
        ++frames;
    }

    void Run(double seconds)
    {
        const uint64_t until = now + uint64_t(seconds * 1e6);
        while (now < until)
            Frame();
    }
};

// Off: the engine's own size, to the bit, and no change.
void TestOff()
{
    Console c;
    c.limits = {1.0f, 1.0f, 0.0f};
    c.Run(5);
    CHECK(c.governor.Scale() == 1.0f);
    CHECK(c.changes == 0);
    c.limits = {0.7f, 0.7f, 0.0f};
    c.Run(5);
    CHECK(c.governor.Scale() == 0.7f);
    c.limits = {0.9f, 0.73f, 0.0f}; // the least above the most
    c.Run(5);
    CHECK(c.governor.Scale() == 0.73f);
    CHECK(c.governor.Totals().windows == 0);
}

// Console run BV's busy view: 35.5 ms at the full size, GPU-bound; the CPU
// needs 29.6 ms. The goal is 33.3 ms, so the picture shrinks only until the
// GPU fits in 32.3 ms, and stays there.
void TestBusyView()
{
    Console c;
    c.Run(3);
    const float settled = c.governor.Scale();
    CHECK(settled < 1.0f && settled >= 0.9f);
    CHECK(c.GpuMs(settled) <= ScaleGovernor::kSlowGoalMs - ScaleGovernor::kMarginMs + ScaleGovernor::kSlackMs + 0.01);
    const uint64_t changes = c.changes;
    c.lowest = 1.0f;
    c.Run(120);
    CHECK(c.changes == changes);
    CHECK(c.governor.Scale() == settled);
    CHECK(c.governor.Totals().triesLost == 0);
    CHECK(c.lowest == settled);
}

// The engine names a goal no CPU reaches: the GPU must never hold the
// frame. The picture then sits one step under the size at which it would,
// and the tries of the step above grow rare.
void TestNoWaitGoal()
{
    Console c;
    c.limits.goalMs = 1.0f;
    c.Run(4);
    const float settled = c.governor.Scale();
    CHECK(settled >= 0.84f && settled < 0.93f);
    CHECK(c.GpuMs(settled) <= c.cpuMs + 2 * ScaleGovernor::kSlackMs + 0.01);
    c.governor.ClearTotals();
    c.lowest = 1.0f;
    c.Run(120);
    const ScaleGovernor::Counts &counts = c.governor.Totals();
    CHECK(counts.tries >= 3 && counts.tries <= 8);
    CHECK(counts.triesLost == counts.tries || counts.triesLost + 1 == counts.tries);
    CHECK(c.lowest == settled);
    CHECK(counts.highest <= settled + 1.5f / ScaleGovernor::kSteps);
    // Nearly every frame at the settled size.
    CHECK(counts.scaleSum / double(counts.frames) < settled + 0.004);
}

// A view the CPU fills in 13 ms: the goal is 16.7 ms and the picture shrinks
// to reach it.
void TestFastGoal()
{
    Console c;
    c.cpuMs = 13.0;
    c.fixedMs = 6.0;
    c.pixelMs = 14.0;
    c.Run(4);
    const float settled = c.governor.Scale();
    CHECK(settled >= 0.78f && settled <= 0.85f);
    CHECK(c.lastFrameMs <= ScaleGovernor::kFastGoalMs - ScaleGovernor::kMarginMs + ScaleGovernor::kSlackMs + 0.01);
    CHECK(c.governor.Totals().goalMs == ScaleGovernor::kFastGoalMs);
    const uint64_t changes = c.changes;
    c.Run(60);
    CHECK(c.changes == changes);
}

// The same CPU, but the smallest picture allowed still needs 19.7 ms of the
// GPU: 16.7 ms is given up, the picture is whole again, and the tries of
// 16.7 ms grow rare.
void TestFastGoalOutOfReach()
{
    Console c;
    c.cpuMs = 13.0;
    c.fixedMs = 14.0;
    c.pixelMs = 12.0;
    c.Run(6);
    CHECK(c.governor.Totals().fastLost == 1);
    CHECK(c.governor.Scale() == 1.0f);
    CHECK(c.governor.Totals().goalMs == ScaleGovernor::kSlowGoalMs);
    c.governor.ClearTotals();
    c.Run(300);
    const ScaleGovernor::Counts &counts = c.governor.Totals();
    CHECK(counts.fastLost >= 2 && counts.fastLost <= 5);
    CHECK(counts.scaleSum / double(counts.frames) > 0.98);
    CHECK(c.lowest >= 0.6875f);
}

// The view empties: the picture is whole again within two seconds, with no
// try (the GPU's time is known to fit).
void TestViewEmpties()
{
    Console c;
    c.Run(5);
    CHECK(c.governor.Scale() < 1.0f);
    c.fixedMs = 8.0;
    c.pixelMs = 12.0;
    c.governor.ClearTotals();
    c.Run(2);
    CHECK(c.governor.Scale() == 1.0f);
    CHECK(c.governor.Totals().tries == 0);
    // And fills again.
    c.fixedMs = 10.65;
    c.pixelMs = 24.85;
    c.Run(2);
    CHECK(c.governor.Scale() < 1.0f && c.governor.Scale() >= 0.9f);
}

// A CPU slower than 33.3 ms is its own goal: the GPU must not add to it.
void TestSlowCpu()
{
    Console c;
    c.cpuMs = 40.0;
    c.fixedMs = 15.0;
    c.pixelMs = 30.0;
    c.Run(5);
    const float settled = c.governor.Scale();
    CHECK(settled < 1.0f && settled >= 0.85f);
    CHECK(c.GpuMs(settled) <= c.cpuMs + 2 * ScaleGovernor::kSlackMs + 0.01);
    c.lowest = 1.0f;
    c.Run(60);
    CHECK(c.lowest >= settled - 0.001f);
}

// Never under the least allowed, however slow the GPU.
void TestLeast()
{
    Console c;
    c.fixedMs = 30.0;
    c.pixelMs = 40.0;
    c.limits.least = 0.8f;
    c.Run(20);
    CHECK(c.lowest == 0.8125f); // 26 of 32, the step that 0.8 rounds to
    CHECK(c.governor.Scale() == 0.8125f);
    // Halves of the screen are the floor whatever the engine asks.
    c.limits.least = 0.1f;
    c.Run(20);
    CHECK(c.lowest == 0.5f);
}

// Frame times that move by 1.5 ms either way and a 60 ms hitch every 97
// frames: the picture stays in a narrow band and does not flicker.
void TestNoise()
{
    Console c;
    c.noiseMs = 1.5;
    c.hitchEvery = 97;
    c.Run(5);
    c.governor.ClearTotals();
    c.changes = 0;
    c.lowest = 1.0f;
    const uint64_t from = c.now;
    c.Run(120);
    const ScaleGovernor::Counts &counts = c.governor.Totals();
    const double minutes = double(c.now - from) / 60e6;
    std::printf("noise: %.1f changes a minute, scale %.3f to %.3f, mean %.3f, %llu tries (%llu lost)\n",
                double(c.changes) / minutes, counts.lowest, counts.highest, counts.scaleSum / double(counts.frames),
                (unsigned long long)counts.tries, (unsigned long long)counts.triesLost);
    CHECK(c.lowest >= 0.84f);
    CHECK(counts.highest <= 0.97f);
    CHECK(double(c.changes) / minutes <= 12.0);
}

// Held (the depth pass governor tries the other way): the size stays however
// slow the GPU gets, and follows once the hold ends.
void TestHold()
{
    Console c;
    c.Run(5);
    const float settled = c.governor.Scale();
    c.fixedMs = 20.0; // the try: no depth pass, more pixel work
    for (int i = 0; i < 40; ++i)
    {
        const double frame = (std::max)(c.cpuMs, c.GpuMs(settled));
        c.now += uint64_t(frame * 1000.0);
        c.governor.Hold(c.now);
        CHECK(c.governor.Scale() == settled);
    }
    c.Run(3); // the try was kept
    CHECK(c.governor.Scale() < settled);
    CHECK(c.GpuMs(c.governor.Scale()) <= ScaleGovernor::kSlowGoalMs - ScaleGovernor::kMarginMs + ScaleGovernor::kSlackMs + 0.01);
}

// The helpers that keep a smaller picture sharp (gxm_engine_options.h).
void TestSharpHelpers()
{
    using namespace gfx_gxm;
    // The mip bias: 8 * log2(scale), in sceGxm's eighths of a level.
    CHECK(SceneMipBiasSteps(1.0f) == 0);
    CHECK(SceneMipBiasSteps(1.5f) == 0);
    CHECK(SceneMipBiasSteps(0.97f) == 0);
    CHECK(SceneMipBiasSteps(0.9375f) == -1);
    CHECK(SceneMipBiasSteps(0.85f) == -2);
    CHECK(SceneMipBiasSteps(0.7f) == -4);
    CHECK(SceneMipBiasSteps(0.5f) == -8);
    CHECK(SceneMipBiasSteps(0.01f) == -16); // never more than two levels
    // Width and height together, or the same share of pixels from the
    // width alone.
    g_sceneWidthOnly = false;
    CHECK(SceneScaleX(0.8f) == 0.8f && SceneScaleY(0.8f) == 0.8f);
    g_sceneWidthOnly = true;
    CHECK(SceneScaleY(0.8f) == 1.0f);
    CHECK(SceneScaleX(0.8f) > 0.6399f && SceneScaleX(0.8f) < 0.6401f);
    CHECK(SceneScaleX(0.5f) == 0.5f); // no narrower than half the screen
    CHECK(SceneScaled(960, SceneScaleX(0.9375f)) == 843);
    CHECK(SceneScaled(544, SceneScaleY(0.9375f)) == 544);
    g_sceneWidthOnly = false;
}

// The engine's limits move while the game runs.
void TestLimitsMove()
{
    Console c;
    c.Run(5);
    c.limits.most = 0.8f;
    c.Frame();
    CHECK(c.governor.Scale() <= 0.8f);
    c.Run(5);
    CHECK(c.governor.Scale() == 0.8f); // the GPU fits at 0.8: the engine's own most, to the bit
    c.limits.least = 1.0f; // off
    c.Frame();
    CHECK(c.governor.Scale() == 0.8f);
    c.limits = {0.7f, 1.0f, 0.0f};
    c.Frame();
    CHECK(c.governor.Scale() == 1.0f); // a fresh start from the top
    // The clock standing still or going back changes nothing.
    const float before = c.governor.Scale();
    for (int i = 0; i < 100; ++i)
        c.governor.FrameEnded(c.now, 5000, c.limits);
    c.governor.FrameEnded(c.now - 1000, 5000, c.limits);
    CHECK(c.governor.Scale() == before);
}
} // namespace

int main()
{
    TestOff();
    TestBusyView();
    TestNoWaitGoal();
    TestFastGoal();
    TestFastGoalOutOfReach();
    TestViewEmpties();
    TestSlowCpu();
    TestLeast();
    TestNoise();
    TestHold();
    TestSharpHelpers();
    TestLimitsMove();
    if (g_failures)
    {
        std::fprintf(stderr, "%d scale governor checks failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vita scale governor: all checks passed\n");
    return EXIT_SUCCESS;
}
