#pragma once

#include <filesystem>

#include "world_frontend_layout.h"

// World device probe: the offline asset exporter with a Direct3D 9 device. The
// probe's CL_InitDedicated configures the renderer and registers its dvars but,
// unlike the dedicated server's, latches nothing off and loads no zone.
//
// Kisak_StartWorldDevice runs where a client's CL_InitRenderer would, inside
// $init: it creates the window and the device through r_init.cpp
// (R_PreCreateWindow, R_SetWndParms, R_CreateWindow, R_CreateDevice), checks
// that the shader model 3 path was picked, and queues the graphics zones with
// r_init.cpp's own R_LoadGraphicsAssets, which is what R_InitHardware does right
// after the device exists. The zone loader then creates every texture, shader,
// vertex declaration and vertex/index buffer on that device.
//
// Kisak_ProbeWorldDevice runs after $init: it loads one allowlisted map zone the
// same way, points the renderer at its GfxWorld, checks that every device object
// the loaded zones asked for exists and matches its asset, writes
// world-device-probe.json into the private output directory, and throws
// std::runtime_error on any failure.
//
// loaderLayout is KISAK_WORLD_FRONTEND_LAYOUT() as the caller's translation unit
// (compiled with the dedicated definitions, like the database loader) sees it.
// The probe refuses to create anything when its own view differs.
//
// Built only by KISAK_BUILD_WORLD_DEVICE_PROBE
// (scripts/platform/linux/world_device_probe.cmake). See
// docs/linux-world-device-probe.md.
void Kisak_StartWorldDevice(const char *mapName, const kisak_world_frontend::Layout &loaderLayout);
void Kisak_ProbeWorldDevice(const std::filesystem::path &directory, const char *mapName);
