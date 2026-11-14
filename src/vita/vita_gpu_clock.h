// The Vita's GPU clock for the multiplayer client.
//
// An application's GPU runs at the clock the system gives it unless the
// application asks for more: the client raises the GPU to 222 MHz and its
// crossbar to 166 MHz at start-up (vita_main.cpp, Vita_Startup), the most
// an application may ask for, as it already does for the CPU (444 MHz) and
// the bus (222 MHz). The clocks found at start-up are kept so that the live
// switch "vita_gxm no-gpu-boost 1" (gxm_legacy_draw.cpp) can put them back
// for a run that compares, and "vita_gxm no-gpu-boost 0" raise them again.

#pragma once

#include <psp2/power.h>

namespace vita
{
struct GpuClocks
{
    int gpu = 0;  // MHz
    int xbar = 0; // MHz
};

// The clocks the system gave the application, read once at start-up.
inline GpuClocks &BootGpuClocks()
{
    static GpuClocks clocks;
    return clocks;
}

inline GpuClocks CurrentGpuClocks()
{
    return {scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency()};
}

// Raises the GPU clocks, or puts back the ones found at start-up.
inline void SetGpuBoost(bool on)
{
    constexpr int kGpuMhz = 222;
    constexpr int kXbarMhz = 166;
    const GpuClocks &boot = BootGpuClocks();
    scePowerSetGpuClockFrequency(on ? kGpuMhz : boot.gpu);
    scePowerSetGpuXbarClockFrequency(on ? kXbarMhz : boot.xbar);
}
} // namespace vita
