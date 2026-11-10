// The desktop client's controller mapping (gamepad_map.h).

#include "gamepad_map.h"

namespace kisak::gamepad {

namespace vi = vita_input;

namespace {

constexpr float kTriggerHysteresis = 0.1f; // a trigger lets go this far below where it took hold

struct ControlInfo
{
    int vita;            // the Vita control the mapping is fed, or -1: a key this file sends
    int key;             // the key it sends in the game
    const char *name;
    const char *binding;
};

// In the order of Button, then the rest of Control. The keys and the default
// commands of the Vita controls come from the Vita mapping, so the two cannot
// drift apart; the four it has no button for use its touch areas' keys and
// defaults (checked by the host test).
const ControlInfo kControls[kControlCount] = {
    {vi::kCross, 0, "A", nullptr},
    {vi::kCircle, 0, "B", nullptr},
    {vi::kSquare, 0, "X", nullptr},
    {vi::kTriangle, 0, "Y", nullptr},
    {vi::kSelect, 0, "Back", nullptr},
    {vi::kStart, 0, "Start", nullptr},
    {vi::kDpadUp, 0, "D-pad up", nullptr},
    {vi::kDpadDown, 0, "D-pad down", nullptr},
    {vi::kDpadLeft, 0, "D-pad left", nullptr},
    {vi::kDpadRight, 0, "D-pad right", nullptr},
    {-1, vi::kKeyAux1 + 12, "Left bumper", "+smoke"},
    {-1, vi::kKeyAux1 + 13, "Right bumper", "+frag"},
    {-1, vi::kKeyAux1 + 7, "Left stick click", "+breath_sprint"},
    {-1, vi::kKeyAux1 + 14, "Right stick click", "+melee"},
    {vi::kLeftShoulder, 0, "Left trigger", nullptr},
    {vi::kRightShoulder, 0, "Right trigger", nullptr},
    {vi::kStickUp, 0, "Left stick up", nullptr},
    {vi::kStickDown, 0, "Left stick down", nullptr},
    {vi::kStickLeft, 0, "Left stick left", nullptr},
    {vi::kStickRight, 0, "Left stick right", nullptr},
};

// The buttons the Vita mapping is not fed, in the order of extraDown_.
constexpr int kExtras[] = {kLeftBumper, kRightBumper, kLeftStickClick, kRightStickClick};
constexpr int kSprintExtra = 2;

bool IsControl(int control)
{
    return control >= 0 && control < kControlCount;
}

// SDL's -32768..32767 as the Vita's 0..255 with 128 at rest.
uint8_t Axis(int16_t value)
{
    return static_cast<uint8_t>((static_cast<int>(value) + 32768) >> 8);
}

bool Pulled(int16_t value, bool was, float threshold)
{
    threshold = threshold < 0.05f ? 0.05f : threshold > 0.95f ? 0.95f : threshold;
    const float travel = static_cast<float>(value < 0 ? 0 : value) / 32767.0f;
    return travel >= (was ? threshold - kTriggerHysteresis * threshold : threshold);
}

int Clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

} // namespace

int GameKey(int control)
{
    if (!IsControl(control))
        return 0;
    const ControlInfo &info = kControls[control];
    return info.vita >= 0 ? vi::GameKey(static_cast<vi::Control>(info.vita)) : info.key;
}

const char *ControlName(int control)
{
    return IsControl(control) ? kControls[control].name : "";
}

const char *DefaultBinding(int control)
{
    if (!IsControl(control))
        return nullptr;
    const ControlInfo &info = kControls[control];
    return info.vita >= 0 ? vi::DefaultBinding(static_cast<vi::Control>(info.vita)) : info.binding;
}

void Mapper::Update(const Reading &reading, uint32_t nowMs, bool menu, bool bindPending, const Settings &settings,
                    vi::KeySink &sink)
{
    leftTrigger_ = Pulled(reading.leftTrigger, leftTrigger_, settings.triggerThreshold);
    rightTrigger_ = Pulled(reading.rightTrigger, rightTrigger_, settings.triggerThreshold);

    vi::PadState state;
    for (int button = 0; button < kButtonCount; ++button)
    {
        const int vita = kControls[button].vita;
        if (vita >= 0 && (reading.buttons >> button & 1) != 0)
            state.buttons |= 1u << vita;
    }
    if (leftTrigger_)
        state.buttons |= 1u << vi::kLeftShoulder;
    if (rightTrigger_)
        state.buttons |= 1u << vi::kRightShoulder;
    state.lx = Axis(reading.lx);
    state.ly = Axis(reading.ly);
    state.rx = Axis(reading.rx);
    state.ry = Axis(reading.ry);

    // A controller has no touch panels: the Vita's touch keys never come from
    // the mapping, so the four buttons below are their only source.
    vi::Settings pad = settings.pad;
    pad.frontTouch = false;
    pad.rearTouch = false;
    pad_.Update(state, nowMs, menu, bindPending, pad, sink);

    // The bumpers and the stick clicks: plain buttons, down while held. Like
    // the mapping's own buttons they send their game key in a menu too, where
    // it is bound to nothing.
    const bool forward = !menu && pad_.Held(vi::kStickUp);
    for (int i = 0; i < kExtraCount; ++i)
    {
        bool down = (reading.buttons >> kExtras[i] & 1) != 0;
        if (i == kSprintExtra)
        {
            // A click starts the sprint; pushing on keeps it, as on a console.
            if (down && !menu)
                sprintLatched_ = true;
            else if (!forward)
                sprintLatched_ = false;
            down = down || sprintLatched_;
        }
        if (down != extraDown_[i])
        {
            extraDown_[i] = down;
            sink.Key(kControls[kExtras[i]].key, down);
        }
    }

    const vi::PadState rest;
    active_ = reading.buttons != 0 || leftTrigger_ || rightTrigger_ || sprintLatched_ || pad_.Held(vi::kStickUp)
              || pad_.Held(vi::kStickDown) || pad_.Held(vi::kStickLeft) || pad_.Held(vi::kStickRight)
              || state.rx != rest.rx || state.ry != rest.ry || state.lx != rest.lx || state.ly != rest.ly;
}

vi::Motion Mapper::Frame(int mouseX, int mouseY, int mouseDx, int mouseDy, const Settings &settings)
{
    const bool mouseMoved = !mouseSeen_ || mouseX != lastMouseX_ || mouseY != lastMouseY_ || mouseDx != 0
                            || mouseDy != 0;
    mouseSeen_ = true;
    lastMouseX_ = mouseX;
    lastMouseY_ = mouseY;
    if (mouseMoved)
    {
        // The mouse takes the cursor, and the stick carries on from there.
        // Only now: set every frame, a slow push of the stick would lose the
        // part of a pixel it gains each time.
        padCursor_ = false;
        SetCursor(mouseX, mouseY, settings);
    }
    const vi::Motion pad = pad_.Take();
    if (mouseMoved)
        cursorX_ = pad.x; // -1 while the d-pad has it hidden: not a move of the pad's
    else if (pad.x != cursorX_ || pad.y != cursorY_)
        padCursor_ = true;

    vi::Motion out;
    out.x = padCursor_ ? pad.x : mouseX;
    out.y = padCursor_ ? pad.y : mouseY;
    out.dx = mouseDx + pad.dx;
    out.dy = mouseDy + pad.dy;
    if (padCursor_)
    {
        cursorX_ = pad.x;
        cursorY_ = pad.y;
    }
    return out;
}

void Mapper::SetCursor(int x, int y, const Settings &settings)
{
    pad_.SetCursor(x, y, settings.pad);
    cursorX_ = Clamp(x, 0, settings.pad.screenWidth - 1);
    cursorY_ = Clamp(y, 0, settings.pad.screenHeight - 1);
}

void Mapper::Release(vi::KeySink &sink)
{
    pad_.Release(sink);
    for (int i = 0; i < kExtraCount; ++i)
    {
        if (extraDown_[i])
        {
            extraDown_[i] = false;
            sink.Key(kControls[kExtras[i]].key, false);
        }
    }
    sprintLatched_ = false;
    active_ = false;
}

} // namespace kisak::gamepad
