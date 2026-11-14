// Host checks for the GXM back end's depth pass governor
// (src/gfx_gxm/gxm_prepass_governor.h), run by
// scripts/tests/test_vita_prepass_governor.py.

#include <gfx_gxm/gxm_prepass_governor.h>

#include <cstdio>
#include <functional>

using gfx_gxm::PrepassGovernor;

namespace
{
int g_failures;

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s (line %d)\n", #condition, __LINE__);                                   \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

// Runs `seconds` of frames whose length depends on the way the governor is
// in; returns the share of frames drawn with the pass and counts changes.
struct Run
{
    PrepassGovernor governor;
    uint64_t now = 1'000'000;
    int changes = 0;
    int probes = 0;

    double For(double seconds, const std::function<double(bool)> &frameMs)
    {
        const uint64_t end = now + uint64_t(seconds * 1e6);
        uint64_t withPass = 0, frames = 0;
        while (now < end)
        {
            const bool pass = governor.Pass();
            now += uint64_t(frameMs(pass) * 1000.0);
            ++frames;
            withPass += pass ? 1 : 0;
            const PrepassGovernor::Change change = governor.FrameEnded(now);
            if (change.changed)
                ++changes;
            if (change.probe)
                ++probes;
        }
        return double(withPass) / double(frames);
    }

    // Runs on until a try that is still open has been timed.
    void FinishTry(const std::function<double(bool)> &frameMs)
    {
        for (int i = 0; i < 200 && governor.Probing(); ++i)
            For(0.001, frameMs);
    }
};
} // namespace

int main()
{
    // Run AM's busy view at scale 0.7: 40.3 ms with the pass, 33.3 without.
    {
        Run run;
        run.governor.Reset(true);
        const double share = run.For(60.0, [](bool pass) { return pass ? 40.3 : 33.3; });
        CHECK(!run.governor.Pass());
        CHECK(share < 0.2); // mostly without, apart from the tries
        CHECK(run.probes >= 3); // it keeps checking the other way
    }
    // Run AM facing the other way: 17.7 ms with the pass, 23.8 without.
    {
        Run run;
        run.governor.Reset(true);
        const double share = run.For(60.0, [](bool pass) { return pass ? 17.7 : 23.8; });
        CHECK(run.governor.Pass());
        CHECK(share > 0.8);
    }
    // Close times (scale 1: 39.3 against 40.7) stay where they are: no
    // change for less than the margin, beyond the tries.
    {
        Run run;
        run.governor.Reset(true);
        const auto close = [](bool pass) { return pass ? 39.3 : 40.7; };
        run.For(60.0, close);
        run.FinishTry(close);
        CHECK(run.governor.Pass());
        CHECK(run.probes >= 3);
        CHECK(run.changes == 2 * run.probes); // each try goes and comes back
    }
    // Held for five minutes the tries back off, so the slower way's share
    // of the frames is small (run AO: tries every 8 s cost 1 to 3 ms).
    {
        Run run;
        run.governor.Reset(true);
        const double share = run.For(300.0, [](bool pass) { return pass ? 40.3 : 33.3; });
        CHECK(share < 0.03);
        CHECK(run.probes <= 8);
    }
    {
        Run run;
        run.governor.Reset(true);
        const double share = run.For(300.0, [](bool pass) { return pass ? 17.7 : 23.8; });
        CHECK(share > 0.97);
    }
    // One hitch in a window does not move the governor (run AO: a 36.8 ms
    // window mean dropped the pass on the light view).
    {
        Run run;
        run.governor.Reset(true);
        int frame = 0;
        const auto hitchy = [&frame](bool pass) {
            ++frame;
            return frame == 2000 ? 600.0 : pass ? 17.7 : 23.8;
        };
        const double share = run.For(60.0, hitchy);
        CHECK(frame > 2000);
        run.FinishTry(hitchy);
        CHECK(run.governor.Pass());
        CHECK(run.changes == 2 * run.probes);
        CHECK(share > 0.9);
    }
    // The view changes: the governor follows within a few seconds.
    {
        Run run;
        run.governor.Reset(true);
        run.For(30.0, [](bool pass) { return pass ? 17.7 : 23.8; });
        CHECK(run.governor.Pass());
        run.For(15.0, [](bool pass) { return pass ? 40.3 : 33.3; });
        CHECK(!run.governor.Pass());
        run.For(15.0, [](bool pass) { return pass ? 17.7 : 23.8; });
        CHECK(run.governor.Pass());
    }
    // A change leaves out the frames that follow it, and no time is taken
    // before a whole window: the first window decides nothing but a try.
    {
        PrepassGovernor governor;
        governor.Reset(true);
        uint64_t now = 5'000'000;
        int changed = 0;
        for (uint32_t i = 0; i < PrepassGovernor::kWindowFrames; ++i)
            changed += governor.FrameEnded(now += 20'000).changed ? 1 : 0;
        CHECK(changed == 0);
        const PrepassGovernor::Change change = governor.FrameEnded(now += 20'000);
        CHECK(change.changed && change.probe && !change.pass);
        CHECK(change.passMs > 19.9 && change.passMs < 20.1);
        for (uint32_t i = 0; i < PrepassGovernor::kSettleFrames; ++i)
            CHECK(!governor.FrameEnded(now += 50'000).changed);
    }
    // A clock that does not move takes no time and divides by nothing.
    {
        PrepassGovernor governor;
        governor.Reset(false);
        for (int i = 0; i < 100; ++i)
            CHECK(!governor.FrameEnded(7).changed);
        CHECK(!governor.Pass());
    }

    if (g_failures)
    {
        printf("%d prepass governor check(s) failed\n", g_failures);
        return 1;
    }
    printf("prepass governor: all checks passed\n");
    return 0;
}
