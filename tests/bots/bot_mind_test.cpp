// Host test for src/bots/bot_mind.h (scripts/tests/test_bot_mind.py).
//
// The Mind picks what a bot does from what it knows: these checks hold it to
// sensible, human choices (fight what it sees, get out to reload or heal when
// there is cover, hunt a lead, roam otherwise), to its personality, and to
// not flipping back and forth. The AreaMemory sends bots to places they have
// not seen and where enemies were; PreAimPoint finds the corner of the way.

#include <bots/bot_mind.h>
#include <bots/bot_motor.h>

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

static kb::Personality Leaning(float aggression, float patience = 0.5f)
{
    kb::Personality p;
    p.aggression = aggression;
    p.patience = patience;
    return p;
}

static kb::Intent Pick(const kb::Situation &s, const kb::Personality &p)
{
    kb::Mind mind;
    mind.Reset(-100000);
    return mind.Decide(s, p);
}

static void Choices()
{
    std::printf("\n== what a bot chooses ==\n");
    const kb::Personality calm = Leaning(0.2f);
    const kb::Personality bold = Leaning(0.9f);

    kb::Situation nothing;
    nothing.nowMs = 10000;
    Check(Pick(nothing, calm) == kb::Intent::Roam && Pick(nothing, bold) == kb::Intent::Roam,
          "with nothing known, a bot roams");

    kb::Situation fight = nothing;
    fight.enemyInView = true;
    fight.seenEnemies = 1;
    fight.coverKnown = true;
    Check(Pick(fight, calm) == kb::Intent::Fight && Pick(fight, bold) == kb::Intent::Fight,
          "an enemy in view with a full clip and full health: fight");

    kb::Situation reload = fight;
    reload.reloading = true;
    reload.clip = 0.0f;
    reload.canReload = true;
    Check(Pick(reload, calm) == kb::Intent::Cover, "a calm bot reloading in a fight takes the cover it knows");
    Check(Pick(reload, Leaning(0.5f)) == kb::Intent::Cover && Pick(reload, bold) == kb::Intent::Fight,
          "so does a middling one; a bold one reloads where it stands");
    kb::Situation noCover = reload;
    noCover.coverKnown = false;
    Check(Pick(noCover, calm) == kb::Intent::Fight && Pick(noCover, bold) == kb::Intent::Fight,
          "with no cover known, it fights on");
    Check(kb::CoverNeed(reload, bold) < kb::CoverNeed(reload, calm),
          "a bold bot wants cover less than a calm one");

    kb::Situation dry = reload;
    dry.canReload = false;
    dry.reloading = false;
    Check(Pick(dry, calm) == kb::Intent::Fight, "with nothing to reload, cover does not help: fight");

    kb::Situation hurt = fight;
    hurt.health = 0.3f;
    hurt.hurtAgoMs = 200;
    Check(Pick(hurt, calm) == kb::Intent::Cover, "badly hurt in a fight: cover");
    kb::Situation healing = nothing;
    healing.coverKnown = true;
    healing.inCover = true;
    healing.health = 0.6f;
    healing.hurtAgoMs = 5000;
    Check(Pick(healing, calm) == kb::Intent::Cover, "in cover and still hurt: stays to heal");
    healing.health = 1.0f;
    Check(Pick(healing, calm) == kb::Intent::Roam, "healed, with nothing known: leaves cover");

    kb::Situation crowd = fight;
    crowd.seenEnemies = 3;
    Check(Pick(crowd, calm) == kb::Intent::Cover, "a calm bot facing three enemies takes cover");

    kb::Situation lead = nothing;
    lead.enemyRemembered = true;
    lead.rememberedConfidence = 0.8f;
    Check(Pick(lead, calm) == kb::Intent::Hunt && Pick(lead, bold) == kb::Intent::Hunt,
          "an enemy it lost sight of: hunt");
    kb::Situation sound = nothing;
    sound.heardSomething = true;
    Check(Pick(sound, bold) == kb::Intent::Hunt, "a bold bot hunts a sound");
    Check(kb::ScoreIntents(sound, bold).hunt > kb::ScoreIntents(sound, calm).hunt,
          "a bold bot wants to hunt more than a calm one");
    kb::Situation claimed = sound;
    claimed.teammatesHunting = 1;
    Check(Pick(claimed, bold) == kb::Intent::Roam, "a sound a teammate already went after: even a bold bot keeps on");
    kb::Situation sharedLead = lead;
    sharedLead.teammatesHunting = 1;
    Check(Pick(sharedLead, bold) == kb::Intent::Hunt, "an enemy it saw itself: a second bot may still go");
    sharedLead.teammatesHunting = 2;
    Check(Pick(sharedLead, bold) == kb::Intent::Roam, "but never a third");
    kb::Situation faded = lead;
    faded.rememberedConfidence = 0.0f;
    faded.enemyRemembered = false;
    Check(Pick(faded, calm) == kb::Intent::Roam, "a forgotten lead: roam");
    kb::Situation flag = lead;
    flag.heardSomething = true;
    flag.forObjective = true;
    Check(Pick(flag, bold) == kb::Intent::Roam, "going for a flag or on it, it keeps at it rather than hunt a lead");
    kb::Situation flagFight = fight;
    flagFight.forObjective = true;
    Check(Pick(flagFight, bold) == kb::Intent::Fight, "but it fights an enemy in view there");
}

