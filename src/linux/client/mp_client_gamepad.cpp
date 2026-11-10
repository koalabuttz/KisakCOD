// A game controller in the multiplayer client (mp_client_gamepad.h).
//
// The engine takes keys and a mouse. src/linux/client/input/gamepad_map.cpp
// decides what the controller's buttons, sticks and triggers mean in those
// terms (the PS Vita client's mapping with a full-size controller on it), and
// gamepad_sdl.cpp reads the controller through SDL3. This file is the engine
// between them, the way src/vita/client/vita_client_input.cpp is on the Vita:
//   * GamepadPump takes a reading whenever the window's events are pumped and
//     queues the keys as SE_KEY events, the way a keyboard's arrive;
//   * GamepadMouse is what the game window calls in place of CL_MouseEvent:
//     the right stick's motion is added to the mouse's, and in a menu the
//     cursor belongs to whichever of the mouse and the left stick moved last.
//     CL_MouseEvent's answer says who has the mouse, which is how the next
//     reading is mapped;
//   * once a second GamepadFrame looks for a controller, notices one leaving,
//     and gives the controller's keys their default actions while none of the
//     AUX keys is bound to anything.
// The controller's controls are engine keys of their own (AUX1 to AUX16 and
// the keypad arrows), so `bind` and the controls menu work on them.
//
// Nothing here runs until a controller is connected, apart from asking SDL
// once a second whether there is one; in_gamepad 0 stops that too.

#if !defined(KISAK_LINUX_MP_CLIENT) || !defined(KISAK_LINUX_GAMEPAD)
#error "mp_client_gamepad.cpp is KisakCOD-mp's controller support: build it with KISAK_LINUX_GAMEPAD"
#endif

#include "mp_client_gamepad.h"

#include "mp_client_window.h"
#include "input/gamepad_map.h"
#include "input/gamepad_sdl.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <ui/keycodes.h>
#include <ui/ui_shared.h>
#include <win32/win_local.h>

#include <cstdio>

namespace gp = kisak::gamepad;
namespace vi = kisak::vita_input;

// The mapping names the engine's keys by number, to stay free of the engine.
static_assert(vi::kKeyEnter == K_ENTER && vi::kKeyEscape == K_ESCAPE);
static_assert(vi::kKeyUpArrow == K_UPARROW && vi::kKeyDownArrow == K_DOWNARROW);
static_assert(vi::kKeyLeftArrow == K_LEFTARROW && vi::kKeyRightArrow == K_RIGHTARROW);
static_assert(vi::kKeyKpUpArrow == K_KP_UPARROW && vi::kKeyKpDownArrow == K_KP_DOWNARROW);
static_assert(vi::kKeyKpLeftArrow == K_KP_LEFTARROW && vi::kKeyKpRightArrow == K_KP_RIGHTARROW);
static_assert(vi::kKeyMouse1 == K_MOUSE1 && vi::kKeyWheelDown == K_MWHEELDOWN && vi::kKeyWheelUp == K_MWHEELUP);
static_assert(vi::kKeyAux1 == K_AUX1 && vi::kKeyAux1 + 15 == K_AUX16);

