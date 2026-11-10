// The PS Vita multiplayer client's window procedure and input: the parts of
// src/win32/win_input.cpp and src/win32/win_wndproc.cpp the engine calls (the
// same set src/linux/client/mp_client_input.cpp provides on Linux).
//
// The window procedure sets g_wv.hWnd and makes the app active: the screen is
// always in front.
//
// The controls (docs/vita-client.md, "Controls"). What each button, stick and
// touch means is input/vita_input_map.cpp, which the host tests run. This file
// reads the pad and the two touch panels, feeds the mapping, and hands what
// comes out to the engine:
//   * Sys_PumpClientWindow (Win_GetEvent calls it whenever the event queue is
//     empty) takes a reading and queues the keys as SE_KEY events, the way
//     WM_KEYDOWN does on Windows;
//   * IN_Frame hands the menu cursor or the look motion to CL_MouseEvent, as
//     IN_MouseMove does on Windows. Its answer says who has the mouse, which
//     is how the next reading is mapped;
//   * IN_SetCursorPos moves the cursor the mapping keeps.
// The Vita's controls are engine keys of their own (AUX1 to AUX16, the keypad
// arrows and the keypad's 5), so `bind` and the controls menu work on them. A
// profile has no bindings for them at first; the default layout is set once,
// while none of the AUX keys is bound to anything (vita_defaultBinds,
// vita_binddefaults). A profile that got an earlier default layout is given
// the controls that came since, once (vita_layout). The controls menu and the
// game's hints show the controls by name, "Square" for AUX3 (NameControls).
//
// START + SELECT still quits, from the main loop (Vita_QuitRequested): both
// readers peek at the pad, so neither takes the other's samples.
//
// Text: when a menu's text field is being edited or the chat line is open, the
// system's on-screen keyboard comes up (vita_keyboard.h), and what was typed
// goes to the game as SE_CHAR events and an Enter, the way WM_CHAR does on
// Windows. When it opens and what the game gets is input/vita_text_entry.cpp,
// which the host tests run too. `vita_typecommand` opens it for a console
// command. If the system refuses the keyboard, the one field a new install
// cannot get past, the new profile's name, is filled in (OfferProfileName).

#include "vita_client.h"

#include "vita_keyboard.h"

#include "input/vita_input_map.h"
#include "input/vita_text_entry.h"

#include <linux/client/mp_client_input.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/com_playerprofile.h>
#include <qcommon/threads.h>
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <sound/snd_public.h>
#include <ui/keycodes.h>
#include <ui/ui_input_core.h>
#include <ui/ui_shared.h>
#include <win32/win_input.h>
#include <win32/win_local.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <cstdio>
#include <cstring>

namespace vi = kisak::vita_input;
namespace vt = kisak::vita_text;

// The mapping names the engine's keys by number, to stay free of the engine.
constexpr bool SameKey(int mapped, int engine)
{
    return mapped == engine;
}
static_assert(SameKey(vi::kKeyEnter, K_ENTER) && SameKey(vi::kKeyEscape, K_ESCAPE));
static_assert(SameKey(vi::kKeyUpArrow, K_UPARROW) && SameKey(vi::kKeyDownArrow, K_DOWNARROW));
static_assert(SameKey(vi::kKeyLeftArrow, K_LEFTARROW) && SameKey(vi::kKeyRightArrow, K_RIGHTARROW));
static_assert(SameKey(vi::kKeyKpUpArrow, K_KP_UPARROW) && SameKey(vi::kKeyKpDownArrow, K_KP_DOWNARROW));
static_assert(SameKey(vi::kKeyKpLeftArrow, K_KP_LEFTARROW) && SameKey(vi::kKeyKpRightArrow, K_KP_RIGHTARROW));
static_assert(SameKey(vi::kKeyKp5, K_KP_5));
static_assert(SameKey(vi::kKeyMouse1, K_MOUSE1) && SameKey(vi::kKeyWheelDown, K_MWHEELDOWN));
static_assert(SameKey(vi::kKeyWheelUp, K_MWHEELUP) && SameKey(vi::kKeyAux1, K_AUX1));
static_assert(SameKey(vi::kKeyAux1 + 15, K_AUX16));
static_assert(SameKey(vi::kMaxFrontTouches, 6) && SameKey(vi::kMaxRearTouches, 4) && SCE_TOUCH_MAX_REPORT >= 6);
static_assert(SameKey(vt::kKeyEnter, K_ENTER) && SameKey(vt::kKeyEscape, K_ESCAPE));
static_assert(SameKey(vt::kMaxText, kVitaKeyboardMaxText));

