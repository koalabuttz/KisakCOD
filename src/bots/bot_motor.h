// How a KisakCOD bot moves its view: the "hand" between what the bot wants to
// look at and the user command it sends (docs/bots.md).
//
// A bot never sets its view directly. Like a player it sends a usercmd_s each
// frame and the server's Pmove turns that into a view (PM_UpdateViewAngles):
//
//     view = delta_angles + cmd.angles * 360 / 65536      (then wrapped)
//
// so the bot can only turn as fast as this motor lets it, and everything after
// the command (recoil, spread, sway, aim-down-sight time, damage) is the game's
// own player code.
//
// The motor is a critically damped spring on yaw and pitch with a top speed: it
// starts a turn smoothly, does not overshoot by itself, and slows as it
// arrives, the way a hand moves a mouse or a stick. Later phases add the flick
// overshoot, reaction time and tracking lag of the skill profile on top.
//
// No engine call and no engine type: src/bots/bot_brain.cpp feeds it the
// player state, and scripts/tests/test_bot_motor.py runs it on the host.

#pragma once

#include <cmath>
#include <cstdint>

namespace kisak::bots
{

constexpr float kDegreesPerShort = 360.0f / 65536.0f;

// Pitch limits a player has by default (player_view_pitch_up / _down).
constexpr float kMaxPitchUp = 85.0f;
constexpr float kMaxPitchDown = 85.0f;

// An angle in [-180, 180).
inline float AngleNormalize180(float angle)
{
    float turns = angle * (1.0f / 360.0f);
    return (turns - std::floor(turns + 0.5f)) * 360.0f;
}

// The shortest signed turn from `from` to `to`, in [-180, 180).
inline float AngleDelta(float to, float from)
{
    return AngleNormalize180(to - from);
}

// The usercmd angle that makes Pmove produce `view`, given the player state's
// delta_angles (which the server changes on spawn, on a mantle, and when it
// clamps pitch). A 16-bit value, as a human client sends it.
inline int32_t CmdAngleFor(float view, float deltaAngle)
{
    float shorts = AngleNormalize180(view - deltaAngle) / kDegreesPerShort;
    int32_t value = static_cast<int32_t>(std::lround(shorts));
    if (value >= 32768)
        value -= 65536;
    if (value < -32768)
        value += 65536;
    return value;
}

// The view a usercmd angle gives, as Pmove computes it.
inline float ViewForCmdAngle(int32_t cmdAngle, float deltaAngle)
{
    return AngleNormalize180(deltaAngle + static_cast<float>(cmdAngle) * kDegreesPerShort);
}

// A small, fast, reproducible random source, one per bot, so a bot's choices
// can be replayed from its seed in a test.
struct Rng
{
    uint32_t state = 0x9e3779b9u;

    void Seed(uint32_t seed)
    {
        state = seed ? seed : 0x9e3779b9u;
    }

    uint32_t Next()
    {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    // [0, 1)
    float Unit()
    {
        return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f);
    }

    // [lo, hi)
    float Range(float lo, float hi)
    {
        return lo + (hi - lo) * Unit();
    }
};

struct ViewMotorParams
{
    // How quickly the hand closes on the target (rad/s of the spring): about
    // 4 for an unhurried look around, 15-25 for a flick at an enemy.
    float responsiveness = 6.0f;
    // Top turn speed in degrees per second, per axis.
    float maxSpeed = 180.0f;
};

struct ViewMotorState
{
    float angles[2] = {0.0f, 0.0f}; // pitch, yaw
    float velocity[2] = {0.0f, 0.0f}; // degrees per second
    bool primed = false;
};

// Starts the motor from where the player is looking now: on spawn, on a new
// bot in the slot, or when the server moved the view (a respawn or a killcam).
inline void PrimeView(ViewMotorState &state, float pitch, float yaw)
{
    state.angles[0] = AngleNormalize180(pitch);
    state.angles[1] = AngleNormalize180(yaw);
    state.velocity[0] = 0.0f;
    state.velocity[1] = 0.0f;
    state.primed = true;
}

inline float ClampPitch(float pitch)
{
    if (pitch < -kMaxPitchUp)
        return -kMaxPitchUp;
    if (pitch > kMaxPitchDown)
        return kMaxPitchDown;
    return pitch;
}

// Moves the view toward `targetPitch`, `targetYaw` over `seconds`. Steps of at
// most 5 ms keep the spring stable at any server frame rate.
inline void StepView(ViewMotorState &state,
                     float targetPitch,
                     float targetYaw,
                     float seconds,
                     const ViewMotorParams &params)
{
    if (!(seconds > 0.0f))
        return;
    if (seconds > 0.25f)
        seconds = 0.25f;

    const float target[2] = {ClampPitch(targetPitch), AngleNormalize180(targetYaw)};
    const float omega = params.responsiveness > 0.0f ? params.responsiveness : 0.0f;
    const float maxSpeed = params.maxSpeed > 0.0f ? params.maxSpeed : 0.0f;

    int steps = static_cast<int>(std::ceil(seconds / 0.005f));
    if (steps < 1)
        steps = 1;
    const float dt = seconds / static_cast<float>(steps);

    for (int step = 0; step < steps; ++step)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            float error = AngleDelta(target[axis], state.angles[axis]);
            float accel = omega * omega * error - 2.0f * omega * state.velocity[axis];
            float velocity = state.velocity[axis] + accel * dt;
            if (velocity > maxSpeed)
                velocity = maxSpeed;
            if (velocity < -maxSpeed)
                velocity = -maxSpeed;
            state.velocity[axis] = velocity;
            state.angles[axis] = AngleNormalize180(state.angles[axis] + velocity * dt);
        }
        state.angles[0] = ClampPitch(state.angles[0]);
    }
}

// The movement keys that walk from `from` toward `to` (x, y) for a player
// looking along `viewYaw` (degrees; yaw 0 looks along +x, 90 along +y), at
// full stick: forward is along the view, right is to its right. A player can
// walk any way while looking another, so this does not wait for the turn.
struct MoveKeys
{
    int forward = 0; // -127..127
    int right = 0;   // -127..127
};

inline MoveKeys MoveToward(float viewYaw, const float *from, const float *to)
{
    MoveKeys keys;
    const float dx = to[0] - from[0];
    const float dy = to[1] - from[1];
    if (dx * dx + dy * dy < 1.0f)
        return keys;
    const float want = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
    const float error = AngleDelta(want, viewYaw) * (3.14159265358979f / 180.0f);
    keys.forward = static_cast<int>(std::lround(std::cos(error) * 127.0f));
    keys.right = static_cast<int>(std::lround(-std::sin(error) * 127.0f));
    return keys;
}

// The yaw that looks from `from` toward `to`.
inline float YawToward(const float *from, const float *to)
{
    return std::atan2(to[1] - from[1], to[0] - from[0]) * (180.0f / 3.14159265358979f);
}

// Where an idle bot looks next: a new heading off to one side, near level, and
// how long it holds it. Used until the bot has something to look at.
struct IdleLook
{
    float pitch = 0.0f;
    float yaw = 0.0f;
    float holdSeconds = 0.0f;
};

inline IdleLook NextIdleLook(Rng &rng, float currentYaw)
{
    IdleLook look;
    float turn = rng.Range(30.0f, 150.0f);
    if (rng.Unit() < 0.5f)
        turn = -turn;
    look.yaw = AngleNormalize180(currentYaw + turn);
    look.pitch = rng.Range(-8.0f, 8.0f);
    look.holdSeconds = rng.Range(1.5f, 4.0f);
    return look;
}

} // namespace kisak::bots
