#pragma once

// A game controller for the desktop client: what each button, stick and
// trigger means to the engine.
//
// The multiplayer engine takes keys and a mouse, nothing else. The PS Vita
// client already turns a pad into those (src/vita/client/input/
// vita_input_map.h): buttons and the left stick's four directions are engine
// keys of their own that `bind` ties to actions, the right stick is mouse
// motion, and in a menu the left stick moves the cursor, the d-pad moves the
// focus, A clicks and B is Escape. This file puts a full-size controller on
// that same mapping, unchanged, and adds what a Vita does not have:
//
//   controller          Vita control the mapping sees   key      default
//   A / B / X / Y       Cross / Circle / Square / Tri.  AUX1-4   jump, stance, use/reload, next weapon
//   left trigger        L                               AUX5     aim down the sight
//   right trigger       R                               AUX6     fire
//   Back (View)         Select                          AUX7     scores
//   Start (Menu)        Start                           Escape   the menu
//   d-pad               d-pad                           keypad arrows   action slots 1 to 4
//   left stick          left stick                      AUX9-12  move
//   right stick         right stick                     mouse    look
//   left bumper         (the screen's left third)       AUX13    special grenade
//   right bumper        (the screen's top right)        AUX14    frag grenade
//   right stick click   (the screen's bottom right)     AUX15    melee
//   left stick click    (a tap on the rear touch pad)   AUX8     sprint / hold breath
//
// The last four are the keys the Vita's touch areas send, so one profile's
// bindings mean the same on both. Here they are plain buttons, with one
// exception that follows the console game: a click of the left stick starts
// the sprint, and the key stays down for as long as the stick is pushed
// forward, so the stick need not be held in.
//
// Like the Vita mapping, this is the mapping alone: no engine call and no SDL
// call. mp_client_gamepad.cpp reads the controller and hands the keys and the
// motion to the engine; scripts/tests/test_linux_gamepad.py runs this file on
// the host.

#include <vita/client/input/vita_input_map.h>

#include <cstdint>

namespace kisak::gamepad {

enum Button : int
{
    kA,
    kB,
    kX,
    kY,
    kBack,
    kStart,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kLeftBumper,
    kRightBumper,
    kLeftStickClick,
    kRightStickClick,
    kButtonCount,
};

// Everything that sends a key: the buttons, the two triggers and the left
// stick's four directions.
enum Control : int
{
    kLeftTrigger = kButtonCount,
    kRightTrigger,
    kStickUp,
    kStickDown,
    kStickLeft,
    kStickRight,
    kControlCount,
};

// The key a control sends in the game, its name for logs and the guide, and
// the command its key is bound to when nothing else is (nullptr for Start,
// which is Escape in the engine itself).
int GameKey(int control);
const char *ControlName(int control);
const char *DefaultBinding(int control);

// One reading of the controller, in SDL's units.
struct Reading
{
    uint32_t buttons = 0;       // 1 << Button
    int16_t lx = 0, ly = 0;     // -32768 left or up, 32767 right or down
    int16_t rx = 0, ry = 0;
    int16_t leftTrigger = 0;    // 0 released, 32767 fully pulled
    int16_t rightTrigger = 0;
};

struct Settings
{
    vita_input::Settings pad;      // sticks, look and cursor speeds, the screen's size
    float triggerThreshold = 0.3f; // of full travel: the trigger's key goes down
};

class Mapper
{
public:
    // One reading. The arguments are the Vita mapping's: `nowMs` is the
    // engine's clock, `menu` is true while a menu has the mouse, `bindPending`
    // while the controls menu waits for a key.
    void Update(const Reading &reading, uint32_t nowMs, bool menu, bool bindPending, const Settings &settings,
                vita_input::KeySink &sink);

    // The frame's hand-over to the engine's mouse path, shared with the real
    // mouse. In: where the mouse is and how far it moved this frame. Out: what
    // the engine should be told. The look motion of the two adds up. The
    // cursor belongs to whichever moved last: while the mouse rests, the
    // stick's cursor (x is -1 while the d-pad has hidden it) stands in for the
    // mouse's position, and the first motion of the mouse takes it back and
    // moves the stick's cursor to where the mouse is.
    vita_input::Motion Frame(int mouseX, int mouseY, int mouseDx, int mouseDy, const Settings &settings);
    // The engine moved the cursor (IN_SetCursorPos).
    void SetCursor(int x, int y, const Settings &settings);
    // Lets go of everything held: the controller went away, or the window
    // lost focus.
    void Release(vita_input::KeySink &sink);

    bool CursorHidden() const { return pad_.CursorHidden(); }
    // The pad's cursor, not the mouse's position, is what the engine is told.
    bool PadHasCursor() const { return padCursor_; }
    // Something on the controller is pressed, pulled or pushed.
    bool Active() const { return active_; }

private:
    enum : int
    {
        kExtraCount = 4, // the bumpers and the two stick clicks
    };

    vita_input::Mapper pad_;
    bool extraDown_[kExtraCount] = {}; // the key is down
    bool sprintLatched_ = false;       // the left stick's click, kept by pushing forward
    bool leftTrigger_ = false, rightTrigger_ = false;
    bool active_ = false;
    bool mouseSeen_ = false;  // lastMouse_ is a reading
    bool padCursor_ = false;  // the stick or the d-pad moved the cursor last
    int lastMouseX_ = 0, lastMouseY_ = 0;
    int cursorX_ = 0, cursorY_ = 0; // where the pad's cursor was left, to see it move
};

} // namespace kisak::gamepad