// The names the engine shows for keys (cl_keys.cpp); it has no header for
// them. The names `bind` and the config files use are another table.
extern keyname_t keynames_localized[96];

namespace {

// The pad is sampled once a display frame; reading more often than this only
// costs system calls. Win_GetEvent asks several times a frame.
constexpr uint32_t kPollEveryMs = 4;
constexpr uint32_t kHousekeepingEveryMs = 1000;
// The name the "create profile" box offers when no keyboard can type one.
constexpr char kProfileName[] = "Vita";
// The longest chat line the keyboard takes.
constexpr int kChatLength = 120;
constexpr uint32_t kAnswerAfterMs = 500; // vita_keyboardAnswer: how long its keyboard stays "up"
// The menus' text field types (Item_IsTextField) that take digits only.
constexpr int kItemNumericField = 9;
constexpr int kItemDecimalField = 17;

struct Button
{
    unsigned int bit;
    vi::Control control;
};
constexpr Button kButtons[] = {
    {SCE_CTRL_CROSS, vi::kCross},       {SCE_CTRL_CIRCLE, vi::kCircle},     {SCE_CTRL_SQUARE, vi::kSquare},
    {SCE_CTRL_TRIANGLE, vi::kTriangle}, {SCE_CTRL_LTRIGGER, vi::kLeftShoulder}, {SCE_CTRL_RTRIGGER, vi::kRightShoulder},
    {SCE_CTRL_SELECT, vi::kSelect},     {SCE_CTRL_START, vi::kStart},       {SCE_CTRL_UP, vi::kDpadUp},
    {SCE_CTRL_DOWN, vi::kDpadDown},     {SCE_CTRL_LEFT, vi::kDpadLeft},     {SCE_CTRL_RIGHT, vi::kDpadRight},
};

// The part of a touch panel that stands for the screen, in the panel's own
// numbers.
struct Panel
{
    int minX, minY, maxX, maxY;
    bool known; // read from the console, not assumed
};

const dvar_t *in_mouse;
const dvar_t *vita_stickDeadzone;
const dvar_t *vita_lookSpeed;
const dvar_t *vita_lookPitchRatio;
const dvar_t *vita_lookCurve;
const dvar_t *vita_cursorSpeed;
const dvar_t *vita_frontTouch;
const dvar_t *vita_rearTouch;
const dvar_t *vita_defaultBinds;
const dvar_t *vita_layout;
const dvar_t *vita_buttonNames;
const dvar_t *vita_inputDebug;
const dvar_t *vita_keyboard;
const dvar_t *vita_keyboardAnswer;
cmd_function_s s_bindDefaultsCommand;
cmd_function_s s_typeCommandCommand;
bool s_commandAdded;

qboolean in_appactive;
bool s_installed;
bool s_look; // CL_MouseEvent's last answer: the game looks with the mouse
bool s_polled;
uint32_t s_lastPollMs;
uint32_t s_lastHousekeepingMs;
uint32_t s_eventTime;
// The front panel has twice the screen's resolution; the rear pad's active
// area is shorter than the screen. Both are asked for at start-up.
Panel s_front = {0, 0, 1919, 1087, false};
Panel s_rear = {0, 108, 1919, 889, false};
vi::Mapper s_mapper;
vi::Settings s_settings;
vt::Entry s_text;
bool s_keyboardRefused; // the system did not open the keyboard
bool s_restOnce;        // the keyboard closed: the next reading still does nothing
// vita_keyboardAnswer stands in for the keyboard that was asked for.
bool s_answering;
bool s_answerDropped;
uint32_t s_answerSinceMs;
int s_answerLength;
// Fingers seen at the last reading and when each landed, for vita_inputDebug.
uint8_t s_debugIds[2][SCE_TOUCH_MAX_REPORT];
uint32_t s_debugSince[2][SCE_TOUCH_MAX_REPORT];
int s_debugCount[2];

bool Debugging()
{
    return vita_inputDebug && vita_inputDebug->current.enabled;
}

class EngineKeys final : public vi::KeySink
{
public:
    void Key(int key, bool down) override
    {
        if (Debugging())
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: %s %s\n", Key_KeynumToString(key, 0), down ? "down" : "up");
        Sys_QueEvent(s_eventTime, SE_KEY, key, down, 0, nullptr);
    }
};
EngineKeys s_keys;

int16_t ToScreen(int value, int low, int high, int size)
{
    if (high <= low)
        return 0;
    const int scaled = (value - low) * size / (high - low + 1);
    return static_cast<int16_t>(scaled < 0 ? 0 : scaled >= size ? size - 1 : scaled);
}

void ReadPanelInfo(SceUInt32 port, Panel *panel, bool display)
{
    SceTouchPanelInfo info;
    memset(&info, 0, sizeof(info));
    if (sceTouchGetPanelInfo(port, &info) < 0)
        return;
    // The screen's part of the front panel; all of the rear pad that senses.
    const Panel read = display ? Panel{info.minDispX, info.minDispY, info.maxDispX, info.maxDispY, true}
                               : Panel{info.minAaX, info.minAaY, info.maxAaX, info.maxAaY, true};
    if (read.maxX > read.minX && read.maxY > read.minY)
        *panel = read;
}

void ReadPanel(SceUInt32 port, const Panel &panel, uint32_t now, vi::Touch *touches, uint8_t *count, int most)
{
    // A read that fails, or hands back nothing, leaves no reports: no fingers.
    SceTouchData data;
    memset(&data, 0, sizeof(data));
    *count = 0;
    if (sceTouchPeek(port, &data, 1) < 0)
        data.reportNum = 0;
    int reports = static_cast<int>(data.reportNum);
    if (reports > SCE_TOUCH_MAX_REPORT)
        reports = SCE_TOUCH_MAX_REPORT;
    const bool debug = Debugging();
    const char *const name = port == SCE_TOUCH_PORT_FRONT ? "front" : "rear";
    uint32_t since[SCE_TOUCH_MAX_REPORT];
    for (int i = 0; i < reports; ++i)
    {
        const SceTouchReport &report = data.report[i];
        const vi::Touch touch = {ToScreen(report.x, panel.minX, panel.maxX, kVitaScreenWidth),
                                 ToScreen(report.y, panel.minY, panel.maxY, kVitaScreenHeight), report.id};
        // A finger landing, once: where the panel says it is and where that is
        // on the screen. This is how a console run checks the rear pad's left
        // and right.
        bool seen = false;
        since[i] = now;
        for (int j = 0; j < s_debugCount[port]; ++j)
        {
            if (s_debugIds[port][j] == report.id)
            {
                seen = true;
                since[i] = s_debugSince[port][j];
            }
        }
        if (debug && !seen)
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: %s touch %d at %d,%d on the panel, %d,%d on the screen\n", name,
                       report.id, report.x, report.y, touch.x, touch.y);
        if (*count < most)
            touches[(*count)++] = touch;
    }
    // And leaving, with how long it stayed: a tap on the rear pad is 300 ms at
    // most, and a console run shows what fingers that only hold the console do.
    for (int j = 0; debug && j < s_debugCount[port]; ++j)
    {
        bool there = false;
        for (int i = 0; i < reports; ++i)
            there = there || data.report[i].id == s_debugIds[port][j];
        if (!there)
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: %s touch %d lifted after %u ms\n", name, s_debugIds[port][j],
                       static_cast<unsigned int>(now - s_debugSince[port][j]));
    }
    s_debugCount[port] = reports;
    for (int i = 0; i < reports; ++i)
    {
        s_debugIds[port][i] = data.report[i].id;
        s_debugSince[port][i] = since[i];
    }
}

