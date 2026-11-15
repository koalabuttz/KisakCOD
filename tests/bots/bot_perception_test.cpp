// Host test for src/bots/bot_perception.h (scripts/tests/test_bot_perception.py).
//
// Perception is what keeps a bot fair: these checks hold it to what a player
// in its place could know. Nobody off the screen, out of sight or too far is
// noticed; nobody is noticed faster than the human floor, however the samples
// fall; distance, the screen's edge, stillness, crouching and cover all slow
// noticing; a lost player is remembered where last seen and then forgotten;
// sounds are heard only in range and only roughly.

#include <bots/bot_motor.h>
#include <bots/bot_perception.h>

#include <cmath>
#include <cstdio>

namespace kb = kisak::bots;

static int gFailures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

static kb::SightSample Plain(float distance)
{
    kb::SightSample sample;
    sample.distance = distance;
    sample.exposure = 1.0f;
    sample.speed = 190.0f;
    return sample;
}

// Sense ticks of 100 ms until noticed; returns the time in view, or -1.
static int MsToNotice(const kb::SightSample &sample, int limitMs = 20000)
{
    kb::PerceptionParams params;
    kb::Perception perception;
    perception.Reset();
    const float place[3] = {0.0f, 0.0f, 0.0f};
    for (int now = 100; now <= limitMs; now += 100)
        if (perception.See(1, sample, place, now, 0.1f, params))
            return perception.contacts[1].noticedAfterMs;
    return -1;
}

static void NoticeRates()
{
    std::printf("\n== what can be noticed, and how fast ==\n");
    kb::PerceptionParams params;
    const int close = MsToNotice(Plain(300.0f));
    Check(close >= 200 && close <= 400, "a running player close by, mid-screen, is noticed in 0.2 to 0.4 s");

    kb::SightSample offScreen = Plain(300.0f);
    offScreen.yawOff = 45.0f;
    Check(kb::NoticeRate(offScreen, params) == 0.0f && MsToNotice(offScreen) < 0, "nobody off the screen across is noticed");
    kb::SightSample above = Plain(300.0f);
    above.pitchOff = -30.0f;
    Check(kb::NoticeRate(above, params) == 0.0f, "nobody off the screen up or down is noticed");
    kb::SightSample hidden = Plain(300.0f);
    hidden.exposure = 0.0f;
    Check(kb::NoticeRate(hidden, params) == 0.0f, "nobody fully behind cover is noticed");
    Check(kb::NoticeRate(Plain(params.maxRange + 1.0f), params) == 0.0f, "nobody past the sight range is noticed");

    const int far = MsToNotice(Plain(3000.0f));
    Check(far > 3 * close, "a player 3000 units away takes several times longer than one close by");
    kb::SightSample edge = Plain(300.0f);
    edge.yawOff = 38.0f;
    Check(MsToNotice(edge) > 2 * close, "a player at the screen's edge takes longer than one mid-screen");
    kb::SightSample still = Plain(300.0f);
    still.speed = 0.0f;
    kb::SightSample prone = still;
    prone.stance = kb::Stance::Prone;
    Check(MsToNotice(still) > close && MsToNotice(prone) > MsToNotice(still),
          "a still player takes longer, and a still prone one longer again");
    kb::SightSample sliver = Plain(300.0f);
    sliver.exposure = 1.0f / 3.0f;
    Check(MsToNotice(sliver) > close, "a player mostly behind cover takes longer");
}

