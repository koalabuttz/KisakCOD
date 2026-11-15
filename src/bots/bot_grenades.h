// How a KisakCOD bot throws a grenade (docs/bots.md, phase 4c).
//
// A player throws a frag where an enemy went out of sight: round the corner
// they ran behind, over the wall they ducked behind. The bot does the same
// with what it knows: the place it last saw an enemy, a moment ago.
//
// The game throws a grenade from the eye along the view, at the weapon's
// projectile speed, plus its upward speed, plus its forward speed along the
// ground (Weapon_Throw_Grenade), and gravity pulls it down. ThrowPitch finds
// the view pitch whose flat arc comes down at the target; ArcPoints gives the
// arc for a check that it clears the map on the way (bounces are left to
// luck, as for a player).
//
// No engine call and no engine type: scripts/tests/test_bot_grenades.py runs
// it on the host.

#pragma once

#include <cmath>

namespace kisak::bots
{

struct ThrowParams
{
    float speed = 0.0f;        // along the view
    float speedUp = 0.0f;      // added straight up
    float speedForward = 0.0f; // added along the ground, the way the view faces
    float gravity = 800.0f;
};

// Where a grenade thrown from `eye` with the view `pitchUp` degrees above the
// horizon (the game's pitch is the opposite sign) is after `seconds`, as
// (distance along the ground, height above the eye).
inline void ArcAt(const ThrowParams &p, float pitchUp, float seconds, float *along, float *up)
{
    const float radians = pitchUp * (3.14159265358979f / 180.0f);
    const float horizontal = p.speed * std::cos(radians) + p.speedForward;
    const float vertical = p.speed * std::sin(radians) + p.speedUp;
    *along = horizontal * seconds;
    *up = vertical * seconds - 0.5f * p.gravity * seconds * seconds;
}

// The height above the eye at which the arc of `pitchUp` crosses `distance`
// along the ground, or a large negative number when it never gets there.
inline float ArcHeightAt(const ThrowParams &p, float pitchUp, float distance)
{
    const float radians = pitchUp * (3.14159265358979f / 180.0f);
    const float horizontal = p.speed * std::cos(radians) + p.speedForward;
    if (horizontal <= 1.0f)
        return -1.0e9f;
    const float t = distance / horizontal;
    const float vertical = p.speed * std::sin(radians) + p.speedUp;
    return vertical * t - 0.5f * p.gravity * t * t;
}

constexpr float kThrowPitchLowest = -30.0f;
constexpr float kThrowPitchHighest = 60.0f;

// The lowest view pitch (degrees above the horizon) whose arc comes down at
// `distance` along the ground and `height` above the eye: the flat throw a
// player makes, not a lob. False when the throw cannot reach.
inline bool ThrowPitch(const ThrowParams &p, float distance, float height, float *pitchUp)
{
    if (distance <= 0.0f || p.speed <= 0.0f)
        return false;
    // The arc's height at the distance grows with the pitch up to the
    // furthest throw, then falls: find the first pitch that reaches.
    float low = kThrowPitchLowest;
    if (ArcHeightAt(p, low, distance) >= height)
    {
        *pitchUp = low;
        return true;
    }
    float high = low;
    bool reached = false;
    for (float pitch = low + 1.0f; pitch <= kThrowPitchHighest; pitch += 1.0f)
    {
        if (ArcHeightAt(p, pitch, distance) >= height)
        {
            high = pitch;
            reached = true;
            break;
        }
        low = pitch;
    }
    if (!reached)
        return false;
    for (int step = 0; step < 20; ++step)
    {
        const float middle = 0.5f * (low + high);
        if (ArcHeightAt(p, middle, distance) >= height)
            high = middle;
        else
            low = middle;
    }
    *pitchUp = high;
    return true;
}

// Points along the arc from `eye` thrown toward `yawDegrees` at `pitchUp`,
// every `stepSeconds`, until it has gone `distance` along the ground (the
// last point is there). Returns how many, at most `max`.
inline int ArcPoints(const ThrowParams &p, const float *eye, float yawDegrees, float pitchUp, float distance,
                     float stepSeconds, float (*out)[3], int max)
{
    const float yaw = yawDegrees * (3.14159265358979f / 180.0f);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    int count = 0;
    for (float t = stepSeconds; count < max; t += stepSeconds)
    {
        float along = 0.0f;
        float up = 0.0f;
        ArcAt(p, pitchUp, t, &along, &up);
        const bool last = along >= distance;
        if (last)
        {
            along = distance;
            up = ArcHeightAt(p, pitchUp, distance);
        }
        out[count][0] = eye[0] + c * along;
        out[count][1] = eye[1] + s * along;
        out[count][2] = eye[2] + up;
        ++count;
        if (last || t > 10.0f)
            break;
    }
    return count;
}

} // namespace kisak::bots
