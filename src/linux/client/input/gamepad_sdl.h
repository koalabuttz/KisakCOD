#pragma once

// The game controller itself, through SDL3: finding one, noticing it come and
// go, and one reading of its buttons, sticks and triggers in the form
// gamepad_map.h takes. No engine call; mp_client_gamepad.cpp is the engine's
// side. The client window test drives this with one of SDL's virtual
// controllers.
//
// One controller at a time: the first SDL lists. When it goes away the next
// Refresh takes whichever is listed first then. Used from one thread, the one
// that pumps the window.

#include "gamepad_map.h"

#include <string>

struct SDL_Gamepad;

namespace kisak::gamepad {

class Device
{
public:
    Device() = default;
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    // Starts SDL's gamepad support. False when SDL refuses; Error() says why.
    bool Start();
    // Closes the controller and stops SDL's gamepad support.
    void Stop();
    bool Started() const { return started_; }

    // Looks for a controller while none is open, and notices the open one
    // going away. True when that changed: Connected() and Name() are new.
    bool Refresh();
    bool Connected() const { return pad_ != nullptr; }
    // What SDL calls the controller; empty while there is none.
    const std::string &Name() const { return name_; }

    // One reading. False, with the reading at rest, while there is no
    // controller.
    bool Read(Reading *reading);

    const std::string &Error() const { return error_; }

private:
    void Close();

    SDL_Gamepad *pad_ = nullptr;
    bool started_ = false;
    std::string name_;
    std::string error_;
};

} // namespace kisak::gamepad
