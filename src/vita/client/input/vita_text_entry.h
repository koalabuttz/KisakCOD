#pragma once

// Text entry for the PS Vita client: when the game waits for typed text, the
// system's on-screen keyboard opens, and what was typed goes to the game as
// the characters and keys a PC keyboard would send (docs/vita-client.md,
// "Controls").
//
// This is the part that decides: when the keyboard opens, what the game gets
// when it closes, and how fast. It makes no engine call and no Vita call
// (vita_client_input.cpp does both, through Sink), so the host tests build it:
// scripts/tests/test_vita_input.py.

#include <cstdint>

namespace kisak::vita_text {

// The engine's text is one byte a character: ASCII and Latin-1.
inline constexpr int kMaxText = 255; // a menu field holds no more
// Characters handed over in one frame. The engine's event queue holds 256
// events and empties every frame.
inline constexpr int kCharsPerFrame = 32;

inline constexpr int kKeyEnter = 13;
inline constexpr int kKeyEscape = 27;

// The keyboard's UTF-16 as engine characters, terminated; returns how many.
// Control characters (a line break among them) and characters the engine has
// no byte for are left out. `out` holds at least `outSize` bytes, 1 or more.
int FromKeyboard(const uint16_t *text, char *out, int outSize);
// Engine characters as the keyboard's UTF-16, terminated; returns how many.
int ToKeyboard(const char *text, uint16_t *out, int outSize);

enum class Target : uint8_t
{
    None,
    MenuField, // a text field of a menu being edited
    Chat,      // the say / say_team line
    Console,   // a console command, asked for with AskForCommand
};

// What the game waits for text in, as seen this frame. `field` tells one
// field from another; the same pointer is the same field.
struct Waiting
{
    Target target = Target::None;
    const void *field = nullptr;
};

class Sink
{
public:
    // Opens the keyboard for what the game waits for. False: it did not open.
    virtual bool OpenKeyboard(Target target) = 0;
    // Empties the field the keyboard was opened for.
    virtual void ClearField(Target target) = 0;
    virtual void Char(int character) = 0;
    virtual void Key(int key) = 0; // pressed and let go
    virtual void Command(const char *text) = 0;

protected:
    ~Sink() = default;
};

class Entry
{
public:
    // Once a frame, on the thread that owns the game's input.
    void Frame(const Waiting &waiting, Sink &sink);
    // The keyboard closed: with Enter (`accepted`) and its text, or without.
    void KeyboardClosed(bool accepted, const uint16_t *text);
    // Opens the keyboard for a console command at the next Frame.
    void AskForCommand();
    // The keyboard is on the screen: the controls belong to it.
    bool KeyboardUp() const { return state_ == State::Up; }
    // The keyboard is on the screen for a field or a chat line the game no
    // longer waits in (a menu closed under it, the match ended): it has
    // nothing to type into and may be closed. A console command always has
    // somewhere to go.
    bool Orphaned(const Waiting &waiting) const;
    // The keyboard did not open the last time it was asked to.
    bool Failed() const { return failed_; }

private:
    bool Feed(const Waiting &waiting, Sink &sink);

    enum class State : uint8_t
    {
        Idle,
        Up,      // the keyboard is open
        Feeding, // its text is going to the game
    };
    State state_ = State::Idle;
    Target target_ = Target::None; // what the keyboard was opened for
    const void *field_ = nullptr;
    Waiting handled_;              // the wait the keyboard was last opened for
    bool askedForCommand_ = false;
    bool failed_ = false;
    bool accepted_ = false;
    bool cleared_ = false;
    char text_[kMaxText + 1] = {};
    int length_ = 0;
    int fed_ = 0;
};

} // namespace kisak::vita_text