static void HumanFloor()
{
    std::printf("\n== nobody is noticed faster than a human could ==\n");
    kb::PerceptionParams params;
    kb::Rng rng;
    rng.Seed(77);
    bool floorHeld = true;
    bool rateCapped = true;
    int notices = 0;
    for (int run = 0; run < 2000; ++run)
    {
        kb::Perception perception;
        perception.Reset();
        const float place[3] = {0.0f, 0.0f, 0.0f};
        int now = 0;
        for (int tick = 0; tick < 60; ++tick)
        {
            const int step = 30 + static_cast<int>(rng.Range(0.0f, 300.0f));
            now += step;
            const float seconds = static_cast<float>(step) * 0.001f;
            kb::SightSample sample;
            // Mostly on screen and in range, some samples off it or too far.
            sample.distance = rng.Range(50.0f, 2500.0f) * (rng.Unit() < 0.1f ? 4.0f : 1.0f);
            sample.yawOff = rng.Range(-45.0f, 45.0f);
            sample.pitchOff = rng.Range(-27.0f, 27.0f);
            sample.exposure = static_cast<float>(static_cast<int>(rng.Range(0.0f, 4.0f))) / 3.0f;
            sample.speed = rng.Range(0.0f, 300.0f);
            sample.stance = static_cast<kb::Stance>(static_cast<int>(rng.Range(0.0f, 3.0f)));
            if (kb::NoticeRate(sample, params) > 1.0f / params.minNoticeSeconds + 1e-4f)
                rateCapped = false;
            bool noticed = false;
            if (rng.Unit() < 0.3f)
                perception.Unseen(3, now, seconds, params);
            else
                noticed = perception.See(3, sample, place, now, seconds, params);
            if (noticed)
            {
                ++notices;
                if (perception.contacts[3].noticedAfterMs < static_cast<int>(params.minNoticeSeconds * 1000.0f))
                    floorHeld = false;
                perception.Forget(3);
            }
        }
    }
    Check(rateCapped, "the noticing rate never passes the human floor");

    kb::Perception perception;
    perception.Reset();
    const float place[3] = {0.0f, 0.0f, 0.0f};
    const bool atOnce = perception.See(1, Plain(300.0f), place, 5000, 5.0f, params);
    Check(!atOnce && perception.contacts[1].inViewMs <= 250,
          "a long gap between ticks counts as one short look, not a long one");
    Check(floorHeld && notices > 100, "every notice came after at least minNoticeSeconds in view (2000 random runs)");
}

static void Memory()
{
    std::printf("\n== a lost player is remembered, then forgotten ==\n");
    kb::PerceptionParams params;
    kb::Perception perception;
    perception.Reset();
    const float eye[3] = {0.0f, 0.0f, 60.0f};
    const float seen[3] = {500.0f, 0.0f, 40.0f};
    int now = 0;
    while (!perception.See(5, Plain(500.0f), seen, now += 100, 0.1f, params))
    {
    }
    kb::Focus focus = perception.Choose(eye, now, params);
    Check(focus.kind == kb::FocusKind::Seen && focus.entity == 5, "a noticed player in view is the focus");

    const int lostAt = now;
    perception.Unseen(5, now += 100, 0.1f, params);
    focus = perception.Choose(eye, now, params);
    Check(focus.kind == kb::FocusKind::Remembered && focus.position[0] == 500.0f,
          "out of sight, the player is remembered where last seen");
    const float early = perception.Confidence(5, lostAt + 100, params);
    const float later = perception.Confidence(5, lostAt + 3000, params);
    Check(early > 0.9f && std::fabs(later - 0.5f) < 0.05f, "confidence halves every memorySeconds");
    for (; now < lostAt + 11000; now += 100)
        perception.Unseen(5, now, 0.1f, params);
    Check(!perception.contacts[5].active && perception.Choose(eye, now, params).kind == kb::FocusKind::None,
          "the player is forgotten after forgetSeconds");

    // A half-filled meter drains when the player goes out of sight.
    kb::SightSample farAway = Plain(3000.0f);
    perception.See(6, farAway, seen, now += 100, 0.1f, params);
    const float some = perception.contacts[6].awareness;
    for (int i = 0; i < 20; ++i)
        perception.Unseen(6, now += 100, 0.1f, params);
    Check(some > 0.0f && perception.contacts[6].awareness == 0.0f && perception.contacts[6].inViewMs == 0,
          "a half-noticed player out of sight drains from the meter");
}