void ReadSettings()
{
    s_settings.screenWidth = kVitaScreenWidth;
    s_settings.screenHeight = kVitaScreenHeight;
    if (!vita_stickDeadzone)
        return;
    s_settings.stickDeadzone = vita_stickDeadzone->current.value;
    s_settings.lookSpeed = vita_lookSpeed->current.value;
    s_settings.lookPitchRatio = vita_lookPitchRatio->current.value;
    s_settings.lookCurve = vita_lookCurve->current.value;
    s_settings.cursorSpeed = vita_cursorSpeed->current.value;
    s_settings.frontTouch = vita_frontTouch->current.enabled;
    s_settings.rearTouch = vita_rearTouch->current.enabled;
}

// One reading of the console, through the mapping, into the event queue.
void Poll()
{
    // The window is the client being up; the render thread pumps events too
    // while a map loads, and the mapping belongs to the main thread.
    if (!s_installed || !g_wv.hWnd || !in_appactive || !Sys_IsMainThread())
        return;
    const uint32_t now = Sys_Milliseconds();
    if (s_polled && now - s_lastPollMs < kPollEveryMs)
        return;
    s_polled = true;
    s_lastPollMs = now;

    ReadSettings();
    vi::PadState state;
    // A read that fails, or hands back nothing, is a pad at rest: everything
    // held is let go.
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0)
    {
        for (const Button &button : kButtons)
        {
            if (pad.buttons & button.bit)
                state.buttons |= 1u << button.control;
        }
        state.lx = pad.lx;
        state.ly = pad.ly;
        state.rx = pad.rx;
        state.ry = pad.ry;
    }
    ReadPanel(SCE_TOUCH_PORT_FRONT, s_front, now, state.front, &state.frontCount, vi::kMaxFrontTouches);
    ReadPanel(SCE_TOUCH_PORT_BACK, s_rear, now, state.rear, &state.rearCount, vi::kMaxRearTouches);

    s_eventTime = now;
    // The on-screen keyboard has the controls while it is up. The touch or
    // the button that closed it is still down in the reading after: that one
    // does nothing either.
    if (s_text.KeyboardUp() || s_restOnce)
    {
        s_restOnce = false;
        s_mapper.Rest(state, now, s_keys);
        return;
    }
    s_mapper.Update(state, now, !s_look, Display_KeyBindPending() != 0, s_settings, s_keys);
}

