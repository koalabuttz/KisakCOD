#pragma once

// The PS Vita client's controls: what each button, stick and touch means to
// the engine.
//
// The multiplayer engine takes keys and a mouse, nothing else: it has no
// gamepad path. So the Vita's controls become engine keys, which the game's
// own `bind` command ties to actions, and mouse motion:
//
//   in the game (the engine has the mouse for looking)
//     * every button, the four directions of the left stick, the rear touch
//       pad's two halves and four areas of the screen are keys of their own
//       (AUX1 to AUX16, the keypad arrows and the keypad's 5), so they can be
//       bound and rebound like any PC key;
//     * the rear pad's keys are pressed by a tap, since the fingers that hold
//       the console rest on it, and stay down while the left stick is pushed
//       forward or L is held: by default sprint, as the console game's click
//       of the stick, and a held breath for as long as the sight is up;
//     * the right stick is the mouse: its motion goes through the engine's
//       mouse path, with the game's own sensitivity, pitch inversion and
//       aim-down-sight slowdown.
//   in a menu (a menu has the mouse)
//     * the left stick and a finger on the screen move the menu's cursor, and
//       a touch clicks where it lands;
//     * the d-pad moves the menu's focus with the arrow keys, and hides the
//       cursor until the stick or a finger moves it again;
//     * Cross clicks at the cursor, or accepts the focused item while the
//       cursor is hidden; Circle and Start are Escape; the right stick scrolls.
//   while the controls menu waits for a key to bind
//     * every control sends its game key, so any of them can be given to any
//       action; Start stays Escape, which cancels.
//
// When the mouse changes hands, what the sticks and the touch panels hold is
// let go, and a finger already on the screen does nothing more until it
// lifts: the touch that closed a menu must not throw a grenade. A button keeps
// the key it pressed until it is released.
//
// This file is the mapping alone: no engine call and no Vita call. The glue in
// vita_client_input.cpp reads the hardware, feeds Update, and hands the keys
// and the motion to the engine. scripts/tests/test_vita_input.py runs the same
// code on the host.

#include <cstdint>

namespace kisak::vita_input {

// Engine key numbers (src/ui/keycodes.h). The glue checks they match.
enum : int
{
    kKeyEnter = 0x0D,
    kKeyEscape = 0x1B,
    kKeyUpArrow = 0x9A,
    kKeyDownArrow = 0x9B,
    kKeyLeftArrow = 0x9C,
    kKeyRightArrow = 0x9D,
    kKeyKpUpArrow = 0xB7,
    kKeyKpLeftArrow = 0xB9,
    kKeyKp5 = 0xBA,
    kKeyKpRightArrow = 0xBB,
    kKeyKpDownArrow = 0xBD,
    kKeyMouse1 = 0xC8,
    kKeyWheelDown = 0xCD,
    kKeyWheelUp = 0xCE,
    kKeyAux1 = 0xCF, // AUXn is kKeyAux1 + n - 1, up to AUX16
};

// Everything that acts as a key.
enum Control : int
{
    kCross,
    kCircle,
    kSquare,
    kTriangle,
    kLeftShoulder,
    kRightShoulder,
    kSelect,
    kStart,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kButtonCount, // the controls above are buttons: bits of PadState::buttons
    kStickUp = kButtonCount,
    kStickDown,
    kStickLeft,
    kStickRight,
    kRearLeft,
    kRearRight,
    kFrontLeft,
    kFrontRightTop,
    kFrontRightBottom,
    kFrontTopMiddle,
    kControlCount,
};

// The default layout has grown since the first one. A profile remembers which
// layout it was last given, so a control that came later gets its default
// action there too, once, without touching what the player has rebound.
inline constexpr int kFirstLayout = 1;
inline constexpr int kCurrentLayout = 2; // 2: the screen's top middle (chat)

// The key a control sends while the engine has the mouse, or while the
// controls menu waits for a key to bind. Start is Escape everywhere.
int GameKey(Control control);
// A short name for logs and the guide: "Cross", "Rear touch (left)".
const char *ControlName(Control control);
// The command a control's game key is bound to when nothing else is.
const char *DefaultBinding(Control control);
// The layout (kFirstLayout to kCurrentLayout) the control's default came with.
int LayoutOf(Control control);

struct Touch
{
    int16_t x, y; // screen pixels, both panels (the rear pad scaled to match)
    uint8_t id;   // the panel's number for this finger, the same until it lifts
};

enum : int
{
    kMaxFrontTouches = 6,
    kMaxRearTouches = 4,
};

// One reading of the hardware.
struct PadState
{
    uint32_t buttons = 0;                         // 1 << Control, for the buttons
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128; // 0 left or up, 255 right or down
    uint8_t frontCount = 0, rearCount = 0;
    Touch front[kMaxFrontTouches] = {};
    Touch rear[kMaxRearTouches] = {};
};

struct Settings
{
    int screenWidth = 960;
    int screenHeight = 544;
    float stickDeadzone = 0.2f;   // of full deflection, both sticks
    float moveThreshold = 0.4f;   // left stick, past the dead zone: a direction is held
    float lookSpeed = 1400.0f;    // mouse counts a second at full deflection, sideways
    float lookPitchRatio = 0.65f; // up and down, as a part of lookSpeed
    float lookCurve = 2.0f;       // response: deflection to this power
    float cursorSpeed = 700.0f;   // menu cursor, pixels a second at full deflection
    float scrollRate = 12.0f;     // menu wheel steps a second at full deflection
    bool rearTouch = true;        // the rear pad's two keys, by a tap
    bool frontTouch = true;       // the screen's four keys in the game
};

// Receives the keys, in the order the engine must see them.
class KeySink
{
public:
    virtual void Key(int key, bool down) = 0;

protected:
    ~KeySink() = default;
};

struct Motion
{
    int x, y;   // the menu cursor, in screen pixels; x is -1 while it is hidden
    int dx, dy; // look motion since the last Take, in mouse counts
};

class Mapper
{
public:
    // One reading. `nowMs` is the engine's clock; it may wrap. `menu` is the
    // engine's last answer to the mouse hand-over: true while a menu has the
    // mouse, false while the game looks with it. `bindPending` is true while
    // the controls menu waits for a key to bind.
    void Update(const PadState &pad, uint32_t nowMs, bool menu, bool bindPending, const Settings &settings, KeySink &sink);

