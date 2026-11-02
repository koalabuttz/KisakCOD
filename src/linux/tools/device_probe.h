#pragma once

// Shared between the device probe (device_probe.cpp) and its link boundary
// (device_probe_boundary.cpp).

#include <stdexcept>
#include <string>

namespace kisak::device_probe {

// Com_Error in the probe's host throws this: the engine asked to stop, and the
// probe reports which stage was running instead of exiting from inside r_init.
class EngineError : public std::runtime_error
{
  public:
    explicit EngineError(const std::string &message) : std::runtime_error(message)
    {
    }
};

} // namespace kisak::device_probe
