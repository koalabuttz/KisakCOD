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
// The Vita's controls are engine keys of their own (AUX1 to AUX16 and the
// keypad arrows), so `bind` and the controls menu work on them. A profile has
// no bindings for them at first; the default layout is set once, while none of
// the AUX keys is bound to anything (vita_defaultBinds, vita_binddefaults).
//
// START + SELECT still quits, from the main loop (Vita_QuitRequested): both
// readers peek at the pad, so neither takes the other's samples.
//
// Not here yet: the on-screen keyboard for text fields. Until it is, the one
// field a new install cannot get past, the new profile's name, is filled in
// (OfferProfileName).

#include "vita_client.h"

#include "input/vita_input_map.h"

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
#include <ui/ui_shared.h>
#include <win32/win_input.h>
#include <win32/win_local.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <cstdio>
#include <cstring>

namespace vi = kisak::vita_input;

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
static_assert(SameKey(vi::kKeyMouse1, K_MOUSE1) && SameKey(vi::kKeyWheelDown, K_MWHEELDOWN));
static_assert(SameKey(vi::kKeyWheelUp, K_MWHEELUP) && SameKey(vi::kKeyAux1, K_AUX1));
static_assert(SameKey(vi::kKeyAux1 + 15, K_AUX16));
static_assert(SameKey(vi::kMaxFrontTouches, 6) && SameKey(vi::kMaxRearTouches, 4) && SCE_TOUCH_MAX_REPORT >= 6);

namespace {

// The pad is sampled once a display frame; reading more often than this only
// costs system calls. Win_GetEvent asks several times a frame.
constexpr uint32_t kPollEveryMs = 4;
constexpr uint32_t kHousekeepingEveryMs = 1000;
// The name the "create profile" box offers while no keyboard can type one.
constexpr char kProfileName[] = "Vita";

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
const dvar_t *vita_inputDebug;
cmd_function_s s_bindDefaultsCommand;
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
// those bound to nothing.
int BindDefaults(bool all)
{
    int bound = 0;
    for (int i = 0; i < vi::kControlCount; ++i)
    {
        const vi::Control control = static_cast<vi::Control>(i);
        const char *const command = vi::DefaultBinding(control);
        const int key = vi::GameKey(control);
        if (!command || (!all && !Unbound(key)))
            continue;
        char binding[64];
        snprintf(binding, sizeof(binding), "%s", command);
        Key_SetBinding(0, key, binding);
        ++bound;
    }
    return bound;
}

void BindDefaults_f()
{
    Com_Printf(CON_CHANNEL_SYSTEM, "vita input: %d controls set to the default layout\n", BindDefaults(true));
}

// A profile made on a PC, a new profile and the controls menu's "reset" all
// start from `unbindall` and the PC's keys, which leave every AUX key bound to
// nothing: the Vita's layout is set then. One control left unbound on purpose
// stays so, as long as another AUX key has a binding.
void CheckDefaultBinds()
{
    if (!vita_defaultBinds || !vita_defaultBinds->current.enabled || Display_KeyBindPending())
        return;
    for (int key = K_AUX1; key <= K_AUX16; ++key)
    {
        if (!Unbound(key))
            return;
    }
    const int bound = BindDefaults(false);
    Com_Printf(CON_CHANNEL_SYSTEM,
               "vita input: the Vita's controls had no bindings; %d set to the default layout (docs/vita-client.md)\n",
               bound);
}

// A new install opens on "create profile", whose name field needs a keyboard
// the Vita client does not have yet. The menu's OK takes the name from
// ui_playerProfileNameNew, so while there is no profile and the field is
// empty, a name is put there to accept. Goes when the on-screen keyboard
// comes.
void OfferProfileName()
{
    if (Com_HasPlayerProfile())
        return;
    const dvar_t *const name = Dvar_FindVar("ui_playerProfileNameNew");
    if (!name || name->type != DVAR_TYPE_STRING || *name->current.string)
        return;
    Dvar_SetString(name, kProfileName);
    Com_Printf(CON_CHANNEL_SYSTEM, "vita input: no player profile and no keyboard yet; the new profile's name is offered as %s\n",
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
                                        "The screen's left and right thirds are keys in the game (AUX13 to AUX15)");
    vita_rearTouch = Dvar_RegisterBool("vita_rearTouch", 1, DVAR_ARCHIVE,
                                       "A tap on the rear touch pad's left or right half is a key in the game "
                                       "(AUX8 and AUX16)");
    vita_defaultBinds = Dvar_RegisterBool("vita_defaultBinds", 1, DVAR_ARCHIVE,
                                          "Set the default layout while no AUX key has a binding");
    vita_inputDebug = Dvar_RegisterBool("vita_inputDebug", 0, 0, "Log every key the controls send and every touch");
    if (!s_commandAdded)
    {
        Cmd_AddCommandInternal("vita_binddefaults", BindDefaults_f, &s_bindDefaultsCommand);
        s_commandAdded = true;
        Com_Printf(CON_CHANNEL_SYSTEM,
                   "vita input: front panel %d..%d by %d..%d (%s), rear pad %d..%d by %d..%d (%s)\n", s_front.minX,
                   s_front.maxX, s_front.minY, s_front.maxY, s_front.known ? "read" : "assumed", s_rear.minX,
                   s_rear.maxX, s_rear.minY, s_rear.maxY, s_rear.known ? "read" : "assumed");
    }
}

void IN_Shutdown()
{
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
    Housekeeping(Sys_Milliseconds());
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