    // The frame's hand-over to the engine's mouse path. A click from a touch
    // is held back until its position has gone through here, so the engine
    // sees where before it sees the press.
    Motion Take();

    // The engine moved the cursor.
    void SetCursor(int x, int y, const Settings &settings);

    // Lets go of everything held: the window lost focus or is closing.
    void Release(KeySink &sink);

    bool CursorHidden() const { return cursorHidden_; }
    bool Held(Control control) const { return heldKey_[control] != 0; }

private:
    enum class Mode : uint8_t
    {
        Game, // the game looks with the mouse
        Menu, // a menu has the mouse
        Bind, // the controls menu waits for a key
    };
    enum class Click : uint8_t
    {
        Idle,
        PressOwed,   // a touch landed; the press waits for the hand-over
        Down,        // the press is sent
        ReleaseOwed, // the finger left before the press went out: release next
    };
    enum : int
    {
        kTouchClick = kControlCount, // the screen's click in a menu holds a key too
        kHolderCount,
    };
    enum : int8_t
    {
        kFingerIdle = -2,    // does nothing until it lifts
        kFingerPointer = -1, // moves the menu's cursor
        // 0 and up: the Control of the area of the screen it landed in
    };
    struct Finger
    {
        uint8_t id;
        int8_t role;
    };
    struct RearFinger
    {
        uint8_t id;
        bool right;       // the half it landed on
        bool tap;         // lifting soon enough presses that half's key
        uint32_t sinceMs; // when it landed
    };

    int KeyFor(Control control) const;
    void Press(int holder, int key, KeySink &sink);
    void ReleaseHolder(int holder, KeySink &sink);
    void SetHeld(Control control, bool held, KeySink &sink);
    void EnterMode(Mode mode, KeySink &sink);
    void UpdateButtons(const PadState &pad, uint32_t nowMs, KeySink &sink);
    void UpdateFront(const PadState &pad, const Settings &settings, KeySink &sink);
    void UpdateRear(const PadState &pad, uint32_t nowMs, const Settings &settings, KeySink &sink);
    void UpdateSticks(const PadState &pad, float seconds, const Settings &settings, KeySink &sink);
    void UpdateClick(KeySink &sink);

    int heldKey_[kHolderCount] = {};       // the key each control has down, or 0
    uint32_t repeatAt_[kButtonCount] = {}; // when a held d-pad arrow repeats next
    uint32_t lastButtons_ = 0;
    uint32_t lastMs_ = 0;
    Finger fingers_[kMaxFrontTouches] = {}; // the fingers on the screen at the last reading
    uint8_t fingerCount_ = 0;
    RearFinger rearFingers_[kMaxRearTouches] = {}; // and on the rear pad
    uint8_t rearFingerCount_ = 0;
    uint32_t rearKeepMs_[2] = {}; // when each half's key was last wanted: its tap, the stick forward or L held
    bool started_ = false;
    Mode mode_ = Mode::Menu;
    bool cursorHidden_ = false;
    bool cursorDirty_ = false; // moved by a touch and not handed over yet
    bool touchHeld_ = false;   // the finger that moves the cursor is down
    bool stickWait_ = false;   // the left stick does nothing until it is let go
    Click click_ = Click::Idle;
    float cursorX_ = 480.0f, cursorY_ = 272.0f;
    float lookX_ = 0.0f, lookY_ = 0.0f; // look motion not taken yet
    float scroll_ = 0.0f;               // part of a wheel step not sent yet
};

} // namespace kisak::vita_input