void ReleaseAll()
{
    s_eventTime = Sys_Milliseconds();
    s_mapper.Release(s_keys);
}

bool Unbound(int key)
{
    const char *const binding = Key_GetBinding(0, static_cast<uint32_t>(key));
    return !binding || !*binding;
}

// Gives the Vita's controls their default actions: all of them, or only
// those bound to nothing, and of those only the ones that came after the
// default layout `since`.
int BindDefaults(bool all, int since = 0)
{
    int bound = 0;
    for (int i = 0; i < vi::kControlCount; ++i)
    {
        const vi::Control control = static_cast<vi::Control>(i);
        const char *const command = vi::DefaultBinding(control);
        const int key = vi::GameKey(control);
        if (!command || vi::LayoutOf(control) <= since || (!all && !Unbound(key)))
            continue;
        char binding[64];
        snprintf(binding, sizeof(binding), "%s", command);
        Key_SetBinding(0, key, binding);
        ++bound;
    }
    return bound;
}

void SetLayout(int layout)
{
    if (vita_layout && vita_layout->current.integer != layout)
        Dvar_SetInt(vita_layout, layout);
}

void BindDefaults_f()
{
    Com_Printf(CON_CHANNEL_SYSTEM, "vita input: %d controls set to the default layout\n", BindDefaults(true));
    SetLayout(vi::kCurrentLayout);
}