namespace {

// Reading more often than this only costs calls; Win_GetEvent asks several
// times a frame.
constexpr uint32_t kPollEveryMs = 4;
constexpr uint32_t kHousekeepingEveryMs = 1000;

const dvar_t *in_gamepad;
const dvar_t *in_gamepadDeadzone;
const dvar_t *in_gamepadLookSpeed;
const dvar_t *in_gamepadLookPitchRatio;
const dvar_t *in_gamepadLookCurve;
const dvar_t *in_gamepadCursorSpeed;
const dvar_t *in_gamepadTrigger;
const dvar_t *in_gamepadDefaultBinds;
const dvar_t *in_gamepadDebug;
cmd_function_s s_bindDefaultsCommand;
bool s_commandAdded;

gp::Device s_device;
gp::Mapper s_mapper;
gp::Settings s_settings;
bool s_startFailed; // SDL refused once; not asked again until in_gamepad is switched off and on
bool s_active;      // the game window has the focus
bool s_look;        // CL_MouseEvent's last answer: the game looks with the mouse
bool s_mouseAsked;  // the window handed the mouse over since the last GamepadFrame
int s_mouseX, s_mouseY;
bool s_polled;
uint32_t s_lastPollMs;
uint32_t s_lastHousekeepingMs;
uint32_t s_eventTime;

bool Enabled()
{
    return in_gamepad && in_gamepad->current.enabled;
}

class EngineKeys final : public vi::KeySink
{
public:
    void Key(int key, bool down) override
    {
        if (in_gamepadDebug && in_gamepadDebug->current.enabled)
            Com_Printf(CON_CHANNEL_SYSTEM, "gamepad: %s %s\n", Key_KeynumToString(key, 0), down ? "down" : "up");
        Sys_QueEvent(s_eventTime, SE_KEY, key, down, 0, nullptr);
    }
};
EngineKeys s_keys;

void ReadSettings()
{
    // The cursor moves in the units CL_MouseEvent takes: the window's pixels.
    if (kisak::mp_client::ClientWindow *const window = kisak::mp_client::GameWindow())
    {
        const kisak::window::PixelExtent pixels = window->Host().SurfaceExtent();
        if (pixels.width > 0 && pixels.height > 0)
        {
            s_settings.pad.screenWidth = pixels.width;
            s_settings.pad.screenHeight = pixels.height;
        }
    }
    if (!in_gamepadDeadzone)
        return;
    s_settings.pad.stickDeadzone = in_gamepadDeadzone->current.value;
    s_settings.pad.lookSpeed = in_gamepadLookSpeed->current.value;
    s_settings.pad.lookPitchRatio = in_gamepadLookPitchRatio->current.value;
    s_settings.pad.lookCurve = in_gamepadLookCurve->current.value;
    s_settings.pad.cursorSpeed = in_gamepadCursorSpeed->current.value;
    s_settings.triggerThreshold = in_gamepadTrigger->current.value;
}

void ReleaseAll()
{
    s_eventTime = Sys_Milliseconds();
    s_mapper.Release(s_keys);
}

// One reading of the controller, through the mapping, into the event queue.
void Poll()
{
    if (!Enabled() || !s_active || !s_device.Connected())
        return;
    const uint32_t now = Sys_Milliseconds();
    if (s_polled && now - s_lastPollMs < kPollEveryMs)
        return;
    s_polled = true;
    s_lastPollMs = now;

    gp::Reading reading;
    if (!s_device.Read(&reading))
    {
        // Unplugged between two looks at the list: everything held is let go.
        ReleaseAll();
        return;
    }
    ReadSettings();
    s_eventTime = now;
    s_mapper.Update(reading, now, !s_look, Display_KeyBindPending() != 0, s_settings, s_keys);
}

bool Unbound(int key)
{
    const char *const binding = Key_GetBinding(0, static_cast<uint32_t>(key));
    return !binding || !*binding;
}

// Gives the controller's controls their default actions: all of them, or
// only those bound to nothing.
int BindDefaults(bool all)
{
    int bound = 0;
    for (int control = 0; control < gp::kControlCount; ++control)
    {
        const char *const command = gp::DefaultBinding(control);
        const int key = gp::GameKey(control);
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
    Com_Printf(CON_CHANNEL_SYSTEM, "gamepad: %d controls set to the default layout\n", BindDefaults(true));
}

// A profile's keys start from the PC's, which leave every AUX key bound to
// nothing, and so does the controls menu's reset: a connected controller
// would do nothing but look around. Its layout is set then. One control left
// unbound on purpose stays so, as long as another AUX key has a binding, and
// a profile nobody plugs a controller into is never touched.
void CheckDefaultBinds()
{
    if (!in_gamepadDefaultBinds->current.enabled || Display_KeyBindPending())
        return;
    for (int key = K_AUX1; key <= K_AUX16; ++key)
    {
        if (!Unbound(key))
            return;
    }
    const int bound = BindDefaults(false);
    Com_Printf(CON_CHANNEL_SYSTEM,
               "gamepad: the controller's buttons had no bindings; %d set to the default layout "
               "(docs/linux-mp-client.md, \"Game controllers\")\n",
               bound);
}

// Once a second, from GamepadFrame.
void Housekeeping(uint32_t now)
{
    if (now - s_lastHousekeepingMs < kHousekeepingEveryMs)
        return;
    s_lastHousekeepingMs = now;
    if (!Enabled())
    {
        if (s_device.Started())
        {
            ReleaseAll();
            s_device.Stop();
        }
        s_startFailed = false;
        return;
    }
    if (!s_device.Started())
    {
        if (s_startFailed)
            return;
        if (!s_device.Start())
        {
            s_startFailed = true;
            Com_Printf(CON_CHANNEL_SYSTEM, "gamepad: SDL could not start controller support: %s\n",
                       s_device.Error().c_str());
            return;
        }
    }
    if (s_device.Refresh())
    {
        // Whatever the old controller held is let go; a new one starts clean.
        ReleaseAll();
        if (s_device.Connected())
            Com_Printf(CON_CHANNEL_SYSTEM, "gamepad: using %s\n", s_device.Name().c_str());
        else
            Com_Printf(CON_CHANNEL_SYSTEM, "gamepad: the controller was disconnected\n");
    }
    if (s_device.Connected())
        CheckDefaultBinds();
}

} // namespace

namespace kisak::mp_client {

void GamepadInit()
{
    in_gamepad = Dvar_RegisterBool("in_gamepad", 1, DVAR_ARCHIVE, "Use a connected game controller");
    in_gamepadDeadzone = Dvar_RegisterFloat("in_gamepadDeadzone", 0.2f, 0.0f, 0.9f, DVAR_ARCHIVE,
                                            "Part of a stick's travel around the centre that does nothing");
    in_gamepadLookSpeed = Dvar_RegisterFloat("in_gamepadLookSpeed", 1400.0f, 100.0f, 10000.0f, DVAR_ARCHIVE,
                                             "Right stick at full deflection, in mouse counts a second (the game's "
                                             "sensitivity applies on top)");
    in_gamepadLookPitchRatio = Dvar_RegisterFloat("in_gamepadLookPitchRatio", 0.65f, 0.1f, 2.0f, DVAR_ARCHIVE,
                                                  "Right stick up and down, as a part of in_gamepadLookSpeed");
    in_gamepadLookCurve = Dvar_RegisterFloat("in_gamepadLookCurve", 2.0f, 1.0f, 4.0f, DVAR_ARCHIVE,
                                             "Right stick response: 1 is linear, higher is finer near the centre");
    in_gamepadCursorSpeed = Dvar_RegisterFloat("in_gamepadCursorSpeed", 1000.0f, 100.0f, 8000.0f, DVAR_ARCHIVE,
                                               "Left stick at full deflection in a menu, in pixels a second");
    in_gamepadTrigger = Dvar_RegisterFloat("in_gamepadTrigger", 0.3f, 0.05f, 0.95f, DVAR_ARCHIVE,
                                           "How far a trigger is pulled, as a part of its travel, before its key "
                                           "goes down");
    in_gamepadDefaultBinds = Dvar_RegisterBool("in_gamepadDefaultBinds", 1, DVAR_ARCHIVE,
                                               "When a controller is connected and no AUX key has a binding, set "
                                               "the default controller layout");
    in_gamepadDebug = Dvar_RegisterBool("in_gamepadDebug", 0, 0, "Log every key the controller sends");
    if (!s_commandAdded)
    {
        Cmd_AddCommandInternal("gamepad_binddefaults", BindDefaults_f, &s_bindDefaultsCommand);
        s_commandAdded = true;
    }
}

void GamepadShutdown()
{
    if (!Sys_IsMainThread())
        return;
    if (s_device.Started())
        ReleaseAll();
    s_device.Stop();
    s_startFailed = false;
}

void GamepadPump()
{
    Poll();
}

int GamepadMouse(int x, int y, int dx, int dy)
{
    s_mouseAsked = true;
    s_mouseX = x;
    s_mouseY = y;
    if (!Enabled() || !s_active || !s_device.Connected())
    {
        s_look = CL_MouseEvent(x, y, dx, dy) != 0;
        return s_look;
    }
    Poll();
    ReadSettings();
    const vi::Motion motion = s_mapper.Frame(x, y, dx, dy, s_settings);
    s_look = CL_MouseEvent(motion.x, motion.y, motion.dx, motion.dy) != 0;
    return s_look;
}

void GamepadFrame(bool active)
{
    if (!in_gamepad || !Sys_IsMainThread())
        return;
    if (s_active && !active && s_device.Started())
        ReleaseAll();
    s_active = active;
    // The window hands the mouse over once a frame while it has one to hand
    // over. With in_mouse 0 it does not; the controller looks and points all
    // the same.
    if (active && !s_mouseAsked && Enabled() && s_device.Connected())
        GamepadMouse(s_mouseX, s_mouseY, 0, 0);
    s_mouseAsked = false;
    Housekeeping(Sys_Milliseconds());
}

void GamepadSetCursor(int x, int y)
{
    if (!in_gamepad || !Sys_IsMainThread())
        return;
    ReadSettings();
    s_mapper.SetCursor(x, y, s_settings);
}

} // namespace kisak::mp_client
