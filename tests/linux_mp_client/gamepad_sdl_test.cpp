// Checks the client's controller reader (src/linux/client/input/gamepad_sdl.cpp)
// against SDL3 itself, with one of SDL's virtual controllers standing in for
// the hardware: no real controller, no window, no display server, no engine.
//
// Coverage: SDL's gamepad support starts and stops; no controller reads as a
// controller at rest; a controller that appears is found and named; every
// button, both sticks and both triggers arrive in the reading the mapping
// takes, by position; the reading goes through the mapping to the engine keys
// a player would get; a controller that goes away is noticed and everything
// reads at rest again; the next one is taken.
//
// Not claimed: any particular real controller, or how a desktop delivers one
// (udev, hidraw, Steam Input). That needs hardware.

#include <linux/client/input/gamepad_map.h>
#include <linux/client/input/gamepad_sdl.h>
#include <linux/client/window/sdl_api.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace kisak::gamepad;
namespace vi = kisak::vita_input;

namespace {

int g_failures;
int g_checks;

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        ++g_checks;                                                                                 \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s (line %d)\n", #condition, __LINE__);                                   \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

constexpr char kName[] = "KisakCOD test controller";

struct VirtualPad
{
    SDL_JoystickID id = 0;
    SDL_Joystick *joystick = nullptr;

    bool Attach()
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = kName;
        id = SDL_AttachVirtualJoystick(&desc);
        if (!id)
            return false;
        joystick = SDL_OpenJoystick(id);
        return joystick != nullptr;
    }
    void Detach()
    {
        if (joystick)
            SDL_CloseJoystick(joystick);
        joystick = nullptr;
        if (id)
            SDL_DetachVirtualJoystick(id);
        id = 0;
        SDL_UpdateJoysticks();
    }
    void Button(SDL_GamepadButton button, bool down)
    {
        CHECK(SDL_SetJoystickVirtualButton(joystick, button, down));
        SDL_UpdateJoysticks();
    }
    void Axis(SDL_GamepadAxis axis, int value)
    {
        CHECK(SDL_SetJoystickVirtualAxis(joystick, axis, static_cast<Sint16>(value)));
        SDL_UpdateJoysticks();
    }
};

class Recorder final : public vi::KeySink
{
public:
    void Key(int key, bool down) override { events.push_back(down ? key : -key); }
    std::vector<int> events;
};

bool AtRest(const Reading &reading)
{
    return reading.buttons == 0 && reading.lx == 0 && reading.ly == 0 && reading.rx == 0 && reading.ry == 0
           && reading.leftTrigger == 0 && reading.rightTrigger == 0;
}

} // namespace

