#pragma once

#include <filesystem>

#include "world_frontend_layout.h"

// Offline world front-end probe: loads one allowlisted map zone through the
// engine's database, points the renderer at its GfxWorld, sorts materials and
// world surfaces the way the first frame does, then runs the production front
// end (DPVS portal walk, BSP draw-surface batching, the merge) from cameras
// above the map's own spawn points and checks the map-independent rules in
// world_frontend_invariants.h. Writes world-frontend-probe.json into the
// private output directory and throws std::runtime_error on any failure.
//
// loaderLayout is KISAK_WORLD_FRONTEND_LAYOUT() as the caller's translation unit
// (compiled with the dedicated definitions, like the database loader) sees it.
// The probe refuses to load anything when the front end's view differs.
//
// Built only by KISAK_BUILD_WORLD_FRONTEND_PROBE
// (scripts/platform/linux/world_frontend_probe.cmake). See
// docs/linux-world-frontend.md.
void Kisak_ProbeWorldFrontEnd(const std::filesystem::path &directory, const char *mapName,
                              const kisak_world_frontend::Layout &loaderLayout);
