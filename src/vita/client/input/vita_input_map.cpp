// The PS Vita client's control mapping (vita_input_map.h).

#include "vita_input_map.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace kisak::vita_input {

namespace {

constexpr uint32_t kMaxStepMs = 100;     // a stalled frame is not a long push of a stick
constexpr uint32_t kRepeatDelayMs = 400; // a d-pad arrow held in a menu repeats after this,
constexpr uint32_t kRepeatEveryMs = 110; // and then this often
constexpr float kMoveHysteresis = 0.1f;  // a direction lets go this far below where it took hold
constexpr float kCursorCurve = 2.0f;     // the menu cursor's response: fine near the centre
constexpr float kMaxLook = 30000.0f;     // look motion kept while nothing takes it
constexpr uint32_t kRearTapMs = 300;     // a finger on the rear pad for longer is resting there
constexpr uint32_t kRearKeepMs = 250;    // a tapped key stays down this long past its tap, or
                                         // past the left stick's last push forward, or past L

struct ControlInfo
{
    int key;
    const char *name;
    const char *binding;
    int layout = kFirstLayout; // the default layout the control came with
};

// In the order of Control. The layout is the console game's where the Vita has
// the button: sprint (a click of the left stick there) is a tap on the rear
// touch pad, melee (a click of the right stick) and the grenades (the upper
// shoulder buttons) are areas of the screen. The chat line, a key of the PC's
// keyboard, is a small area at the top of the screen that no thumb rests on.
// The names are what the controls menu and the game's hints show.
const ControlInfo kControls[kControlCount] = {
    {kKeyAux1 + 0, "Cross", "+gostand"},
    {kKeyAux1 + 1, "Circle", "+stance"},
    {kKeyAux1 + 2, "Square", "+usereload"},
    {kKeyAux1 + 3, "Triangle", "weapnext"},
    {kKeyAux1 + 4, "L button", "+speed_throw"}, // not "L" and "R": the menu lists a PC's keys beside them
    {kKeyAux1 + 5, "R button", "+attack"},
    {kKeyAux1 + 6, "Select", "+scores"},
    {kKeyEscape, "Start", nullptr}, // Escape opens and closes the menu in the engine itself
    {kKeyKpUpArrow, "D-pad up", "+actionslot 1"},
    {kKeyKpDownArrow, "D-pad down", "+actionslot 2"},
    {kKeyKpLeftArrow, "D-pad left", "+actionslot 3"},
    {kKeyKpRightArrow, "D-pad right", "+actionslot 4"},
    {kKeyAux1 + 8, "Left stick up", "+forward"},
    {kKeyAux1 + 9, "Left stick down", "+back"},
    {kKeyAux1 + 10, "Left stick left", "+moveleft"},
    {kKeyAux1 + 11, "Left stick right", "+moveright"},
    {kKeyAux1 + 7, "Rear touch (left)", "+breath_sprint"},
    {kKeyAux1 + 15, "Rear touch (right)", "+breath_sprint"},
    {kKeyAux1 + 12, "Screen (left)", "+smoke"},
    {kKeyAux1 + 13, "Screen (top right)", "+frag"},
    {kKeyAux1 + 14, "Screen (bottom right)", "+melee"},
    {kKeyKp5, "Screen (top middle)", "chatmodepublic", 2},
};

bool IsControl(int control)
{
    return control >= 0 && control < kControlCount;
}

bool IsMenuArrow(int key)
{
    return key >= kKeyUpArrow && key <= kKeyRightArrow;
}

float Clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

struct Stick
{
    float x, y; // -1 left or up to 1 right or down
};

// A stick as a direction and a strength: nothing inside the dead zone, then 0
// at its edge to 1 at full deflection.
Stick ReadStick(uint8_t rawX, uint8_t rawY, float deadzone)
{
    // 128 is the centre, so 0 is a little past full deflection.
    const float x = Clamp((static_cast<float>(rawX) - 128.0f) / 127.0f, -1.0f, 1.0f);
    const float y = Clamp((static_cast<float>(rawY) - 128.0f) / 127.0f, -1.0f, 1.0f);
    const float length = std::sqrt(x * x + y * y);
    deadzone = Clamp(deadzone, 0.02f, 0.9f);
    if (length <= deadzone)
        return {0.0f, 0.0f};
    const float strength = Clamp((length - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
    return {x / length * strength, y / length * strength};
}

// The strength to a power: a small push is a smaller part of the full speed.
Stick Curved(Stick stick, float power)
{
    const float strength = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (strength <= 0.0f)
        return stick;
    const float scale = std::pow(strength, Clamp(power, 1.0f, 4.0f) - 1.0f);
    return {stick.x * scale, stick.y * scale};
}

bool Centred(Stick stick)
{
    return stick.x == 0.0f && stick.y == 0.0f;
}

// The area of the screen a finger landed in, in the game: the outer thirds,
// where the thumbs reach, and the top third of the middle, where one has to
// be sent. The rest of the middle is no key.
int FrontArea(const Touch &touch, const Settings &settings)
{
    if (touch.x < settings.screenWidth / 3)
        return kFrontLeft;
    if (touch.x >= settings.screenWidth - settings.screenWidth / 3)
        return touch.y < settings.screenHeight / 2 ? kFrontRightTop : kFrontRightBottom;
    return touch.y < settings.screenHeight / 3 ? kFrontTopMiddle : -1;
}

} // namespace

int GameKey(Control control)
{
    return IsControl(control) ? kControls[control].key : 0;
}

const char *ControlName(Control control)
{
    return IsControl(control) ? kControls[control].name : "";
}

const char *DefaultBinding(Control control)
{
    return IsControl(control) ? kControls[control].binding : nullptr;
}

int LayoutOf(Control control)
{
    return IsControl(control) ? kControls[control].layout : 0;
}

int Mapper::KeyFor(Control control) const
{
    if (mode_ == Mode::Menu)
    {
        switch (control)
        {
        case kCross:
            return cursorHidden_ ? kKeyEnter : kKeyMouse1;
        case kCircle:
            return kKeyEscape;
        case kDpadUp:
            return kKeyUpArrow;
        case kDpadDown:
            return kKeyDownArrow;
        case kDpadLeft:
            return kKeyLeftArrow;
        case kDpadRight:
            return kKeyRightArrow;
        default:
            break;
        }
    }
    return GameKey(control);
}

// Two controls can hold one key (Circle and Start are both Escape in a menu):
// the engine sees it go down once, and up when the last one lets go.
void Mapper::Press(int holder, int key, KeySink &sink)
{
    ReleaseHolder(holder, sink);
    bool down = false;
    for (const int held : heldKey_)
        down = down || held == key;
    heldKey_[holder] = key;
    if (!down)
        sink.Key(key, true);
}

void Mapper::ReleaseHolder(int holder, KeySink &sink)
{
    const int key = heldKey_[holder];
    if (!key)
        return;
    heldKey_[holder] = 0;
    for (const int held : heldKey_)
    {
        if (held == key)
            return;
    }
    sink.Key(key, false);
}

void Mapper::SetHeld(Control control, bool held, KeySink &sink)
{
    if (held && !heldKey_[control])
        Press(control, GameKey(control), sink);
    else if (!held && heldKey_[control])
        ReleaseHolder(control, sink);
}

void Mapper::EnterMode(Mode mode, KeySink &sink)
{
    for (int holder = kButtonCount; holder < kHolderCount; ++holder)
        ReleaseHolder(holder, sink);
    for (int i = 0; i < fingerCount_; ++i)
        fingers_[i].role = kFingerIdle;
    for (int i = 0; i < rearFingerCount_; ++i)
        rearFingers_[i].tap = false;
    click_ = Click::Idle;
    touchHeld_ = false;
    cursorDirty_ = false;
    lookX_ = lookY_ = 0.0f;
    scroll_ = 0.0f;
    // A stick pushed for the game, or to give a direction to an action, must
    // not carry the cursor off with it. Walking on out of a menu is wanted.
    stickWait_ = mode != Mode::Game;
    mode_ = mode;
}

void Mapper::Update(const PadState &pad, uint32_t nowMs, bool menu, bool bindPending, const Settings &settings,
                    KeySink &sink)
{
    float seconds = 0.0f;
    if (started_)
    {
        const uint32_t elapsed = nowMs - lastMs_;
        seconds = static_cast<float>(elapsed < kMaxStepMs ? elapsed : kMaxStepMs) / 1000.0f;
    }
    lastMs_ = nowMs;

    const Mode mode = !menu ? Mode::Game : bindPending ? Mode::Bind : Mode::Menu;
    if (!started_ || mode != mode_)
        EnterMode(mode, sink);
    started_ = true;

    UpdateButtons(pad, nowMs, sink);
    UpdateFront(pad, settings, sink);
    UpdateSticks(pad, seconds, settings, sink);
    UpdateRear(pad, nowMs, settings, sink); // after the sticks: it asks whether the left one pushes forward
    UpdateClick(sink);
}

void Mapper::UpdateButtons(const PadState &pad, uint32_t nowMs, KeySink &sink)
{
    const uint32_t buttons = pad.buttons & ((1u << kButtonCount) - 1);
    for (int i = 0; i < kButtonCount; ++i)
    {
        const bool down = (buttons >> i & 1) != 0;
        const bool was = (lastButtons_ >> i & 1) != 0;
        if (down && !was)
        {
            const int key = KeyFor(static_cast<Control>(i));
            Press(i, key, sink);
            if (IsMenuArrow(key))
            {
                // The d-pad moves the menu's focus. The cursor would take it
                // straight back to what it rests on, so it goes.
                cursorHidden_ = true;
                repeatAt_[i] = nowMs + kRepeatDelayMs;
            }
        }
        else if (!down && was)
        {
            ReleaseHolder(i, sink);
        }
        else if (down && mode_ == Mode::Menu && IsMenuArrow(heldKey_[i])
                 && static_cast<int32_t>(nowMs - repeatAt_[i]) >= 0)
        {
            // The engine repeats only up and down in a menu, so every arrow
            // is let go and pressed again.
            sink.Key(heldKey_[i], false);
            sink.Key(heldKey_[i], true);
            repeatAt_[i] = nowMs + kRepeatEveryMs;
        }
    }
    lastButtons_ = buttons;
}

void Mapper::UpdateFront(const PadState &pad, const Settings &settings, KeySink &sink)
{
    const int count = std::min<int>(pad.frontCount, kMaxFrontTouches);
    Finger now[kMaxFrontTouches];
    bool landed[kMaxFrontTouches];
    bool pointerDown = false;
    for (int i = 0; i < count; ++i)
    {
        now[i].id = pad.front[i].id;
        now[i].role = kFingerIdle;
        landed[i] = true;
        for (int j = 0; j < fingerCount_; ++j)
        {
            if (fingers_[j].id == now[i].id)
            {
                now[i].role = fingers_[j].role;
                landed[i] = false;
            }
        }
        // Two reports with one number: the second is nobody.
        for (int j = 0; j < i; ++j)
        {
            if (now[j].id == now[i].id)
            {
                now[i].role = kFingerIdle;
                landed[i] = false;
            }
        }
        pointerDown = pointerDown || now[i].role == kFingerPointer;
    }

    bool area[kControlCount] = {};
    const Touch *pointer = nullptr;
    bool pointerLanded = false;
    for (int i = 0; i < count; ++i)
    {
        if (landed[i])
        {
            if (mode_ == Mode::Menu)
            {
                // One finger has the cursor; more of them do nothing.
                if (!pointerDown)
                {
                    now[i].role = kFingerPointer;
                    pointerDown = true;
                    pointerLanded = true;
                }
            }
            else if (settings.frontTouch)
            {
                const int landedIn = FrontArea(pad.front[i], settings);
                if (landedIn >= 0)
                    now[i].role = static_cast<int8_t>(landedIn);
            }
        }
        if (now[i].role == kFingerPointer)
            pointer = &pad.front[i];
        else if (now[i].role >= 0)
            area[now[i].role] = true;
    }
    for (int i = 0; i < count; ++i)
        fingers_[i] = now[i];
    fingerCount_ = static_cast<uint8_t>(count);

    // A finger holds the area it landed in until it lifts, wherever it slides.
    for (const Control control : {kFrontLeft, kFrontRightTop, kFrontRightBottom, kFrontTopMiddle})
        SetHeld(control, area[control], sink);

    touchHeld_ = pointer != nullptr;
    if (!pointer)
        return;
    const float x = Clamp(static_cast<float>(pointer->x), 0.0f, static_cast<float>(settings.screenWidth - 1));
    const float y = Clamp(static_cast<float>(pointer->y), 0.0f, static_cast<float>(settings.screenHeight - 1));
    if (pointerLanded || x != cursorX_ || y != cursorY_)
        cursorDirty_ = true;
    cursorX_ = x;
    cursorY_ = y;
    cursorHidden_ = false;
    if (pointerLanded)
    {
        // The tap before this one may still owe its release.
        ReleaseHolder(kTouchClick, sink);
        click_ = Click::PressOwed;
    }
}

// The rear pad lies under the fingers that hold the console: the first
// console run logged 30 touches in 40 seconds where two taps were asked for,
// and one held its key for a second and a half before the menu was up. So a
// finger that stays is nothing, and a tap is the key press. The key then
// stays down for as long as the left stick is pushed forward, which makes the
// default binding the console game's sprint: tap, and run until you stop. It
// also stays down while L is held, the default for aiming down the sight,
// where the same binding holds the breath: tap, and the breath is held until
// the sight comes down. A tap while the key is down is a new press.
void Mapper::UpdateRear(const PadState &pad, uint32_t nowMs, const Settings &settings, KeySink &sink)
{
    const bool active = mode_ != Mode::Menu && settings.rearTouch;
    const int count = std::min<int>(pad.rearCount, kMaxRearTouches);

    // The fingers there now. One that was there before keeps the half it
    // landed on and its time; one that landed while the pad was off, or that
    // has stayed too long, can no longer tap.
    RearFinger fingers[kMaxRearTouches];
    for (int i = 0; i < count; ++i)
    {
        const Touch &touch = pad.rear[i];
        RearFinger finger = {touch.id, touch.x >= settings.screenWidth / 2, active, nowMs};
        for (int j = 0; j < rearFingerCount_; ++j)
        {
            if (rearFingers_[j].id == touch.id)
                finger = rearFingers_[j];
        }
        if (!active || nowMs - finger.sinceMs > kRearTapMs)
            finger.tap = false;
        fingers[i] = finger;
    }

    // The fingers that left: a tap if it was soon enough. Two that leave one
    // half together are one tap.
    bool tapped[2] = {false, false};
    for (int j = 0; j < rearFingerCount_; ++j)
    {
        const RearFinger &was = rearFingers_[j];
        bool there = false;
        for (int i = 0; i < count; ++i)
            there = there || fingers[i].id == was.id;
        if (!there && active && was.tap && nowMs - was.sinceMs <= kRearTapMs)
            tapped[was.right] = true;
    }
    for (int i = 0; i < count; ++i)
        rearFingers_[i] = fingers[i];
    rearFingerCount_ = static_cast<uint8_t>(count);

    for (int half = 0; half < 2; ++half)
    {
        const Control control = half ? kRearRight : kRearLeft;
        if (tapped[half])
        {
            Press(control, GameKey(control), sink);
            rearKeepMs_[half] = nowMs;
        }
        if (!heldKey_[control])
            continue;
        if (!active)
            ReleaseHolder(control, sink);
        else if (heldKey_[kStickUp] || heldKey_[kLeftShoulder])
            rearKeepMs_[half] = nowMs;
        else if (nowMs - rearKeepMs_[half] >= kRearKeepMs)
            ReleaseHolder(control, sink);
    }
}

void Mapper::UpdateSticks(const PadState &pad, float seconds, const Settings &settings, KeySink &sink)
{
    Stick left = ReadStick(pad.lx, pad.ly, settings.stickDeadzone);
    const Stick right = ReadStick(pad.rx, pad.ry, settings.stickDeadzone);
    if (stickWait_)
    {
        if (Centred(left))
            stickWait_ = false;
        left = {0.0f, 0.0f};
    }

    if (mode_ == Mode::Menu)
    {
        // A finger on the screen has the cursor while it is there.
        if (!Centred(left) && !touchHeld_)
        {
            const Stick move = Curved(left, kCursorCurve);
            cursorX_ = Clamp(cursorX_ + move.x * settings.cursorSpeed * seconds, 0.0f,
                             static_cast<float>(settings.screenWidth - 1));
            cursorY_ = Clamp(cursorY_ + move.y * settings.cursorSpeed * seconds, 0.0f,
                             static_cast<float>(settings.screenHeight - 1));
            cursorHidden_ = false;
        }
        // The wheel: a list scrolls under the right stick.
        if (right.y == 0.0f)
            scroll_ = 0.0f;
        scroll_ += right.y * settings.scrollRate * seconds;
        for (; scroll_ >= 1.0f; scroll_ -= 1.0f)
        {
            sink.Key(kKeyWheelDown, true);
            sink.Key(kKeyWheelDown, false);
        }
        for (; scroll_ <= -1.0f; scroll_ += 1.0f)
        {
            sink.Key(kKeyWheelUp, true);
            sink.Key(kKeyWheelUp, false);
        }
        return;
    }

    // The left stick is four keys: the engine moves by keys alone.
    const float threshold = Clamp(settings.moveThreshold, 0.05f, 0.95f);
    const float amount[4] = {-left.y, left.y, -left.x, left.x};
    for (int i = 0; i < 4; ++i)
    {
        const Control control = static_cast<Control>(kStickUp + i);
        const float limit = heldKey_[control] ? threshold - kMoveHysteresis : threshold;
        SetHeld(control, amount[i] > limit, sink);
    }

    if (mode_ == Mode::Game)
    {
        const Stick look = Curved(right, settings.lookCurve);
        lookX_ = Clamp(lookX_ + look.x * settings.lookSpeed * seconds, -kMaxLook, kMaxLook);
        lookY_ = Clamp(lookY_ + look.y * settings.lookSpeed * settings.lookPitchRatio * seconds, -kMaxLook, kMaxLook);
    }
}

void Mapper::UpdateClick(KeySink &sink)
{
    if (click_ == Click::ReleaseOwed || (click_ == Click::Down && !touchHeld_))
    {
        ReleaseHolder(kTouchClick, sink);
        click_ = Click::Idle;
    }
    else if (click_ == Click::PressOwed && !cursorDirty_)
    {
        Press(kTouchClick, kKeyMouse1, sink);
        click_ = touchHeld_ ? Click::Down : Click::ReleaseOwed;
    }
}

Motion Mapper::Take()
{
    Motion motion;
    motion.x = cursorHidden_ ? -1 : static_cast<int>(cursorX_ + 0.5f);
    motion.y = static_cast<int>(cursorY_ + 0.5f);
    // Whole counts go; the part of a count stays for the next frame, so a slow
    // turn is not rounded away.
    motion.dx = static_cast<int>(lookX_);
    motion.dy = static_cast<int>(lookY_);
    lookX_ -= static_cast<float>(motion.dx);
    lookY_ -= static_cast<float>(motion.dy);
    cursorDirty_ = false;
    return motion;
}

void Mapper::SetCursor(int x, int y, const Settings &settings)
{
    cursorX_ = Clamp(static_cast<float>(x), 0.0f, static_cast<float>(settings.screenWidth - 1));
    cursorY_ = Clamp(static_cast<float>(y), 0.0f, static_cast<float>(settings.screenHeight - 1));
}

void Mapper::Release(KeySink &sink)
{
    // lastButtons_ stays: a button still down presses nothing until it has
    // been let go.
    for (int holder = 0; holder < kHolderCount; ++holder)
        ReleaseHolder(holder, sink);
    for (int i = 0; i < fingerCount_; ++i)
        fingers_[i].role = kFingerIdle;
    for (int i = 0; i < rearFingerCount_; ++i)
        rearFingers_[i].tap = false;
    click_ = Click::Idle;
    touchHeld_ = false;
    cursorDirty_ = false;
    lookX_ = lookY_ = 0.0f;
    scroll_ = 0.0f;
    stickWait_ = true;
}

void Mapper::Rest(const PadState &pad, uint32_t nowMs, KeySink &sink)
{
    Release(sink);
    lastMs_ = nowMs;
    lastButtons_ = pad.buttons & ((1u << kButtonCount) - 1);
    fingerCount_ = static_cast<uint8_t>(std::min<int>(pad.frontCount, kMaxFrontTouches));
    for (int i = 0; i < fingerCount_; ++i)
        fingers_[i] = {pad.front[i].id, kFingerIdle};
    rearFingerCount_ = static_cast<uint8_t>(std::min<int>(pad.rearCount, kMaxRearTouches));
    for (int i = 0; i < rearFingerCount_; ++i)
        rearFingers_[i] = {pad.rear[i].id, false, false, nowMs};
}

} // namespace kisak::vita_input
