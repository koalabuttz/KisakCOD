// Host test for src/bots/bot_combat.h (scripts/tests/test_bot_combat.py).
//
// The fight is where a bot could most easily cheat, so these checks hold its
// hand to a human's: it does not move before its reaction onset, never turns
// faster than its top speed, lands a big flick off and closes in with
// corrections, trails a strafing player, clicks no faster than a person, and
// its view kicks when it fires, the way a player's client kicks theirs.

#include <bots/bot_combat.h>
#include <bots/bot_motor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace kb = kisak::bots;

static int gFailures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

constexpr int kFrameMs = 50; // a server frame (sv_fps 20)

// The view (pitch, yaw) from the origin toward `place`, as the brain's
// ViewToward computes it.
static void ViewTo(const float *place, float *view)
{
    view[0] = -std::atan2(place[2], std::hypot(place[0], place[1])) * (180.0f / 3.14159265358979f);
    view[1] = std::atan2(place[1], place[0]) * (180.0f / 3.14159265358979f);
}

struct Run
{
    int firstOnTargetMs = -1; // after the notice
    float errorAfterOneSecond = 0.0f;
    float fastestDegreesPerSecond = 0.0f;
    bool movedBeforeOnset = false;
    int onsetMs = 0;
};

// A still player `offDegrees` to the side at `distance`, noticed at time 0;
// the bot aims for two seconds.
static Run AimAtStill(uint32_t seed, float offDegrees, float distance)
{
    kb::SkillProfile skill;
    kb::Rng rng;
    rng.Seed(seed);
    kb::ViewMotorState view;
    kb::PrimeView(view, 0.0f, 0.0f);
    kb::AimState aim;
    kb::Engage(aim, 3, 0, 0, skill, rng);
    const float yaw = offDegrees * (3.14159265358979f / 180.0f);
    const float place[3] = {distance * std::cos(yaw), distance * std::sin(yaw), 0.0f};
    float target[2];
    ViewTo(place, target);
    const float kick[2] = {0.0f, 0.0f};
    Run run;
    run.onsetMs = aim.startMs;
    for (int now = kFrameMs; now <= 2000; now += kFrameMs)
    {
        const float before[2] = {view.angles[0], view.angles[1]};
        kb::StepAim(aim, view, target, kick, now, kFrameMs * 0.001f, skill, rng);
        const float turned = std::fmax(std::fabs(kb::AngleDelta(view.angles[0], before[0])),
                                       std::fabs(kb::AngleDelta(view.angles[1], before[1])));
        run.fastestDegreesPerSecond = std::fmax(run.fastestDegreesPerSecond, turned / (kFrameMs * 0.001f));
        if (now < aim.startMs && turned > 0.0f)
            run.movedBeforeOnset = true;
        if (run.firstOnTargetMs < 0 && kb::OnTarget(aim, view, kick, target, distance, now, skill))
            run.firstOnTargetMs = now;
        if (now == 1000)
            run.errorAfterOneSecond = kb::AimError(view.angles, target);
    }
    return run;
}

static float Percentile(std::vector<float> values, float share)
{
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(share * static_cast<float>(values.size() - 1));
    return values[index];
}