int main()
{
    // Whatever the machine has plugged in is not this test's business.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");

    Device device;
    Reading reading;
    CHECK(!device.Started() && !device.Connected());
    CHECK(!device.Refresh());
    reading.buttons = 77;
    CHECK(!device.Read(&reading) && AtRest(reading));

    if (!device.Start())
    {
        printf("FAIL: SDL's gamepad support did not start: %s\n", device.Error().c_str());
        return 1;
    }
    CHECK(device.Started());
    CHECK(device.Start()); // twice is once
    int present = 0;
    SDL_free(SDL_GetGamepads(&present));
    if (present != 0)
    {
        printf("SKIP: %d real controller(s) attached; this test wants to be the only one\n", present);
        return 0;
    }
    CHECK(!device.Refresh() && !device.Connected() && device.Name().empty());
    CHECK(!device.Read(&reading) && AtRest(reading));

    VirtualPad pad;
    if (!pad.Attach())
    {
        printf("FAIL: SDL made no virtual controller: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(device.Refresh());
    CHECK(device.Connected());
    CHECK(device.Name() == kName);
    CHECK(!device.Refresh()); // nothing changed since
    CHECK(device.Read(&reading));
    CHECK(AtRest(reading));

    // Every button, by position.
    const struct
    {
        SDL_GamepadButton sdl;
        Button button;
    } buttons[] = {
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
    CHECK(sizeof(buttons) / sizeof(buttons[0]) == kButtonCount);
    for (const auto &pair : buttons)
    {
        pad.Button(pair.sdl, true);
        CHECK(device.Read(&reading));
        CHECK(reading.buttons == 1u << pair.button);
        pad.Button(pair.sdl, false);
        CHECK(device.Read(&reading) && AtRest(reading));
    }
    // The Guide button belongs to the desktop (Steam, the console's menu).
    pad.Button(SDL_GAMEPAD_BUTTON_GUIDE, true);
    CHECK(device.Read(&reading) && AtRest(reading));
    pad.Button(SDL_GAMEPAD_BUTTON_GUIDE, false);

    // Sticks: left and up are negative.
    pad.Axis(SDL_GAMEPAD_AXIS_LEFTX, -32768);
    pad.Axis(SDL_GAMEPAD_AXIS_LEFTY, 32767);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHTX, 12345);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHTY, -20000);
    CHECK(device.Read(&reading));
    CHECK(reading.lx == -32768 && reading.ly == 32767 && reading.rx == 12345 && reading.ry == -20000);
    CHECK(reading.buttons == 0 && reading.leftTrigger == 0 && reading.rightTrigger == 0);
    pad.Axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    pad.Axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHTX, 0);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHTY, 0);

    // Triggers: 0 released to 32767 pulled, whatever range the device reports.
    pad.Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    CHECK(device.Read(&reading));
    CHECK(reading.leftTrigger == 32767 && reading.rightTrigger == 0);
    pad.Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); // half of the device's travel
    CHECK(device.Read(&reading));
    CHECK(reading.leftTrigger == 0 && reading.rightTrigger > 15000 && reading.rightTrigger < 17500);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    CHECK(device.Read(&reading) && AtRest(reading));

    // The whole way: a controller's readings to the engine's keys.
    Mapper mapper;
    Recorder keys;
    Settings settings;
    uint32_t now = 1000;
    const auto step = [&] {
        now += 4;
        CHECK(device.Read(&reading));
        mapper.Update(reading, now, false, false, settings, keys);
    };
    step();
    CHECK(keys.events.empty());
    pad.Button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    step();
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    step();
    pad.Button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    step();
    pad.Button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    pad.Button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    pad.Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    step();
    const int jump = GameKey(kA), fire = GameKey(kRightTrigger), frag = GameKey(kRightBumper);
    CHECK(keys.events.size() == 6);
    if (keys.events.size() == 6)
    {
        CHECK(keys.events[0] == jump && keys.events[1] == fire && keys.events[2] == frag);
        int ups = 0;
        for (int i = 3; i < 6; ++i)
            ups += keys.events[i] == -jump || keys.events[i] == -fire || keys.events[i] == -frag;
        CHECK(ups == 3);
    }

    // Unplugged while a button is down.
    pad.Button(SDL_GAMEPAD_BUTTON_WEST, true);
    CHECK(device.Read(&reading) && reading.buttons == 1u << kX);
    pad.Detach();
    CHECK(!device.Read(&reading) && AtRest(reading));
    CHECK(device.Refresh());
    CHECK(!device.Connected() && device.Name().empty());
    CHECK(!device.Refresh());

    // The next controller is taken, and starts at rest.
    CHECK(pad.Attach());
    CHECK(device.Refresh() && device.Connected() && device.Name() == kName);
    CHECK(device.Read(&reading) && AtRest(reading));

    // Stopping closes it; starting again finds it again.
    device.Stop();
    CHECK(!device.Started() && !device.Connected());
    CHECK(!device.Read(&reading) && AtRest(reading));
    CHECK(device.Start());
    pad = VirtualPad();
    CHECK(pad.Attach());
    CHECK(device.Refresh() && device.Connected());
    pad.Detach();
    device.Stop();
    SDL_Quit();

    if (g_failures)
    {
        printf("%d of %d controller reader checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("controller reader: %d checks passed\n", g_checks);
    return 0;
}