// A profile made on a PC, a new profile and the controls menu's "reset" all
// start from `unbindall` and the PC's keys, which leave every AUX key bound to
// nothing: the Vita's layout is set then. One control left unbound on purpose
// stays so, as long as another AUX key has a binding.
//
// A profile that has bindings got them from the default layout its vita_layout
// names, the first one if its config does not say (the engine sets every dvar
// back to its default before it reads another profile's config). The controls
// that came with later layouts get their default actions there once, where
// they are bound to nothing, and the profile is marked as having today's.
void CheckDefaultBinds()
{
    if (!vita_defaultBinds || !vita_defaultBinds->current.enabled || Display_KeyBindPending())
        return;
    bool anyBound = false;
    for (int key = K_AUX1; key <= K_AUX16; ++key)
        anyBound = anyBound || !Unbound(key);
    if (!anyBound)
    {
        const int bound = BindDefaults(false);
        SetLayout(vi::kCurrentLayout);
        Com_Printf(CON_CHANNEL_SYSTEM,
                   "vita input: the Vita's controls had no bindings; %d set to the default layout (docs/vita-client.md)\n",
                   bound);
        return;
    }
    const int layout = vita_layout ? vita_layout->current.integer : vi::kCurrentLayout;
    if (layout >= vi::kCurrentLayout)
        return;
    const int bound = BindDefaults(false, layout);
    SetLayout(vi::kCurrentLayout);
    Com_Printf(CON_CHANNEL_SYSTEM,
               "vita input: the profile's controls were default layout %d; %d newer controls set to layout %d's actions\n",
               layout, bound, vi::kCurrentLayout);
}

// The engine's names for the Vita's keys are the PC's: "AUX3", and from the
// game's own texts something like it. The controls menu and the hints on the
// screen ("Press ... to use") take a key's name from keynames_localized and
// show it as it is when the game's texts have no entry under it, so the
// controls' own names go there: "Square". The table `bind` and the configs use
// is not touched, so a profile reads the same with and without.
void NameControls()
{
    static const char *s_engineNames[vi::kControlCount];
    const bool named = vita_buttonNames && vita_buttonNames->current.enabled;
    for (int i = 0; i < vi::kControlCount; ++i)
    {
        const vi::Control control = static_cast<vi::Control>(i);
        const int key = vi::GameKey(control);
        if (key == K_ESCAPE)
            continue; // Start is the engine's Escape, under that name
        for (keyname_t *entry = keynames_localized; entry->name; ++entry)
        {
            if (entry->keynum != key)
                continue;
            if (!s_engineNames[i])
                s_engineNames[i] = entry->name;
            entry->name = named ? vi::ControlName(control) : s_engineNames[i];
            break;
        }
    }
}

bool KeyboardWanted()
{
    return vita_keyboard && vita_keyboard->current.enabled && !s_keyboardRefused;
}

// A run with nobody to type: vita_keyboardAnswer's text is what every keyboard
// asked for hands back, and no keyboard is shown.
bool AnswerSet()
{
    return vita_keyboardAnswer && *vita_keyboardAnswer->current.string;
}

// What the game waits for typed text in.
vt::Waiting TextWaiting()
{
    if (!KeyboardWanted())
        return {};
    if (Key_IsCatcherActive(0, KEYCATCH_MESSAGE))
        return {vt::Target::Chat, &playerKeys[0].chatField};
    if (Key_IsCatcherActive(0, KEYCATCH_UI) && g_editingField && g_editItem && g_editItem->dvar
        && Item_IsTextField(g_editItem) && Item_GetEditFieldDef(g_editItem))
        return {vt::Target::MenuField, g_editItem};
    return {};
}

