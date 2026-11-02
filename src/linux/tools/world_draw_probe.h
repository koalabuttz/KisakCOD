#pragma once

#include <filesystem>

#include "world_frontend_layout.h"

// World draw probe: the offline asset exporter with the whole renderer, drawing
// one fixed view of a real map. The probe's CL_InitDedicated configures the
// renderer for one thread (sys_smp_allowed off, which makes the worker thread
// dvars default off), registers the renderer and effects dvars, and turns
// fx_marks off; unlike the dedicated server's it latches nothing off and loads
// no zone.
//
// Kisak_StartWorldDraw runs where a client's CL_InitRenderer would, inside
// $init. After its own checks (the shared struct layouts agree, a Direct3D 9
// interface can be had at all) it starts the renderer with r_init.cpp's own
// R_Init: the window, the device, the graphics zones queued behind it, render
// targets, initial state, the scene, the code images and materials. It does not
// call R_BeginRegistration, which would hand the device to a render thread for
// loading screens; the renderer stays on this thread.
//
// Kisak_DrawWorld runs after $init: it loads one allowlisted map zone,
// registers the world with R_LoadWorld, and draws views of it from the map's own
// cameras, frame by frame as the engine's cgame-free scene render
// (R_GenerateReflectionRawData in r_reflection_probe.cpp) does. After each frame
// it reads the back buffer into memory, as the engine's R_GetBackBufferData
// does after its cubemap frames, and measures it. It writes world-draw-probe.json
// (counts and measurements, no pixels) into the private output directory and
// throws std::runtime_error on any failure.
//
// Kisak_ReplayFreeCamera is the build's second mode (KISAK_FREE_CAMERA_MAP): the
// same renderer start, then the free camera's FrameOwner over the engine renderer
// (engine_scene_renderer.h) replays the free camera's reference traversals on the
// map, with a resize, a minimize, a device reset and a reload along the way. It
// writes free-camera-replay.json. See docs/linux-free-camera.md.
//
// KISAK_FREE_CAMERA_INPUT chooses where the free camera's input comes from:
// unset, the replay above; `window`, the renderer's own window, adopted by the
// free camera's window host (src/linux/client/view/free_camera_window.h), until
// it quits or KISAK_FREE_CAMERA_SECONDS (1 to 3600, live input only) run out;
// `script`, the same window driven by SDL events the run pushes into its queue
// and checks. Kisak_RunFreeCameraWindow runs both and writes
// free-camera-window.json. Kisak_StartWorldDraw reads and checks both variables
// with the other arguments.
//
// loaderLayout is KISAK_WORLD_FRONTEND_LAYOUT() as the caller's translation unit
// (compiled with the dedicated definitions, like the database loader) sees it.
//
// Built only by KISAK_BUILD_WORLD_DRAW_PROBE
// (scripts/platform/linux/world_draw_probe.cmake). See
// docs/linux-world-draw-probe.md.
enum class WorldDrawMode
{
    Draw,       // KISAK_WORLD_DRAW_MAP: Kisak_DrawWorld
    FreeCamera, // KISAK_FREE_CAMERA_MAP: Kisak_ReplayFreeCamera
};
void Kisak_StartWorldDraw(WorldDrawMode mode, const char *mapName, const kisak_world_frontend::Layout &loaderLayout);
void Kisak_DrawWorld(const std::filesystem::path &directory, const char *mapName);
void Kisak_ReplayFreeCamera(const std::filesystem::path &directory, const char *mapName);
enum class FreeCameraInput
{
    Replay, // KISAK_FREE_CAMERA_INPUT unset: Kisak_ReplayFreeCamera
    Window, // window: Kisak_RunFreeCameraWindow, live
    Script, // script: Kisak_RunFreeCameraWindow, scripted
};
// What Kisak_StartWorldDraw read; Replay outside the free-camera mode.
FreeCameraInput Kisak_FreeCameraInput();
// KISAK_FREE_CAMERA_SECONDS, or 0 for no limit.
int Kisak_FreeCameraSeconds();
void Kisak_RunFreeCameraWindow(const std::filesystem::path &directory, const char *mapName, FreeCameraInput input,
                               int secondsLimit);
// Seconds since Kisak_StartWorldDraw began.
double WorldDrawStartSeconds();