static void Steadiness()
{
    std::printf("\n== no flip-flopping ==\n");
    const kb::Personality p = Leaning(0.5f);
    kb::Mind mind;
    mind.Reset(0);
    // Two leads that score within the keep bonus of each other, alternating.
    int changes = 0;
    for (int frame = 0; frame < 200; ++frame)
    {
        kb::Situation s;
        s.nowMs = frame * 50;
        s.enemyRemembered = true;
        s.rememberedConfidence = (frame % 2) ? 0.1f : 0.15f;
        mind.Decide(s, p);
        changes += mind.changed ? 1 : 0;
    }
    Check(changes <= 1, "small swings in a score do not change the intent");

    // A hunt is kept for a moment even when roaming scores a little better...
    kb::Mind hunter;
    hunter.Reset(0);
    kb::Situation lead;
    lead.nowMs = 1000;
    lead.heardSomething = true;
    hunter.Decide(lead, p);
    Check(hunter.intent == kb::Intent::Hunt, "a sound starts a hunt");
    // ...but an enemy in view always takes over.
    kb::Situation seen = lead;
    seen.nowMs = 1100;
    seen.heardSomething = false;
    seen.enemyInView = true;
    seen.seenEnemies = 1;
    hunter.Decide(seen, p);
    Check(hunter.intent == kb::Intent::Fight, "an enemy in view ends a hunt at once");
}

static void Areas()
{
    std::printf("\n== where to roam ==\n");
    kb::AreaMemory memory;
    memory.Reset();
    const kb::Personality calm = Leaning(0.2f);
    const kb::Personality bold = Leaning(0.9f);
    const float here[3] = {100.0f, 100.0f, 0.0f};
    const float there[3] = {2000.0f, 100.0f, 0.0f};
    memory.Visit(here, 10000);
    Check(memory.Interest(there, 20000, calm) > memory.Interest(here, 20000, calm),
          "a place it has not seen is more interesting than one it just left");
    Check(memory.Interest(here, 80000, calm) > memory.Interest(here, 20000, calm),
          "a place it left a minute ago is interesting again");

    const float fight[3] = {-1500.0f, 800.0f, 0.0f};
    const float quiet[3] = {1500.0f, -800.0f, 0.0f};
    memory.EnemyAt(fight, 20000);
    Check(memory.Interest(fight, 25000, calm) > memory.Interest(quiet, 25000, calm),
          "a place where enemies were lately draws it");
    Check(memory.Interest(fight, 25000, bold) - memory.Interest(quiet, 25000, bold)
              > memory.Interest(fight, 25000, calm) - memory.Interest(quiet, 25000, calm),
          "more so for a bold bot");
    Check(std::fabs(memory.Interest(fight, 60000, calm) - memory.Interest(quiet, 60000, calm)) < 1e-4f,
          "after 30 s the enemies there are old news");

    const float far[3] = {1.0e6f, -1.0e6f, 0.0f};
    memory.Visit(far, 30000);
    Check(memory.Interest(far, 30001, calm) >= 0.0f, "a place off the grid is held at its edge");
}

// A made-up map: a wall fills x < 450, y > 50 (a corridor along x that turns
// left at x = 500).
static bool CornerWall(const float *from, const float *to, void *)
{
    for (int i = 1; i < 64; ++i)
    {
        const float t = static_cast<float>(i) / 64.0f;
        const float x = from[0] + (to[0] - from[0]) * t;
        const float y = from[1] + (to[1] - from[1]) * t;
        if (x < 450.0f && y > 50.0f)
            return false;
    }
    return true;
}

static bool OpenField(const float *, const float *, void *)
{
    return true;
}

static bool Blind(const float *, const float *, void *)
{
    return false;
}

static void PreAim()
{
    std::printf("\n== pre-aiming ==\n");
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float eye[3] = {0.0f, 0.0f, 60.0f};
    const float corners[3][3] = {{0.0f, 0.0f, 0.0f}, {500.0f, 0.0f, 0.0f}, {500.0f, 800.0f, 0.0f}};
    float out[3];
    const bool found = kb::PreAimPoint(eye, origin, corners, 3, 1, CornerWall, nullptr, out);
    std::printf("INFO: pre-aim at (%.0f, %.0f, %.0f)\n", out[0], out[1], out[2]);
    Check(found && std::fabs(out[0] - 500.0f) < 1.0f && out[1] >= 0.0f && out[1] < 64.0f,
          "round a corner, it aims where the way turns out of sight");
    Check(found && std::fabs(out[2] - kb::kHeadHeight) < 0.01f, "at head height");

    const bool open = kb::PreAimPoint(eye, origin, corners, 3, 1, OpenField, nullptr, out);
    const float along = 500.0f + out[1];
    Check(open && std::fabs(along - kb::kPreAimReach) < 1.0f, "in the open, it looks as far along its way as it reaches");

    Check(!kb::PreAimPoint(eye, origin, corners, 3, 1, Blind, nullptr, out), "with nothing in sight, no point");
    Check(!kb::PreAimPoint(eye, origin, corners, 3, 3, OpenField, nullptr, out), "with no way left, no point");
}

int main()
{
    Choices();
    Steadiness();
    Areas();
    PreAim();
    std::printf("\n%s\n", gFailures ? "SOME CHECKS FAILED" : "all bot mind checks passed");
    return gFailures ? 1 : 0;
}