// The text entry's side of the engine: the keyboard is asked for with what the
// field holds, and what comes back goes into the event queue.
class EngineText final : public vt::Sink
{
public:
    bool OpenKeyboard(vt::Target target) override
    {
        VitaKeyboardRequest request;
        memset(&request, 0, sizeof(request));
        request.maxLength = vt::kMaxText;
        request.kind = VitaKeyboardKind::Text;
        const char *title = "";
        const char *initial = "";
        switch (target)
        {
        case vt::Target::MenuField:
        {
            // TextWaiting saw the field this frame.
            const editFieldDef_s *const edit = Item_GetEditFieldDef(g_editItem);
            if (edit && edit->maxChars > 0 && edit->maxChars < vt::kMaxText)
                request.maxLength = edit->maxChars;
            if (g_editItem->type == kItemNumericField)
                request.kind = VitaKeyboardKind::Number;
            else if (g_editItem->type == kItemDecimalField)
                request.kind = VitaKeyboardKind::Decimal;
            initial = Dvar_GetVariantString(g_editItem->dvar);
            break;
        }
        case vt::Target::Chat:
            title = playerKeys[0].chat_team ? "Say to team" : "Say";
            request.maxLength = kChatLength;
            break;
        case vt::Target::Console:
            title = "Command";
            break;
        default:
            return false;
        }
        vt::ToKeyboard(title, request.title, kVitaKeyboardMaxTitle + 1);
        vt::ToKeyboard(initial, request.initial, kVitaKeyboardMaxText + 1);
        const bool answered = AnswerSet();
        if (answered)
        {
            s_answering = true;
            s_answerDropped = false;
            s_answerSinceMs = s_eventTime;
            s_answerLength = request.maxLength;
        }
        else if (!Vita_AskKeyboard(request))
        {
            return false;
        }
        // Nothing stays held under the keyboard.
        ReleaseAll();
        if (Debugging() || answered)
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: keyboard asked for (%s, %d characters at most)%s\n",
                       target == vt::Target::MenuField ? "a menu field" : target == vt::Target::Chat ? "chat" : "a command",
                       request.maxLength, answered ? "; vita_keyboardAnswer answers, none is shown" : "");
        return true;
    }

    void ClearField(vt::Target target) override
    {
        if (target == vt::Target::Chat)
        {
            Field_Clear(&playerKeys[0].chatField);
        }
        else if (target == vt::Target::MenuField && g_editItem && g_editItem->dvar)
        {
            // As backspace over the whole field does on a PC.
            Dvar_SetFromStringByName(g_editItem->dvar, "");
            g_editItem->cursorPos[0] = 0;
            if (Item_IsTextField(g_editItem))
            {
                if (editFieldDef_s *const edit = Item_GetEditFieldDef(g_editItem))
                    edit->paintOffset = 0;
            }
        }
    }

    void Char(int character) override
    {
        Sys_QueEvent(s_eventTime, SE_CHAR, character, 0, 0, nullptr);
    }

    void Key(int key) override
    {
        Sys_QueEvent(s_eventTime, SE_KEY, key, 1, 0, nullptr);
        Sys_QueEvent(s_eventTime, SE_KEY, key, 0, 0, nullptr);
    }

    void Command(const char *text) override
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "vita input: typed command: %s\n", text);
        Cbuf_AddText(0, text);
        Cbuf_AddText(0, "\n");
    }
};
EngineText s_engineText;

// The keyboard's result: the system's, or after half a second
// vita_keyboardAnswer's, cut to the length the field takes as the keyboard
// would have cut it.
VitaKeyboardResult TakeKeyboard(uint16_t *typed, uint32_t now)
{
    if (!s_answering)
        return Vita_TakeKeyboard(typed);
    if (!s_answerDropped && now - s_answerSinceMs < kAnswerAfterMs)
        return VitaKeyboardResult::Waiting;
    s_answering = false;
    if (s_answerDropped || !AnswerSet())
        return VitaKeyboardResult::Cancelled;
    vt::ToKeyboard(vita_keyboardAnswer->current.string, typed, kVitaKeyboardMaxText + 1);
    if (s_answerLength >= 0 && s_answerLength < kVitaKeyboardMaxText)
        typed[s_answerLength] = 0;
    return VitaKeyboardResult::Entered;
}

// Once a frame: the keyboard's result, if it closed, and what the game waits
// for now.
void TextFrame(uint32_t now)
{
    s_eventTime = now;
    if (s_text.KeyboardUp())
    {
        uint16_t typed[kVitaKeyboardMaxText + 1];
        const bool answered = s_answering;
        const VitaKeyboardResult result = TakeKeyboard(typed, now);
        if (result == VitaKeyboardResult::Waiting)
        {
            // The menu closed under the keyboard, or the match ended: the
            // keyboard goes too. What it held is dropped when it has closed.
            if (s_text.Orphaned(TextWaiting()))
            {
                s_answerDropped = s_answering;
                if (!s_answering)
                    Vita_AbortKeyboard();
            }
            return;
        }
        s_restOnce = true;
        if (result == VitaKeyboardResult::Failed)
        {
            s_keyboardRefused = true;
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: no on-screen keyboard; text fields stay as they are\n");
        }
        if (Debugging() || answered)
            Com_Printf(CON_CHANNEL_SYSTEM, "vita input: keyboard closed (%s)\n",
                       result == VitaKeyboardResult::Entered ? "entered" : "not entered");
        s_text.KeyboardClosed(result == VitaKeyboardResult::Entered, typed);
    }
    s_text.Frame(TextWaiting(), s_engineText);
    // Without a keyboard nothing can type into the chat line, and it would
    // keep the keys: it is closed again.
    if (!KeyboardWanted() && Key_IsCatcherActive(0, KEYCATCH_MESSAGE))
        s_engineText.Key(K_ESCAPE);
}

