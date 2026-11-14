// Host test for src/bots/bot_motor.h (scripts/tests/test_bot_motor.py).
//
// The motor is what keeps a bot's turning human: these checks hold it to
// that. Commands round-trip to the view Pmove computes from them, turns take
// the short way round, never pass the top speed or overshoot, settle on the
// target, survive any frame time, and the random source replays from a seed.

#include <bots/bot_motor.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kb = kisak::bots;

static int gFailures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

static bool Near(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

// Pmove's view from a command (PM_UpdateViewAngles, bg_pmove.cpp), written
// out here independently of the header so a shared mistake cannot hide.
static float PmoveView(int cmdAngle, float delta)
{
    double v = delta + static_cast<double>(cmdAngle) * (360.0 / 65536.0);
    double turns = v / 360.0;
    return static_cast<float>((turns - std::floor(turns + 0.5)) * 360.0);
}

static void CommandAngles()
{
    std::printf("\n== commands give the view Pmove computes ==\n");
    const float half = 0.5f * 360.0f / 65536.0f + 1e-4f;
    bool roundTrip = true;
    bool inRange = true;
    kb::Rng rng;
    rng.Seed(1234);
    for (int i = 0; i < 20000; ++i)
    {
        float view = rng.Range(-720.0f, 720.0f);
        float delta = rng.Range(-540.0f, 540.0f);
        int32_t cmd = kb::CmdAngleFor(view, delta);
        if (cmd < -32768 || cmd > 32767)
            inRange = false;
        float got = PmoveView(cmd, delta);
        if (std::fabs(kb::AngleDelta(got, view)) > half)
            roundTrip = false;
        if (std::fabs(kb::AngleDelta(kb::ViewForCmdAngle(cmd, delta), got)) > 1e-3f)
            roundTrip = false;
    }
    Check(roundTrip, "every view and delta_angles pair comes back within half a short");
    Check(inRange, "command angles stay 16-bit, as a client sends them");
    Check(kb::CmdAngleFor(180.0f, 0.0f) == -32768 || kb::CmdAngleFor(180.0f, 0.0f) == 32767,
          "looking straight back is a 16-bit angle");
}

static void AngleHelpers()
{
    std::printf("\n== angle helpers ==\n");
    Check(Near(kb::AngleNormalize180(190.0f), -170.0f, 1e-4f), "190 wraps to -170");
    Check(Near(kb::AngleNormalize180(-190.0f), 170.0f, 1e-4f), "-190 wraps to 170");
    Check(Near(kb::AngleNormalize180(720.0f + 45.0f), 45.0f, 1e-3f), "two turns and 45 is 45");
    Check(Near(kb::AngleDelta(-170.0f, 170.0f), 20.0f, 1e-4f), "170 to -170 is +20, the short way");
    Check(Near(kb::AngleDelta(170.0f, -170.0f), -20.0f, 1e-4f), "-170 to 170 is -20, the short way");
}

struct TurnResult
{
    float peakSpeed = 0.0f;
    float overshoot = 0.0f;
    float settleSeconds = -1.0f;
    bool wrongWay = false;
};

// Turns from (0, startYaw) to (pitch, yaw) at a 20 Hz server frame.
static TurnResult Turn(float startYaw, float pitch, float yaw, const kb::ViewMotorParams &params, float frame = 0.05f)
{
    kb::ViewMotorState state;
    kb::PrimeView(state, 0.0f, startYaw);
    TurnResult result;
    const float total = kb::AngleDelta(yaw, startYaw);
    float travelled = 0.0f;
    float lastYaw = state.angles[1];
    for (int i = 0; i < 400; ++i)
    {
        kb::StepView(state, pitch, yaw, frame, params);
        float step = kb::AngleDelta(state.angles[1], lastYaw);
        lastYaw = state.angles[1];
        travelled += step;
        float speed = std::fabs(step) / frame;
        if (speed > result.peakSpeed)
            result.peakSpeed = speed;
        if (total != 0.0f && step * total < -1e-3f)
            result.wrongWay = true;
        float past = (total >= 0.0f) ? travelled - total : total - travelled;
        if (past > result.overshoot)
            result.overshoot = past;
        if (result.settleSeconds < 0.0f
            && std::fabs(kb::AngleDelta(yaw, state.angles[1])) < 0.5f
            && std::fabs(state.angles[0] - kb::ClampPitch(pitch)) < 0.5f)
            result.settleSeconds = (i + 1) * frame;
    }
    return result;
}

static void Motor()
{
    std::printf("\n== the view motor turns like a hand ==\n");
    kb::ViewMotorParams idle;
    idle.responsiveness = 5.0f;
    idle.maxSpeed = 120.0f;

    TurnResult t = Turn(0.0f, 0.0f, 90.0f, idle);
    Check(t.peakSpeed <= 120.0f + 0.5f, "a 90 degree look never passes the 120 deg/s top speed");
    Check(t.overshoot < 0.25f, "and does not overshoot");
    Check(!t.wrongWay, "and never turns back");
    Check(t.settleSeconds > 0.6f && t.settleSeconds < 3.0f, "and settles in a human time (0.6-3 s)");

    TurnResult wrap = Turn(170.0f, 0.0f, -170.0f, idle);
    Check(wrap.settleSeconds > 0.0f && wrap.settleSeconds < t.settleSeconds,
          "170 to -170 takes the 20 degree way round (quicker than a 90 degree look)");
    Check(!wrap.wrongWay, "without starting the long way");

    kb::ViewMotorParams flick;
    flick.responsiveness = 20.0f;
    flick.maxSpeed = 600.0f;
    TurnResult f = Turn(0.0f, 0.0f, 120.0f, flick);
    Check(f.settleSeconds > 0.0f && f.settleSeconds < t.settleSeconds, "a flick settles faster than an idle look");
    Check(f.peakSpeed <= 600.5f, "a flick keeps its own top speed");

    TurnResult up = Turn(0.0f, -120.0f, 0.0f, idle);
    Check(up.settleSeconds > 0.0f, "asking to look past straight up settles at the limit");
    kb::ViewMotorState pitchState;
    kb::PrimeView(pitchState, 0.0f, 0.0f);
    for (int i = 0; i < 100; ++i)
        kb::StepView(pitchState, -120.0f, 0.0f, 0.05f, idle);
    Check(Near(pitchState.angles[0], -kb::kMaxPitchUp, 0.01f), "pitch stops at the player's limit");

    // Odd frame times: a hitch, a tiny step, nothing, a negative clock.
    kb::ViewMotorState odd;
    kb::PrimeView(odd, 0.0f, 0.0f);
    kb::StepView(odd, 0.0f, 90.0f, 3.0f, idle);
    Check(std::isfinite(odd.angles[1]) && odd.angles[1] <= 90.0f + 0.25f, "a 3 second hitch is clamped, not a jump");
    float before = odd.angles[1];
    kb::StepView(odd, 0.0f, 90.0f, 0.0f, idle);
    kb::StepView(odd, 0.0f, 90.0f, -1.0f, idle);
    Check(odd.angles[1] == before, "zero and negative frame times change nothing");
    kb::StepView(odd, 0.0f, 90.0f, 0.0001f, idle);
    Check(std::isfinite(odd.angles[1]), "a tiny step stays finite");

    // The same turn at 20 and at 60 command frames a second ends in the same place.
    kb::ViewMotorState a, b;
    kb::PrimeView(a, 0.0f, 0.0f);
    kb::PrimeView(b, 0.0f, 0.0f);
    for (int i = 0; i < 20; ++i)
        kb::StepView(a, 5.0f, 60.0f, 0.05f, idle);
    for (int i = 0; i < 60; ++i)
        kb::StepView(b, 5.0f, 60.0f, 1.0f / 60.0f, idle);
    Check(Near(a.angles[1], b.angles[1], 0.5f) && Near(a.angles[0], b.angles[0], 0.5f),
          "the turn does not depend on the frame rate");
}

static void Random()
{
    std::printf("\n== each bot's random source replays from its seed ==\n");
    kb::Rng a, b;
    a.Seed(42);
    b.Seed(42);
    bool same = true;
    bool inUnit = true;
    for (int i = 0; i < 10000; ++i)
    {
        float x = a.Unit();
        if (x != b.Unit())
            same = false;
        if (!(x >= 0.0f && x < 1.0f))
            inUnit = false;
    }
    Check(same, "the same seed gives the same choices");
    Check(inUnit, "Unit() stays in [0, 1)");
    kb::Rng zero;
    zero.Seed(0);
    Check(zero.Next() != 0, "a zero seed still produces numbers");

    std::printf("\n== an idle bot looks around ==\n");
    kb::Rng rng;
    rng.Seed(7);
    bool ok = true;
    for (int i = 0; i < 1000; ++i)
    {
        kb::IdleLook look = kb::NextIdleLook(rng, 10.0f);
        float turn = std::fabs(kb::AngleDelta(look.yaw, 10.0f));
        if (turn < 30.0f - 1e-3f || turn > 150.0f + 1e-3f)
            ok = false;
        if (look.pitch < -8.0f || look.pitch > 8.0f)
            ok = false;
        if (look.holdSeconds < 1.5f || look.holdSeconds > 4.0f)
            ok = false;
    }
    Check(ok, "each new look turns 30-150 degrees, stays near level and holds 1.5-4 s");
}

int main()
{
    AngleHelpers();
    CommandAngles();
    Motor();
    Random();
    if (gFailures)
    {
        std::printf("\n%d bot motor check(s) failed.\n", gFailures);
        return 1;
    }
    std::printf("\nAll bot motor checks passed.\n");
    return 0;
}
