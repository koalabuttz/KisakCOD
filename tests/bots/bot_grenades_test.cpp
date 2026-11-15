// Host test for src/bots/bot_grenades.h (scripts/tests/test_bot_grenades.py).
//
// The throw a bot makes comes down where it aims, the way the game throws a
// grenade: the flat arc, not a lob, higher for further, out of reach past the
// weapon's range.

#include <bots/bot_grenades.h>

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

int main()
{
    kb::ThrowParams frag;
    frag.speed = 600.0f;
    frag.speedUp = 150.0f;
    frag.gravity = 800.0f;

    float level = 0.0f;
    const bool found = kb::ThrowPitch(frag, 500.0f, -60.0f, &level);
    std::printf("INFO: 500 units away at the feet: %.1f degrees up\n", level);
    Check(found && std::fabs(kb::ArcHeightAt(frag, level, 500.0f) + 60.0f) < 2.0f,
          "the throw comes down where it aims");
    Check(kb::ArcHeightAt(frag, level - 3.0f, 500.0f) < -60.0f, "it is the flattest throw that gets there");

    float further = 0.0f;
    Check(kb::ThrowPitch(frag, 620.0f, -60.0f, &further) && further > level, "further away, higher up");
    float below = 0.0f;
    Check(kb::ThrowPitch(frag, 500.0f, -200.0f, &below) && below < level, "a target below, lower");
    float out = 0.0f;
    Check(!kb::ThrowPitch(frag, 5000.0f, -60.0f, &out), "past the throw's reach: no throw");
    Check(!kb::ThrowPitch(frag, 0.0f, -60.0f, &out), "nowhere to throw: no throw");

    kb::ThrowParams forward = frag;
    forward.speedForward = 200.0f;
    float pushed = 0.0f;
    Check(kb::ThrowPitch(forward, 500.0f, -60.0f, &pushed) && pushed < level,
          "a weapon's forward speed flattens the throw");

    const float eye[3] = {100.0f, 200.0f, 60.0f};
    float arc[64][3];
    const int count = kb::ArcPoints(frag, eye, 90.0f, level, 500.0f, 0.1f, arc, 64);
    const float *last = arc[count - 1];
    std::printf("INFO: %d arc points, last at (%.0f, %.0f, %.0f)\n", count, last[0], last[1], last[2]);
    Check(count >= 3 && std::fabs(last[0] - 100.0f) < 0.5f && std::fabs(last[1] - 700.0f) < 0.5f
              && std::fabs(last[2] - 0.0f) < 2.0f,
          "the arc ends at the target, the way the bot faces");
    bool rises = arc[0][2] > eye[2];
    Check(rises, "a throw at a target below the eye still leaves upward (the weapon's upward speed)");

    std::printf("\n%s\n", gFailures ? "SOME CHECKS FAILED" : "all bot grenade checks passed");
    return gFailures ? 1 : 0;
}