void TypeCommand_f()
{
    if (!KeyboardWanted())
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "vita input: the on-screen keyboard is off (vita_keyboard) or was refused\n");
        return;
    }
    s_text.AskForCommand();
}

// A new install opens on "create profile", whose name field needs a keyboard.
// The menu's OK takes the name from ui_playerProfileNameNew, so when the
// on-screen keyboard is off or the system refused it, and there is no profile
// and the field is empty, a name is put there to accept.
void OfferProfileName()
{
    if (KeyboardWanted() || Com_HasPlayerProfile())
        return;
    const dvar_t *const name = Dvar_FindVar("ui_playerProfileNameNew");
    if (!name || name->type != DVAR_TYPE_STRING || *name->current.string)
        return;
    Dvar_SetString(name, kProfileName);
    Com_Printf(CON_CHANNEL_SYSTEM, "vita input: no player profile and no keyboard; the new profile's name is offered as %s\n",
               kProfileName);
}

// Once a second, from IN_Frame.
void Housekeeping(uint32_t now)
{
    if (now - s_lastHousekeepingMs < kHousekeepingEveryMs)
        return;
    s_lastHousekeepingMs = now;
    CheckDefaultBinds();
    OfferProfileName();
    if (vita_buttonNames && vita_buttonNames->modified)
    {
        Dvar_ClearModified(const_cast<dvar_s *>(vita_buttonNames));
        NameControls();
    }
}

} // namespace

void Vita_ClientLog(const char *line)
{
    Com_Printf(CON_CHANNEL_SYSTEM, "%s\n", line);
}

void Vita_ClientWindowChanged(void *hwnd)
{
    if (!hwnd && g_wv.hWnd && s_installed && Sys_IsMainThread())
        ReleaseAll();
    g_wv.hWnd = static_cast<HWND>(hwnd);
    if (!hwnd)
        return;
    SND_SetHWND(g_wv.hWnd);
    // The screen is always in front: win_wndproc.cpp's VID_AppActivate(true).
    g_wv.isMinimized = false;
    g_wv.activeApp = true;
    IN_Activate(true);
}

void Sys_InstallClientWindow()
{
    // The sticks read as centred until the pad is in analog mode.
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    ReadPanelInfo(SCE_TOUCH_PORT_FRONT, &s_front, true);
    ReadPanelInfo(SCE_TOUCH_PORT_BACK, &s_rear, false);
    s_installed = true;
}

void Sys_PumpClientWindow()
{
    Poll();
}