static void Reaction()
{
    std::printf("\n== reaction and the hand's speed ==\n");
    kb::SkillProfile skill;
    bool onsetFloor = true;
    bool still = true;
    bool speed = true;
    bool neverInstant = true;
    std::vector<float> firstOn;
    int missed = 0;
    for (uint32_t seed = 1; seed <= 1000; ++seed)
    {
        const float off = 5.0f + static_cast<float>(seed % 60);
        const Run run = AimAtStill(seed, off, 600.0f);
        onsetFloor = onsetFloor && run.onsetMs >= static_cast<int>(skill.onsetMinMs);
        still = still && !run.movedBeforeOnset;
        speed = speed && run.fastestDegreesPerSecond <= skill.aimMaxSpeed + 0.01f;
        neverInstant = neverInstant && (run.firstOnTargetMs < 0 || run.firstOnTargetMs >= 100);
        if (run.firstOnTargetMs < 0)
            ++missed;
        else
            firstOn.push_back(static_cast<float>(run.firstOnTargetMs));
    }
    Check(onsetFloor, "the hand never starts sooner than the reaction onset's floor after a notice");
    Check(still, "the hand does not move toward a player before its onset");
    Check(speed, "the hand never turns faster than its top speed");
    Check(neverInstant, "never on target sooner than 100 ms after the notice (5 to 64 degrees off)");
    Check(missed <= 20, "on a still player within 2 s in at least 98% of 1000 runs");
    const float median = firstOn.empty() ? 0.0f : Percentile(firstOn, 0.5f);
    std::printf("INFO: first on target after the notice: median %.0f ms, 90th percentile %.0f ms\n", median,
                firstOn.empty() ? 0.0f : Percentile(firstOn, 0.9f));
    Check(median >= 200.0f && median <= 700.0f, "the median time to first on target is 0.2 to 0.7 s");

    // A bigger flick takes longer.
    std::vector<float> small;
    std::vector<float> big;
    for (uint32_t seed = 1; seed <= 300; ++seed)
    {
        const Run a = AimAtStill(seed, 8.0f, 600.0f);
        const Run b = AimAtStill(seed + 5000, 70.0f, 600.0f);
        if (a.firstOnTargetMs >= 0)
            small.push_back(static_cast<float>(a.firstOnTargetMs));
        if (b.firstOnTargetMs >= 0)
            big.push_back(static_cast<float>(b.firstOnTargetMs));
    }
    Check(!small.empty() && !big.empty() && Percentile(big, 0.5f) > Percentile(small, 0.5f) + 50.0f,
          "a 70 degree flick takes longer than an 8 degree one");
}

static void Accuracy()
{
    std::printf("\n== flick, then settle ==\n");
    kb::SkillProfile skill;
    std::vector<float> errors;
    for (uint32_t seed = 1; seed <= 1000; ++seed)
        errors.push_back(AimAtStill(seed, 60.0f, 800.0f).errorAfterOneSecond);
    const float p90 = Percentile(errors, 0.9f);
    std::printf("INFO: error 1 s after noticing a player 60 degrees off: median %.2f, 90th percentile %.2f degrees\n",
                Percentile(errors, 0.5f), p90);
    Check(p90 < 1.5f, "a second after a 60 degree flick, the error is under 1.5 degrees in 90% of runs");
    Check(Percentile(errors, 0.5f) > 0.05f, "the hand is not perfect: the median error stays above 0.05 degrees");

    // The first movement of a big flick lands off by more than a small one.
    kb::Rng rng;
    rng.Seed(77);
    float bigLanding = 0.0f;
    float smallLanding = 0.0f;
    for (int i = 0; i < 2000; ++i)
    {
        for (int which = 0; which < 2; ++which)
        {
            kb::AimState aim;
            kb::ViewMotorState view;
            kb::PrimeView(view, 0.0f, 0.0f);
            aim.startMs = 0;
            aim.nextMoveMs = 0;
            const float target[2] = {0.0f, which ? 60.0f : 5.0f};
            const float kick[2] = {0.0f, 0.0f};
            kb::StepAim(aim, view, target, kick, 0, 0.0f, skill, rng);
            const float off = std::hypot(aim.offset[0], aim.offset[1]);
            (which ? bigLanding : smallLanding) += off;
        }
    }
    Check(bigLanding > 4.0f * smallLanding, "a 60 degree flick lands much further off than a 5 degree one");
}

