// How a KisakCOD bot fights: aiming with a human hand, the trigger, recoil and
// moving under fire (docs/bots.md, phase 3).
//
// A bot fights only a player its perception (bot_perception.h) noticed and
// sees now, and only through its user command, like a player:
//
//   reaction  after noticing, the hand starts moving onsetMs later (a
//             player's decision and motor onset on top of the 0.2 s it
//             took to notice).
//   aim       the hand makes aimed movements toward where the bot saw the
//             player, each landing off by an error that grows with the size
//             of the movement (a quick 60 degree flick lands a few degrees
//             off, a small correction lands close), and corrects again
//             every correctionMs: a human's flick, then settle.
//   tracking  the bot sees a moving player ten times a second (its sense
//             ticks) and leads only part of the motion it saw, so it trails
//             a strafing player the way a player's hand does.
//   trigger   it fires only while its view is on the player it sees, by the
//             bot's own estimate, never by where the player really is: taps
//             a semi-automatic at a human clicking pace, holds an automatic,
//             in bursts at range.
//   recoil    the game's view kick is the player's client's job (CG_KickAngles
//             adds it to every user command), and a bot has no client, so
//             the bot kicks its own view the same way, from the weapon's own
//             numbers, and its hand pulls back only part of it, late.
//   moving    while it fights it keeps walking or strafes from side to side.
//
// Aim-down-sight, reload and the view error a weapon adds on entering the
// sight are in src/bots/bot_brain.cpp, which reads the weapon.
//
// No engine call and no engine type: scripts/tests/test_bot_combat.py runs it
// on the host.

#pragma once

#include "bot_motor.h"

#include <cmath>
#include <cstdint>

namespace kisak::bots
{

// A normal random number (mean 0, deviation 1) from two uniform ones.
inline float Gaussian(Rng &rng)
{
    float u = rng.Unit();
    if (u < 1e-7f)
        u = 1e-7f;
    const float v = rng.Unit();
    return std::sqrt(-2.0f * std::log(u)) * std::cos(6.28318530718f * v);
}

// How a bot fights: one profile for now (phase 5 adds difficulties). The
// numbers are a decent, not a great, player's.
struct SkillProfile
{
    // After noticing a player, the hand starts after this many ms (mean,
    // deviation, floor).
    float onsetMeanMs = 120.0f;
    float onsetDeviationMs = 40.0f;
    float onsetMinMs = 60.0f;

    // The hand on an aimed movement: the spring's rate and top speed, in
    // degrees a second.
    float aimResponsiveness = 16.0f;
    float aimMaxSpeed = 450.0f;

    // Where an aimed movement lands: off by a normal error with deviation
    // aimErrorBase + aimErrorPerDegree * the movement's size, in degrees.
    float aimErrorBase = 0.35f;
    float aimErrorPerDegree = 0.07f;
    // The next corrective movement starts this long after the last one
    // (mean, deviation, floor).
    float correctionMeanMs = 170.0f;
    float correctionDeviationMs = 40.0f;
    float correctionMinMs = 100.0f;
    // The share of a seen player's motion since the last look that the hand
    // leads (0 aims where the player was seen, 1 where they are now).
    float leadFraction = 0.6f;

    // The trigger: on target when the view is within this share of the
    // player's angular half-width of where the bot thinks they are, and
    // never tighter than fireConeMinDegrees.
    float fireConeScale = 1.0f;
    float fireConeMinDegrees = 0.5f;
    // Semi-automatic: one click every tapMeanMs (deviation, floor).
    float tapMeanMs = 170.0f;
    float tapDeviationMs = 40.0f;
    float tapMinMs = 110.0f;
    // Automatic: beyond burstRange, bursts of burstMeanMs with pauses of
    // burstPauseMs (each varied by up to a third).
    float burstRange = 900.0f;
    float burstMeanMs = 450.0f;
    float burstPauseMs = 250.0f;

    // Recoil: the share of the view kick the hand pulls back, and how late
    // (the time constant of the hand following the kick, in seconds).
    float recoilCompensation = 0.5f;
    float recoilLagSeconds = 0.15f;

    // Aims down the sight at players further than this.
    float adsRange = 400.0f;

    // Under fire: a strafe or a walk on lasts strafeMinSeconds to
    // strafeMaxSeconds; strafeShare of them are strafes.
    float strafeMinSeconds = 0.4f;
    float strafeMaxSeconds = 1.2f;
    float strafeShare = 0.6f;
};

// A sample from a normal distribution with a floor, as whole milliseconds.
inline int DelayMs(Rng &rng, float mean, float deviation, float floor)
{
    float ms = mean + deviation * Gaussian(rng);
    if (ms < floor)
        ms = floor;
    return static_cast<int>(std::lround(ms));
}

// ---------------------------------------------------------------------------
// The weapon's view kick, as the player's client gives it.

// One weapon's kick, hip or aimed (WeaponDef fHip... / fAds... fields).
struct KickParams
{
    float pitchMin = 0.0f;
    float pitchMax = 0.0f;
    float yawMin = 0.0f;
    float yawMax = 0.0f;
    float centerSpeed = 2400.0f;
};

struct ViewKick
{
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f}; // pitch, yaw, roll, added to the view
};