static void Hearing()
{
    std::printf("\n== sounds are heard in range, roughly ==\n");
    kb::PerceptionParams params;
    Check(kb::FootstepRange(250.0f, kb::Stance::Standing, false, true, params) == 0.0f,
          "Dead Silence makes footsteps silent");
    Check(kb::FootstepRange(150.0f, kb::Stance::Crouched, false, false, params) == 0.0f &&
              kb::FootstepRange(40.0f, kb::Stance::Prone, false, false, params) == 0.0f,
          "crouched and prone players are silent");
    Check(kb::FootstepRange(20.0f, kb::Stance::Standing, false, false, params) == 0.0f,
          "a player standing still is silent");
    Check(kb::FootstepRange(250.0f, kb::Stance::Standing, true, false, params) >
              kb::FootstepRange(190.0f, kb::Stance::Standing, false, false, params) &&
              kb::FootstepRange(190.0f, kb::Stance::Standing, false, false, params) >
                  kb::FootstepRange(100.0f, kb::Stance::Standing, false, false, params),
          "sprinting carries further than running, and running further than walking");

    const float listener[3] = {0.0f, 0.0f, 0.0f};
    const float source[3] = {1000.0f, 0.0f, 0.0f};
    kb::Rng rng;
    rng.Seed(5);
    bool bounded = true;
    bool varies = false;
    float first[3];
    for (int i = 0; i < 1000; ++i)
    {
        float place[3];
        kb::HeardPlace(listener, source, params.hearingError, rng.Unit(), rng.Unit(), place);
        const float off = std::hypot(place[0] - source[0], place[1] - source[1]);
        if (off > 1000.0f * params.hearingError + 1e-3f)
            bounded = false;
        if (i == 0)
            first[0] = place[0], first[1] = place[1], first[2] = place[2];
        else if (std::fabs(place[0] - first[0]) > 1.0f || std::fabs(place[1] - first[1]) > 1.0f)
            varies = true;
    }
    Check(bounded && varies, "a heard place is off by up to hearingError of the distance, and not always the same way");

    kb::Perception perception;
    perception.Reset();
    const float eye[3] = {0.0f, 0.0f, 60.0f};
    perception.Hear(9, source, 1000);
    Check(perception.Choose(eye, 1500, params).kind == kb::FocusKind::Heard, "a fresh sound turns the bot's head");
    Check(perception.Choose(eye, 1000 + static_cast<int>(params.heardSeconds * 1000.0f) + 100, params).kind ==
              kb::FocusKind::None,
          "an old sound does not");
    Check(!perception.contacts[9].noticed, "hearing a player does not notice them");
}

static void Priorities()
{
    std::printf("\n== what the bot looks at ==\n");
    kb::PerceptionParams params;
    kb::Perception perception;
    perception.Reset();
    const float eye[3] = {0.0f, 0.0f, 60.0f};
    const float nearPlace[3] = {400.0f, 0.0f, 40.0f};
    const float farPlace[3] = {1500.0f, 0.0f, 40.0f};
    const float heard[3] = {0.0f, 300.0f, 40.0f};
    int now = 0;
    while (!perception.See(2, Plain(1500.0f), farPlace, now += 100, 0.1f, params))
    {
    }
    while (!perception.See(3, Plain(400.0f), nearPlace, now += 100, 0.1f, params))
        perception.See(2, Plain(1500.0f), farPlace, now, 0.1f, params);
    perception.See(2, Plain(1500.0f), farPlace, now, 0.1f, params);
    perception.Hear(4, heard, now);
    Check(perception.Choose(eye, now, params).entity == 3, "the nearer of two noticed players in view comes first");
    perception.Unseen(3, now + 100, 0.1f, params);
    perception.Unseen(2, now + 100, 0.1f, params);
    const kb::Focus focus = perception.Choose(eye, now + 100, params);
    Check(focus.kind == kb::FocusKind::Remembered, "a fresh memory comes before a sound");
}

int main()
{
    NoticeRates();
    HumanFloor();
    Memory();
    Hearing();
    Priorities();
    if (gFailures)
    {
        std::printf("\n%d bot perception check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("\nAll bot perception checks passed.\n");
    return 0;
}