static void SprayAndSwitch()
{
    std::printf("\n== long sprays, switching targets, the aim point ==\n");
    kb::SkillProfile skill;
    const float kick[2] = {0.0f, 0.0f};
    const float target[2] = {0.0f, 0.0f};

    // The spread of the hand's landings on a still player, with the trigger
    // kept going for `sprayMs`.
    auto spread = [&](int sprayMs) {
        kb::Rng rng;
        rng.Seed(11);
        float sum = 0.0f;
        for (int i = 0; i < 4000; ++i)
        {
            kb::AimState aim;
            kb::ViewMotorState view;
            kb::PrimeView(view, 0.0f, 0.0f);
            kb::StepAim(aim, view, target, kick, 0, 0.0f, skill, rng, sprayMs);
            sum += std::hypot(aim.offset[0], aim.offset[1]);
        }
        return sum / 4000.0f;
    };
    const float tap = spread(0);
    const float spray = spread(1500);
    const float longSpray = spread(10000);
    std::printf("INFO: mean landing error on a still player: %.2f degrees, %.2f after a 1.5 s spray\n", tap, spray);
    Check(spray > 2.5f * tap, "a 1.5 s spray lands far wider than a first shot");
    Check(std::fabs(longSpray - spread(static_cast<int>(1000.0f * skill.sprayErrorMax / skill.sprayErrorPerSecond)))
              < 0.01f,
          "and a spray stops getting worse at its cap");

    // A player crossing the view is harder to land on.
    {
        kb::Rng rng;
        rng.Seed(12);
        float still = 0.0f;
        float moving = 0.0f;
        for (int i = 0; i < 2000; ++i)
        {
            for (int which = 0; which < 2; ++which)
            {
                kb::AimState aim;
                kb::ViewMotorState view;
                kb::PrimeView(view, 0.0f, 0.0f);
                aim.nextMoveMs = 1000000;
                for (int now = 0; now <= 500; now += kFrameMs)
                {
                    const float there[2] = {0.0f, which ? 0.03f * static_cast<float>(now) : 0.0f};
                    if (now == 500)
                        aim.nextMoveMs = now;
                    kb::StepAim(aim, view, there, kick, now, kFrameMs * 0.001f, skill, rng);
                }
                (which ? moving : still) += std::hypot(aim.offset[0], aim.offset[1]);
            }
        }
        Check(moving > 1.5f * still, "a player crossing the view at 30 degrees a second is landed on less well");
    }

    // Straight from one fight into the next: a longer wait, a rougher flick.
    kb::Rng rng;
    rng.Seed(13);
    float fresh = 0.0f;
    float switched = 0.0f;
    bool floor = true;
    for (int i = 0; i < 2000; ++i)
    {
        kb::AimState a;
        kb::AimState b;
        kb::Engage(a, 1, 0, 0, skill, rng);
        kb::Engage(b, 2, 0, 0, skill, rng, true);
        fresh += static_cast<float>(a.startMs);
        switched += static_cast<float>(b.startMs);
        floor = floor && b.startMs >= static_cast<int>(skill.onsetMinMs + skill.switchOnsetMinMs);
    }
    std::printf("INFO: hand onset: %.0f ms for a first player, %.0f ms going straight to the next\n", fresh / 2000.0f,
                switched / 2000.0f);
    Check(switched > fresh + 0.8f * skill.switchOnsetMeanMs, "going straight to the next player takes longer");
    Check(floor, "and never less than both floors");

    // Each fight has its own recoil control and aim point, within bounds.
    bool bounded = true;
    float lowShare = 1.0f;
    float highShare = 0.0f;
    float lowAim = 0.0f;
    float highAim = 0.0f;
    for (int i = 0; i < 2000; ++i)
    {
        kb::AimState aim;
        kb::Engage(aim, 1, 0, 0, skill, rng);
        bounded = bounded && aim.recoilShare >= 0.1f && aim.recoilShare <= 0.8f
                  && std::fabs(aim.heightOffset) <= 2.0f * skill.aimHeightDeviation;
        lowShare = std::fmin(lowShare, aim.recoilShare);
        highShare = std::fmax(highShare, aim.recoilShare);
        lowAim = std::fmin(lowAim, aim.heightOffset);
        highAim = std::fmax(highAim, aim.heightOffset);
    }
    Check(bounded, "the recoil share stays in 0.1 to 0.8 and the aim point within two deviations of the chest");
    Check(highShare - lowShare > 0.3f && highAim - lowAim > 2.0f * skill.aimHeightDeviation,
          "and they differ from fight to fight");
}

