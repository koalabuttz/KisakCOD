#pragma once

// Engine key codes to free-camera intent. Keys arrive as the production SE_KEY
// numbers (ui/keycodes.h: lowercase ASCII for letters), so this sits after the
// Sys_QueEvent drain exactly where the client would hand a key to its bindings.
//
// Fixed host bindings, not the player's config:
//   w/s forward/back, a/d strafe, space/c up/down, shift fast,
//   arrow keys turn (left/right yaw, up/down pitch).
// Holding both keys of a pair cancels, as +forward/+back do.

#include "free_camera.h"

#include <array>

namespace kisak::view {

class FreeCameraControls {
public:
    // Returns true when the key is one of the camera's bindings.
    bool Key(int key, bool down);
    // Relative pointer counts. Only meaningful while the window has captured the
    // pointer; the owner decides that before forwarding.
    void Mouse(int dx, int dy);
    // Pending pointer counts, cleared by this call.
    struct Look { int dx = 0, dy = 0; };
    Look TakeLook();
    MoveIntent Intent() const;
    // Drop every held key and pending count, as Key_ClearStates does on focus loss.
    void ReleaseAll();
    bool AnyHeld() const;

private:
    enum Binding { Forward, Back, Left, Right, Up, Down, Fast, TurnLeft, TurnRight, LookUp, LookDown, Count };
    static int BindingOf(int key);
    std::array<bool, Count> held_{};
    // Wide accumulators: a long unfocused burst cannot overflow before it is taken.
    long long dx_ = 0, dy_ = 0;
};

} // namespace kisak::view