void IN_Init()
{
    in_mouse = Dvar_RegisterBool("in_mouse", 1, DVAR_ARCHIVE | DVAR_LATCH, "Initialize the mouse drivers");
    Dvar_ClearModified((dvar_s *)in_mouse);

    vita_stickDeadzone = Dvar_RegisterFloat("vita_stickDeadzone", 0.2f, 0.0f, 0.9f, DVAR_ARCHIVE,
                                            "Part of a stick's travel around the centre that does nothing");
    vita_lookSpeed = Dvar_RegisterFloat("vita_lookSpeed", 1400.0f, 100.0f, 10000.0f, DVAR_ARCHIVE,
                                        "Right stick at full deflection, in mouse counts a second (the game's "
                                        "sensitivity applies on top)");
    vita_lookPitchRatio = Dvar_RegisterFloat("vita_lookPitchRatio", 0.65f, 0.1f, 2.0f, DVAR_ARCHIVE,
                                             "Right stick up and down, as a part of vita_lookSpeed");
    vita_lookCurve = Dvar_RegisterFloat("vita_lookCurve", 2.0f, 1.0f, 4.0f, DVAR_ARCHIVE,
                                        "Right stick response: 1 is linear, higher is finer near the centre");
    vita_cursorSpeed = Dvar_RegisterFloat("vita_cursorSpeed", 700.0f, 100.0f, 4000.0f, DVAR_ARCHIVE,
                                          "Left stick at full deflection in a menu, in pixels a second");
    vita_frontTouch = Dvar_RegisterBool("vita_frontTouch", 1, DVAR_ARCHIVE,
                                        "The screen's left and right thirds and the top of its middle are keys in "
                                        "the game (AUX13 to AUX15 and KP_5)");
    vita_rearTouch = Dvar_RegisterBool("vita_rearTouch", 1, DVAR_ARCHIVE,
                                       "A tap on the rear touch pad's left or right half is a key in the game "
                                       "(AUX8 and AUX16)");
    vita_defaultBinds = Dvar_RegisterBool("vita_defaultBinds", 1, DVAR_ARCHIVE,
                                          "Set the default layout while no AUX key has a binding");
    vita_layout = Dvar_RegisterInt("vita_layout", vi::kFirstLayout, vi::kFirstLayout, vi::kCurrentLayout, DVAR_ARCHIVE,
                                   "The default layout this profile's controls were last given; controls that "
                                   "came with a later one get their default actions once");
    vita_buttonNames = Dvar_RegisterBool("vita_buttonNames", 1, DVAR_ARCHIVE,
                                         "The controls menu and the hints name the Vita's controls (Square), not "
                                         "the engine's keys (AUX3)");
    Dvar_ClearModified(const_cast<dvar_s *>(vita_buttonNames));
    NameControls();
    vita_inputDebug = Dvar_RegisterBool("vita_inputDebug", 0, 0, "Log every key the controls send and every touch");
    vita_keyboard = Dvar_RegisterBool("vita_keyboard", 1, DVAR_ARCHIVE,
                                      "Open the on-screen keyboard when a menu's text field or the chat line waits "
                                      "for text");
    vita_keyboardAnswer = Dvar_RegisterString("vita_keyboardAnswer", "", DVAR_NOFLAG,
                                              "For a run with nobody to type: when not empty, no keyboard is shown "
                                              "and every keyboard asked for is answered with this text");
    if (!s_commandAdded)
    {
        Cmd_AddCommandInternal("vita_binddefaults", BindDefaults_f, &s_bindDefaultsCommand);
        Cmd_AddCommandInternal("vita_typecommand", TypeCommand_f, &s_typeCommandCommand);
        s_commandAdded = true;
        Com_Printf(CON_CHANNEL_SYSTEM,
                   "vita input: front panel %d..%d by %d..%d (%s), rear pad %d..%d by %d..%d (%s)\n", s_front.minX,
                   s_front.maxX, s_front.minY, s_front.maxY, s_front.known ? "read" : "assumed", s_rear.minX,
                   s_rear.maxX, s_rear.minY, s_rear.maxY, s_rear.known ? "read" : "assumed");
    }
}

void IN_Shutdown()
{
    Vita_AbortKeyboard();
    if (s_installed && Sys_IsMainThread())
        ReleaseAll();
}

void IN_Activate(qboolean active)
{
    if (in_appactive && !active && s_installed && Sys_IsMainThread())
        ReleaseAll();
    in_appactive = active;
}

void IN_ActivateMouse(qboolean)
{
}

void IN_DeactivateMouse()
{
}

void IN_Frame()
{
    if (!s_installed || !g_wv.hWnd || !in_appactive || !Sys_IsMainThread())
        return;
    Poll();
    const vi::Motion motion = s_mapper.Take();
    s_look = CL_MouseEvent(motion.x, motion.y, motion.dx, motion.dy) != 0;
    const uint32_t now = Sys_Milliseconds();
    TextFrame(now);
    Housekeeping(now);
}

void IN_ShowSystemCursor(BOOL)
{
    // The menus draw their own cursor; there is no other.
}

void IN_SetCursorPos(tagPOINT position)
{
    if (!Sys_IsMainThread())
        return;
    ReadSettings();
    s_mapper.SetCursor(position.x, position.y, s_settings);
}

bool IN_IsForegroundWindow()
{
    return g_wv.hWnd != nullptr;
}

void IN_SetForegroundWindow()
{
}