static void Tracking()
{
    std::printf("\n== tracking a strafing player ==\n");
    kb::SkillProfile skill;
    // A player 500 units away runs across at 190 units a second, turning
    // back every second; the bot looks ten times a second.
    float stillOn = 0.0f;
    float movingOn = 0.0f;
    float movingError = 0.0f;
    int frames = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed)
    {
        for (int moving = 0; moving < 2; ++moving)
        {
            kb::Rng rng;
            rng.Seed(seed);
            kb::ViewMotorState view;
            kb::PrimeView(view, 0.0f, 0.0f);
            kb::AimState aim;
            kb::Engage(aim, 3, 0, 0, skill, rng);
            float seen[3] = {500.0f, 0.0f, 0.0f};
            float velocity[3] = {0.0f, 0.0f, 0.0f};
            int seenAt = 0;
            const float kick[2] = {0.0f, 0.0f};
            for (int now = kFrameMs; now <= 4000; now += kFrameMs)
            {
                const float t = static_cast<float>(now) * 0.001f;
                const float phase = std::fmod(t, 2.0f);
                const float side = phase < 1.0f ? phase : 2.0f - phase;
                const float truth[3] = {500.0f, moving ? -95.0f + 190.0f * side : 0.0f, 0.0f};
                if (now % 100 == 0)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        const float measured = (truth[k] - seen[k]) * 1000.0f / static_cast<float>(now - seenAt);
                        velocity[k] = 0.5f * velocity[k] + 0.5f * measured;
                        seen[k] = truth[k];
                    }
                    seenAt = now;
                }
                float lead[3];
                kb::LeadPlace(seen, velocity, now - seenAt, skill, lead);
                float aimAt[2];
                ViewTo(lead, aimAt);
                kb::StepAim(aim, view, aimAt, kick, now, kFrameMs * 0.001f, skill, rng);
                if (now < 1000)
                    continue;
                float real[2];
                ViewTo(truth, real);
                const bool on = kb::AimError(view.angles, real) <= kb::TargetHalfWidthDegrees(500.0f);
                if (moving)
                {
                    movingOn += on ? 1.0f : 0.0f;
                    movingError += kb::AimError(view.angles, real);
                    ++frames;
                }
                else
                {
                    stillOn += on ? 1.0f : 0.0f;
                }
            }
        }
    }
    const float movingShare = movingOn / static_cast<float>(frames);
    const float stillShare = stillOn / static_cast<float>(frames);
    std::printf("INFO: really on the player: still %.0f%%, strafing %.0f%% of frames (mean error %.2f degrees)\n",
                stillShare * 100.0f, movingShare * 100.0f, movingError / static_cast<float>(frames));
    Check(movingShare < stillShare, "a strafing player is harder to stay on than a still one");
    Check(movingShare > 0.12f, "the bot still tracks a strafing player some of the time");

    // It leads at most a sense tick's worth of what it saw.
    const float seen[3] = {0.0f, 0.0f, 0.0f};
    const float velocity[3] = {200.0f, 0.0f, 0.0f};
    float lead[3];
    kb::LeadPlace(seen, velocity, 1000, skill, lead);
    Check(lead[0] <= 200.0f * 0.15f * skill.leadFraction + 0.01f, "it leads no more than 0.15 s of the motion it saw");
}

static void Trigger()
{
    std::printf("\n== the trigger ==\n");
    kb::SkillProfile skill;
    kb::Rng rng;
    rng.Seed(5);
    kb::TriggerState trigger;
    int presses = 0;
    int lastPress = -100000;
    int closest = 100000;
    bool releasedBetween = true;
    bool held = false;
    for (int now = 0; now <= 20000; now += kFrameMs)
    {
        const bool press = kb::StepTrigger(trigger, true, false, 500.0f, now, skill, rng);
        if (press)
        {
            releasedBetween = releasedBetween && !held;
            ++presses;
            closest = std::min(closest, now - lastPress);
            lastPress = now;
        }
        held = press;
    }
    std::printf("INFO: semi-automatic: %d clicks in 20 s, closest %d ms apart\n", presses, closest);
    Check(closest >= static_cast<int>(skill.tapMinMs), "a semi-automatic is clicked no faster than the floor");
    Check(releasedBetween, "every click is released before the next");
    Check(presses >= 60 && presses <= 160, "a semi-automatic is clicked 3 to 8 times a second");

    kb::TriggerState automatic;
    bool holds = true;
    for (int now = 0; now <= 2000; now += kFrameMs)
        holds = holds && kb::StepTrigger(automatic, true, true, 300.0f, now, skill, rng);
    Check(holds, "an automatic is held down close up");
    Check(!kb::StepTrigger(automatic, false, true, 300.0f, 2050, skill, rng), "and let go off target");

    kb::TriggerState burst;
    int down = 0;
    int ups = 0;
    bool was = false;
    for (int now = 0; now <= 10000; now += kFrameMs)
    {
        const bool press = kb::StepTrigger(burst, true, true, 2000.0f, now, skill, rng);
        down += press ? 1 : 0;
        ups += (was && !press) ? 1 : 0;
        was = press;
    }
    Check(ups >= 8 && down > 100 && down < 180, "an automatic fires in bursts at range");
}