// One shot (the view part of BG_WeaponFireRecoil): `reduce` is the weapon's
// reduced-kick share while weaponRestrictKickTime runs, 1 otherwise, and
// `unitPitch`, `unitYaw` are in [0, 1).
inline void KickOnShot(ViewKick &kick, const KickParams &params, float reduce, float unitPitch, float unitYaw)
{
    const float pitch = (unitPitch * (params.pitchMax - params.pitchMin) + params.pitchMin) * reduce;
    const float yaw = (unitYaw * (params.yawMax - params.yawMin) + params.yawMin) * reduce;
    kick.velocity[0] = -pitch;
    kick.velocity[1] = yaw;
    kick.velocity[2] = yaw * -0.5f;
}

// The kick over `ms` milliseconds, in the 5 ms steps CG_KickAngles takes: it
// flies out, is pulled back toward the centre at centerSpeed, and never
// passes 10 degrees.
inline void StepKick(ViewKick &kick, float centerSpeed, int ms)
{
    for (int left = ms; left > 0; left -= 5)
    {
        const float ft = static_cast<float>(left <= 5 ? left : 5) * 0.001f;
        for (int i = 0; i < 3; ++i)
        {
            float &velocity = kick.velocity[i];
            float &angle = kick.angles[i];
            if (velocity == 0.0f && angle == 0.0f)
                continue;
            if (angle != 0.0f)
                velocity += (angle <= 0.0f ? 1.0f : -1.0f) * centerSpeed * ft;
            float change = velocity * ft;
            if (angle * change < 0.0f)
                change *= 0.06f;
            if ((angle + change) * angle < 0.0f)
            {
                angle = 0.0f;
                velocity = 0.0f;
            }
            else
            {
                angle += change;
                if (angle == 0.0f)
                {
                    velocity = 0.0f;
                }
                else if (std::fabs(angle) > 10.0f)
                {
                    angle = angle > 0.0f ? 10.0f : -10.0f;
                    velocity = 0.0f;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Aiming.

// The angular size of a player at `distance` units: half their width (about
// 16 units), in degrees.
inline float TargetHalfWidthDegrees(float distance)
{
    if (!(distance > 1.0f))
        distance = 1.0f;
    return std::atan2(16.0f, distance) * (180.0f / 3.14159265358979f);
}

// How far the view (pitch, yaw) is from `target`, in degrees on the screen.
inline float AimError(const float *view, const float *target)
{
    const float pitch = AngleDelta(target[0], view[0]);
    const float yaw = AngleDelta(target[1], view[1]) * std::cos(target[0] * (3.14159265358979f / 180.0f));
    return std::sqrt(pitch * pitch + yaw * yaw);
}

struct AimState
{
    int target = -1;
    // When the hand may first move toward the target.
    int startMs = 0;
    // When the next aimed movement starts, and where the current one lands,
    // relative to the target (pitch, yaw).
    int nextMoveMs = 0;
    float offset[2] = {0.0f, 0.0f};
    // The kick as the hand has caught up with it so far.
    float kickFelt[2] = {0.0f, 0.0f};
};

// Starts aiming at `target`, noticed at `noticedAt`: the hand moves after the
// reaction onset, counted from the notice or from now, whichever is later.
inline void Engage(AimState &aim, int target, int noticedAt, int nowMs, const SkillProfile &skill, Rng &rng)
{
    aim = AimState();
    aim.target = target;
    const int from = noticedAt > nowMs ? noticedAt : nowMs;
    aim.startMs = from + DelayMs(rng, skill.onsetMeanMs, skill.onsetDeviationMs, skill.onsetMinMs);
    aim.nextMoveMs = aim.startMs;
}

// One frame of aiming at `target` (pitch, yaw, where the bot thinks the
// player is) for `seconds`, with the weapon's kick now at `kick` (pitch,
// yaw). Moves `view`, the hand's view without the kick.
inline void StepAim(AimState &aim, ViewMotorState &view, const float *target, const float *kick, int nowMs,
                    float seconds, const SkillProfile &skill, Rng &rng)
{
    if (seconds > 0.0f && skill.recoilLagSeconds > 0.0f)
    {
        const float follow = 1.0f - std::exp(-seconds / skill.recoilLagSeconds);
        for (int axis = 0; axis < 2; ++axis)
            aim.kickFelt[axis] += (kick[axis] - aim.kickFelt[axis]) * follow;
    }

    ViewMotorParams params;
    params.responsiveness = skill.aimResponsiveness;
    params.maxSpeed = skill.aimMaxSpeed;
    if (nowMs < aim.startMs)
    {
        // Not moving yet: the hand comes to rest where it is.
        StepView(view, view.angles[0], view.angles[1], seconds, params);
        return;
    }

    if (nowMs >= aim.nextMoveMs)
    {
        // A new aimed movement: it lands off by an error that grows with how
        // far the hand has to go (the kick included: the eye sees the view).
        const float seen[2] = {view.angles[0] + kick[0], view.angles[1] + kick[1]};
        const float size = AimError(seen, target);
        const float deviation = skill.aimErrorBase + skill.aimErrorPerDegree * size;
        aim.offset[0] = deviation * Gaussian(rng) * 0.7f; // people err less up and down
        aim.offset[1] = deviation * Gaussian(rng);
        aim.nextMoveMs = nowMs + DelayMs(rng, skill.correctionMeanMs, skill.correctionDeviationMs,
                                         skill.correctionMinMs);
    }

    const float goalPitch = target[0] + aim.offset[0] - skill.recoilCompensation * aim.kickFelt[0];
    const float goalYaw = target[1] + aim.offset[1] - skill.recoilCompensation * aim.kickFelt[1];
    StepView(view, goalPitch, goalYaw, seconds, params);
}

// Whether the bot thinks its view (the hand plus the kick) is on the player.
inline bool OnTarget(const AimState &aim, const ViewMotorState &view, const float *kick, const float *target,
                     float distance, int nowMs, const SkillProfile &skill)
{
    if (nowMs < aim.startMs)
        return false;
    const float seen[2] = {view.angles[0] + kick[0], view.angles[1] + kick[1]};
    float cone = TargetHalfWidthDegrees(distance) * skill.fireConeScale;
    if (cone < skill.fireConeMinDegrees)
        cone = skill.fireConeMinDegrees;
    return AimError(seen, target) <= cone;
}

// Where the bot thinks a player it saw at `seen` (moving at `velocity`, units
// a second) is `sinceMs` after it looked: it leads part of the motion, for at
// most one sense tick's worth.
inline void LeadPlace(const float *seen, const float *velocity, int sinceMs, const SkillProfile &skill, float *out)
{
    float seconds = static_cast<float>(sinceMs) * 0.001f;
    if (seconds < 0.0f)
        seconds = 0.0f;
    if (seconds > 0.15f)
        seconds = 0.15f;
    for (int k = 0; k < 3; ++k)
        out[k] = seen[k] + velocity[k] * seconds * skill.leadFraction;
}

// ---------------------------------------------------------------------------
// The trigger.

struct TriggerState
{
    bool held = false;
    int nextPressMs = 0;
    int burstEndMs = 0;
    int pauseEndMs = 0;
};

// Whether the attack button is down this frame. `automatic` holds it while on
// target; otherwise each shot is its own press, released the next frame.
inline bool StepTrigger(TriggerState &trigger, bool onTarget, bool automatic, float distance, int nowMs,
                        const SkillProfile &skill, Rng &rng)
{
    if (!automatic)
    {
        if (trigger.held)
        {
            trigger.held = false;
            return false;
        }
        if (!onTarget || nowMs < trigger.nextPressMs)
            return false;
        trigger.held = true;
        trigger.nextPressMs = nowMs + DelayMs(rng, skill.tapMeanMs, skill.tapDeviationMs, skill.tapMinMs);
        return true;
    }

    if (!onTarget)
    {
        trigger.held = false;
        return false;
    }
    if (distance < skill.burstRange)
    {
        trigger.held = true;
        return true;
    }
    if (trigger.held)
    {
        if (nowMs < trigger.burstEndMs)
            return true;
        trigger.held = false;
        trigger.pauseEndMs = nowMs + static_cast<int>(skill.burstPauseMs * rng.Range(0.67f, 1.33f));
        return false;
    }
    if (nowMs < trigger.pauseEndMs)
        return false;
    trigger.held = true;
    trigger.burstEndMs = nowMs + static_cast<int>(skill.burstMeanMs * rng.Range(0.67f, 1.33f));
    return true;
}

// ---------------------------------------------------------------------------
// Moving under fire.

struct StrafeState
{
    int side = 0; // -1 left, 1 right, 0 walk on
    int untilMs = 0;
};

// The strafe for this frame: -1 or 1 to step sideways, 0 to keep walking.
inline int StepStrafe(StrafeState &strafe, int nowMs, const SkillProfile &skill, Rng &rng)
{
    if (nowMs >= strafe.untilMs)
    {
        if (rng.Unit() < skill.strafeShare)
        {
            // A strafe, more often the other way than the last one.
            const bool flip = strafe.side != 0 ? rng.Unit() < 0.7f : rng.Unit() < 0.5f;
            strafe.side = strafe.side != 0 ? (flip ? -strafe.side : strafe.side) : (flip ? -1 : 1);
        }
        else
        {
            strafe.side = 0;
        }
        strafe.untilMs = nowMs + static_cast<int>(1000.0f * rng.Range(skill.strafeMinSeconds, skill.strafeMaxSeconds));
    }
    return strafe.side;
}

} // namespace kisak::bots
