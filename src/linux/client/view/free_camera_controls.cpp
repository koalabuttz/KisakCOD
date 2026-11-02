#include "free_camera_controls.h"

#include <ui/keycodes.h>

#include <climits>

namespace kisak::view {

int FreeCameraControls::BindingOf(int key)
{
    switch (key) {
    case 'w': return Forward;
    case 's': return Back;
    case 'a': return Left;
    case 'd': return Right;
    case K_SPACE: return Up;
    case 'c': return Down;
    case K_SHIFT: return Fast;
    case K_LEFTARROW: return TurnLeft;
    case K_RIGHTARROW: return TurnRight;
    case K_UPARROW: return LookUp;
    case K_DOWNARROW: return LookDown;
    default: return -1;
    }
}

bool FreeCameraControls::Key(int key, bool down)
{
    const int binding = BindingOf(key);
    if (binding < 0)
        return false;
    held_[binding] = down;
    return true;
}

void FreeCameraControls::Mouse(int dx, int dy)
{
    dx_ += dx;
    dy_ += dy;
}

FreeCameraControls::Look FreeCameraControls::TakeLook()
{
    auto narrow = [](long long value) {
        if (value > INT_MAX) return INT_MAX;
        if (value < INT_MIN) return INT_MIN;
        return static_cast<int>(value);
    };
    Look look{narrow(dx_), narrow(dy_)};
    dx_ = dy_ = 0;
    return look;
}

MoveIntent FreeCameraControls::Intent() const
{
    auto pair = [this](Binding positive, Binding negative) {
        return static_cast<int>(held_[positive]) - static_cast<int>(held_[negative]);
    };
    MoveIntent intent;
    intent.forward = pair(Forward, Back);
    intent.right = pair(Right, Left);
    intent.up = pair(Up, Down);
    intent.yaw = pair(TurnLeft, TurnRight);
    intent.pitch = pair(LookDown, LookUp);
    intent.fast = held_[Fast];
    return intent;
}

void FreeCameraControls::ReleaseAll()
{
    held_.fill(false);
    dx_ = dy_ = 0;
}

bool FreeCameraControls::AnyHeld() const
{
    for (bool held : held_)
        if (held)
            return true;
    return false;
}

} // namespace kisak::view
