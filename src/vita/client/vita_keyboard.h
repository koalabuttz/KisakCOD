#pragma once

// The PS Vita's on-screen keyboard (the system's IME dialog) for the
// multiplayer client. See docs/vita-client.md, "Controls".
//
// The system draws the keyboard over the game's frames, so the dialog belongs
// to the thread that draws: it is opened, drawn, asked for its state and
// closed there, from the display's hook before each flip (gxm_display.h).
// The game's input runs on the main thread. It asks for a keyboard and takes
// the result through the calls below, which any thread may make.

#include <cstdint>

inline constexpr int kVitaKeyboardMaxText = 255;
inline constexpr int kVitaKeyboardMaxTitle = 32;

enum class VitaKeyboardKind : uint8_t
{
    Text,    // letters, digits and signs
    Number,  // digits
    Decimal, // digits and a decimal sign
};

struct VitaKeyboardRequest
{
    // UTF-16, terminated.
    uint16_t title[kVitaKeyboardMaxTitle + 1];
    uint16_t initial[kVitaKeyboardMaxText + 1];
    int maxLength; // characters, 1 to kVitaKeyboardMaxText
    VitaKeyboardKind kind;
};

enum class VitaKeyboardResult : uint8_t
{
    None,      // nothing was asked for
    Waiting,   // asked for or on the screen
    Entered,   // closed with Enter; the text is handed over
    Cancelled, // closed without
    Failed,    // the system did not open it; the log says why
};

// Asks for the keyboard at the next frame drawn. False: one is already asked
// for or up, or the system refused the last one (it is not asked again).
bool Vita_AskKeyboard(const VitaKeyboardRequest &request);
// The state of what was asked for. Entered, Cancelled and Failed are handed
// over once; `text` (kVitaKeyboardMaxText + 1 values) gets Entered's text.
VitaKeyboardResult Vita_TakeKeyboard(uint16_t *text);
// Closes a keyboard that is asked for or up, as if it was cancelled: the
// client is quitting, or the game no longer waits for text.
void Vita_AbortKeyboard();
