// Host test of the figures behind the PS Vita Legacy back end's `frame time`
// log line (src/gfx_gxm/gxm_frame_time.h).
// Built and run by scripts/tests/test_vita_frame_time.py.

#include <gfx_gxm/gxm_frame_time.h>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using gfx_gxm::FrameTime;

namespace
{
int g_failures = 0;

void Expect(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

bool Near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

// A draw that is timed whatever the pick says.
void Timed(FrameTime &time, uint64_t micros, uint64_t innerBefore)
{
    ++time.draws;
    time.TimedDraw(micros, innerBefore);
}

// Nothing to say before a period began or without a frame in it.
void Empty()
{
    FrameTime time;
    EXPECT(time.Describe(1000, 0.5).empty());
    time.Restart(1000);
    EXPECT(time.since == 1000);
    EXPECT(time.Describe(5000, 0.5).empty()); // no frame shown
    time.Shown(100);
    EXPECT(!time.Describe(5000, 0.5).empty());
    EXPECT(time.Describe(500, 0.5).empty()); // a clock that went back
    const FrameTime::Means none = FrameTime{}.Mean(1000);
    EXPECT(Near(none.all, 0.0) && Near(none.rest, 0.0));
}

// Each call's time lands under its own name, and a frame's means add up.
void Sums()
{
    FrameTime time;
    time.Restart(1000000);
    for (int frame = 0; frame < 10; ++frame)
    {
        time.SceneStart(100);
        // Four draws of 250, every one timed here.
        for (int draw = 0; draw < 4; ++draw)
            Timed(time, 250, time.Inner());
        time.Other(400, time.Inner());
        time.SceneEnd(300);
        time.Wait(frame == 3 ? 5000 : 1000);
        time.Shown(frame == 7 ? 9000 : 2000);
    }
    EXPECT(time.frames == 10 && time.scenes == 10 && time.waits == 10 && time.others == 10);
    EXPECT(time.draws == 40 && time.timedDraws == 40 && time.slowDraws == 0);
    EXPECT(time.longestWait == 5000 && time.longestShow == 9000);
    // Ten frames in 200 ms.
    const FrameTime::Means m = time.Mean(1000000 + 200000);
    EXPECT(Near(m.all, 20.0));
    EXPECT(Near(m.timedOneIn, 1.0));
    EXPECT(Near(m.draws, 1.0));   // 4 x 250 us
    EXPECT(Near(m.others, 0.4));
    EXPECT(Near(m.scenes, 0.4));  // 100 + 300 us
    EXPECT(Near(m.waits, 1.4));   // 9 x 1000 + 5000 us over ten frames
    EXPECT(Near(m.show, 2.7));    // 9 x 2000 + 9000 us
    EXPECT(Near(m.rest, 20.0 - 1.0 - 0.4 - 0.4 - 1.4 - 2.7));
    EXPECT(Near(m.all, m.draws + m.others + m.scenes + m.waits + m.show + m.rest));

    const std::string line = time.Describe(1000000 + 200000, 0.75);
    std::printf("%s\n", line.c_str());
    EXPECT(line.find("frame time, last 10 frames: 20.0 ms a frame.") == 0);
    EXPECT(line.find("4 draws 1.00 ms (one in 1.0 timed)") != std::string::npos);
    EXPECT(line.find("1.0 clears and copies 0.40 ms") != std::string::npos);
    EXPECT(line.find("1.0 scenes' starts and ends 0.40 ms") != std::string::npos);
    EXPECT(line.find("1.00 waits for the GPU 1.40 ms (longest 5.0 ms)") != std::string::npos);
    EXPECT(line.find("showing the frame 2.70 ms (longest 9.0 ms)") != std::string::npos);
    EXPECT(line.find("The rest, 14.1 ms,") != std::string::npos);
    EXPECT(line.find("0 timed draws took over 2 ms (0.0 ms more in all, counted once)") != std::string::npos);
    EXPECT(line.find("A clock read costs 0.75 us") != std::string::npos);
    EXPECT(line.size() < 700); // the back end's log line holds 1024 with its prefix
}

// Scene work and waits inside a draw, a clear or a copy count once, under
// their own names.
void Nested()
{
    FrameTime time;
    time.Restart(1);
    uint64_t inner = time.Inner();
    time.SceneEnd(200);   // the draw changed the target: the open scene ends,
    time.SceneStart(100); // another starts,
    time.Wait(3000);      // and a renamed buffer waited for the GPU
    Timed(time, 3500, inner);
    EXPECT(time.timedDrawMicros == 200); // 3500 less 3300
    EXPECT(time.sceneMicros == 300 && time.waitMicros == 3000);

    inner = time.Inner();
    time.SceneEnd(50);
    time.Other(450, inner);
    EXPECT(time.otherMicros == 400);

    // A clock that steps between the two reads cannot take a sum below zero.
    inner = time.Inner();
    time.Wait(1000);
    Timed(time, 900, inner);
    EXPECT(time.timedDrawMicros == 200);
    inner = time.Inner();
    time.Wait(1000);
    time.Other(900, inner);
    EXPECT(time.otherMicros == 400);
}

// One slow timed draw must not be multiplied with the rest.
void SlowDraws()
{
    FrameTime time;
    time.Restart(1);
    // 1600 draws, 100 of them timed at 50 us each: 80 ms of draws.
    for (int i = 0; i < 100; ++i)
        Timed(time, 50, time.Inner());
    time.draws += 1500;
    // One more made a program on the spot: 1.2 s.
    Timed(time, 1200000, time.Inner());
    time.Shown(0);
    EXPECT(time.slowDraws == 1);
    EXPECT(time.slowDrawMicros == 1200000 - FrameTime::kSlowDrawMicros);
    EXPECT(time.timedDrawMicros == 100 * 50 + FrameTime::kSlowDrawMicros);
    const FrameTime::Means m = time.Mean(1 + 2000000);
    // (5000 + 2000) us x 1601 / 101, and the excess once.
    const double expected = (7000.0 * 1601.0 / 101.0 + 1198000.0) / 1000.0;
    EXPECT(Near(m.draws, expected));
    EXPECT(m.draws < 1400.0); // not 1.2 s sixteen times over
    const std::string line = time.Describe(1 + 2000000, 1.0);
    EXPECT(line.find("1 timed draws took over 2 ms (1198.0 ms more in all, counted once)") != std::string::npos);
}

// About one draw in sixteen is picked, and not every sixteenth: a frame that
// repeats its draws must not have the same ones timed each time.
void Picks()
{
    FrameTime time;
    const int kDraws = 160000;
    int picked = 0, last = -1;
    std::set<int> residues, gaps;
    for (int i = 0; i < kDraws; ++i)
        if (time.PickDraw())
        {
            ++picked;
            residues.insert(i % 16);
            if (last >= 0)
                gaps.insert(i - last);
            last = i;
        }
    std::printf("picks: %d of %d draws, %u residues mod 16, %u different gaps\n", picked, kDraws,
                unsigned(residues.size()), unsigned(gaps.size()));
    EXPECT(time.draws == uint64_t(kDraws));
    EXPECT(picked > kDraws / 16 * 9 / 10 && picked < kDraws / 16 * 11 / 10);
    EXPECT(residues.size() == 16);
    EXPECT(gaps.size() > 20);

    // A frame of 2000 draws, the same every time: over many frames every
    // position gets timed about as often as any other.
    FrameTime frames;
    const int kPerFrame = 2000, kFrames = 1800;
    static int timed[kPerFrame];
    for (int frame = 0; frame < kFrames; ++frame)
        for (int draw = 0; draw < kPerFrame; ++draw)
            if (frames.PickDraw())
                ++timed[draw];
    int least = kFrames, most = 0;
    for (const int count : timed)
    {
        least = std::min(least, count);
        most = std::max(most, count);
    }
    std::printf("a repeated frame: each draw timed %d to %d times in %d frames (%.1f expected)\n", least, most, kFrames,
                kFrames / 16.0);
    EXPECT(least > kFrames / 16 / 2 && most < kFrames / 16 * 2);
}

// The next period starts from nothing and goes on picking where it was.
void Restart()
{
    FrameTime time;
    time.Restart(10);
    for (int i = 0; i < 100; ++i)
        time.PickDraw();
    time.Wait(5);
    time.Shown(7);
    const uint32_t pick = time.drawPick;
    EXPECT(pick != 1);
    time.Restart(99);
    EXPECT(time.since == 99 && time.drawPick == pick);
    EXPECT(time.frames == 0 && time.draws == 0 && time.waits == 0 && time.waitMicros == 0 && time.showMicros == 0 &&
           time.longestShow == 0);
}
} // namespace

int main()
{
    Empty();
    Sums();
    Nested();
    SlowDraws();
    Picks();
    Restart();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gxm frame time: all checks passed\n");
    return 0;
}
