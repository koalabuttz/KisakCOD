// The game controller through SDL3 (gamepad_sdl.h).

#include "gamepad_sdl.h"

#include <linux/client/window/sdl_api.h>

#if KISAK_LINUX_SDL_MAJOR != 3
#error "the client's controller support uses SDL3's gamepad API: build it with KISAK_LINUX_SDL_MAJOR=3"
#endif

namespace kisak::gamepad {

namespace {

struct ButtonPair
{
    SDL_GamepadButton sdl;
    Button button;
};

// By position, not by label: the bottom face button jumps on every
// controller, whatever letter is printed on it.
constexpr ButtonPair kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, kA},
    {SDL_GAMEPAD_BUTTON_EAST, kB},
    {SDL_GAMEPAD_BUTTON_WEST, kX},
    {SDL_GAMEPAD_BUTTON_NORTH, kY},
    {SDL_GAMEPAD_BUTTON_BACK, kBack},
    {SDL_GAMEPAD_BUTTON_START, kStart},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftBumper},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightBumper},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftStickClick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightStickClick},
};
static_assert(sizeof(kButtons) / sizeof(kButtons[0]) == kButtonCount);

} // namespace

Device::~Device()
{
    Stop();
}

bool Device::Start()
{
    if (started_)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        error_ = SDL_GetError();
        return false;
    }
    started_ = true;
    error_.clear();
    return true;
}

void Device::Close()
{
    if (pad_)
        SDL_CloseGamepad(pad_);
    pad_ = nullptr;
    name_.clear();
}

void Device::Stop()
{
    Close();
    if (started_)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    started_ = false;
}

bool Device::Refresh()
{
    if (!started_)
        return false;
    // The window's event pump updates controllers too; this keeps the list
    // current when it is asked for between pumps.
    SDL_UpdateGamepads();
    bool changed = false;
    if (pad_ && !SDL_GamepadConnected(pad_))
    {
        Close();
        changed = true;
    }
    if (pad_)
        return changed;
    int count = 0;
    SDL_JoystickID *const ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count && !pad_; ++i)
    {
        pad_ = SDL_OpenGamepad(ids[i]);
        if (!pad_)
            error_ = SDL_GetError();
    }
    SDL_free(ids);
    if (!pad_)
        return changed;
    const char *const name = SDL_GetGamepadName(pad_);
    name_ = name && *name ? name : "controller";
    return true;
}

bool Device::Read(Reading *reading)
{
    *reading = Reading();
    if (!pad_ || !SDL_GamepadConnected(pad_))
        return false;
    for (const ButtonPair &pair : kButtons)
    {
        if (SDL_GetGamepadButton(pad_, pair.sdl))
            reading->buttons |= 1u << pair.button;
    }
    reading->lx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
    reading->ly = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY);
    reading->rx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX);
    reading->ry = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY);
    reading->leftTrigger = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    reading->rightTrigger = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return true;
}

} // namespace kisak::gamepad