static void Recoil()
{
    std::printf("\n== the view kick ==\n");
    kb::KickParams params;
    params.pitchMin = 40.0f;
    params.pitchMax = 60.0f;
    params.yawMin = -20.0f;
    params.yawMax = 20.0f;
    params.centerSpeed = 1500.0f;

    kb::ViewKick kick;
    kb::KickOnShot(kick, params, 1.0f, 0.5f, 0.5f);
    kb::StepKick(kick, params.centerSpeed, 50);
    Check(kick.angles[0] < 0.0f, "a shot kicks the view up");
    float worst = 0.0f;
    for (int ms = 0; ms < 3000; ms += 50)
    {
        kb::StepKick(kick, params.centerSpeed, 50);
        worst = std::fmax(worst, std::fabs(kick.angles[0]));
    }
    Check(kick.angles[0] == 0.0f && kick.velocity[0] == 0.0f, "and it comes back to the centre");

    // Fire every 80 ms for 3 s with a big kick: never past 10 degrees.
    kb::Rng rng;
    rng.Seed(9);
    kb::ViewKick sustained;
    params.pitchMin = 400.0f;
    params.pitchMax = 500.0f;
    for (int ms = 0; ms < 3000; ms += 80)
    {
        kb::KickOnShot(sustained, params, 1.0f, rng.Unit(), rng.Unit());
        kb::StepKick(sustained, params.centerSpeed, 80);
        for (float angle : sustained.angles)
            worst = std::fmax(worst, std::fabs(angle));
    }
    Check(worst <= 10.0f, "the kick never passes 10 degrees, as on a player's client");
    Check(std::fabs(sustained.angles[0]) > 1.0f, "holding the trigger climbs");

    // The reduced kick share applies.
    kb::ViewKick reduced;
    kb::KickOnShot(reduced, params, 0.5f, 0.0f, 0.5f);
    Check(std::fabs(-reduced.velocity[0] - params.pitchMin * 0.5f) < 0.01f, "the reduced-kick share scales the kick");

    // The hand pulls back part of the kick, late.
    kb::SkillProfile skill;
    kb::AimState aim;
    kb::ViewMotorState view;
    kb::PrimeView(view, 0.0f, 0.0f);
    aim.startMs = 0;
    aim.nextMoveMs = 1000000; // no new aimed movement: only the pull-back
    const float target[2] = {0.0f, 0.0f};
    const float kickNow[2] = {-4.0f, 0.0f};
    kb::StepAim(aim, view, target, kickNow, 50, 0.05f, skill, rng);
    const float early = view.angles[0];
    for (int now = 100; now <= 1500; now += 50)
        kb::StepAim(aim, view, target, kickNow, now, 0.05f, skill, rng);
    Check(early < 1.0f, "the hand barely answers the kick in the first frame");
    Check(view.angles[0] > 1.0f && view.angles[0] < 4.0f * skill.recoilCompensation + 0.3f,
          "and in time pulls down only part of it");
}

static void Strafing()
{
    std::printf("\n== moving under fire ==\n");
    kb::SkillProfile skill;
    kb::Rng rng;
    rng.Seed(11);
    kb::StrafeState strafe;
    int left = 0;
    int right = 0;
    int walk = 0;
    int changes = 0;
    int last = 0;
    for (int now = 0; now <= 60000; now += kFrameMs)
    {
        const int side = kb::StepStrafe(strafe, now, skill, rng);
        left += side < 0;
        right += side > 0;
        walk += side == 0;
        changes += side != last;
        last = side;
    }
    Check(left > 0 && right > 0 && walk > 0, "it strafes both ways and walks on");
    Check(changes > 40 && changes < 160, "a strafe lasts 0.4 to 1.2 s");
}

int main()
{
    Reaction();
    Accuracy();
    SprayAndSwitch();
    Tracking();
    Trigger();
    Recoil();
    Strafing();
    std::printf("\n%s\n", gFailures ? "SOME CHECKS FAILED" : "all bot combat checks passed");
    return gFailures ? 1 : 0;
}
