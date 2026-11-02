#pragma once

// The engine calls the world draw probe and the free-camera adapter share: one
// frame of a scene, read back and measured, and one map loaded and unloaded the
// way the client's cgame start does it. Nothing here chooses what to draw.
//
// Every failure throws std::runtime_error("<tool> stage=<stage>: <reason>"),
// where <tool> is world_draw_probe unless SetTool named another mode.
//
// Built only into KisakCOD-world-draw-probe (scripts/platform/linux/world_draw_probe.cmake),
// with the client definitions and the native D3D9 boundary.

#include "world_draw_rules.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

struct GfxWorld;
struct refdef_s;

namespace kisak_world_draw_engine
{
namespace wdr = kisak_world_draw;

// ---- reporting --------------------------------------------------------------

// The name failures and reports carry. A string literal: it is kept, not copied.
void SetTool(const char *tool);
const char *Tool();
[[gnu::format(printf, 1, 2)]] void Say(const char *format, ...);
[[noreturn]] void Fail(const char *stage, const std::string &reason);
void Require(bool ok, const char *stage, const std::string &why);
std::string Hex(uint32_t value);
std::string Hex64(uint64_t value);
std::string Json(const std::string &text);
// Created exclusively, owner-only, inside the private directory the export
// entry point already checked.
void WriteExclusiveFile(const std::filesystem::path &file, const std::string &text);
double Seconds(std::chrono::steady_clock::time_point since);

// ---- the frame --------------------------------------------------------------

struct Camera
{
    std::string classname;
    float eye[3] = {};
    float angles[3] = {};
};

struct FrameResult
{
    wdr::FrameStats stats;
    wdr::DrawCounts counts;
    // Surfaces this frame moved into the static model cache (R_CacheStaticModelSurface):
    // a view has not settled while it is filling.
    uint32_t smcPatches = 0;
    double seconds = 0;
};

// The renderer R_Init started, with its graphics zones and code images.
void RequireStartedRenderer();
// The back buffer's size, as the device reports it.
void BackBufferSize(uint32_t &width, uint32_t &height);
// The back buffer copied to system memory and measured; the pixels are not kept.
wdr::FrameStats ReadBackBuffer();
// Whether the backend will draw this frame's pre-tessellated surfaces from the
// index buffer the front end fills for it (docs/linux-world-draw-probe.md, "The
// first frame").
bool PreTessBuffersAgree();
// A frame with no scene in it, as a client presents for its menu or loading
// screen. After R_Init and after every device reset this is what makes the two
// pre-tessellated index buffer pointers agree again.
void PresentEmptyFrame();
// A zeroed refdef with what every frame here shares: local client 0, no blur, no
// scissor, the primary lights R_InitPrimaryLights sets up. The caller fills the
// view: x, y, width, height, the tangents, vieworg, viewaxis, zNear and time.
void PrepareRefdef(refdef_s &refdef);
// One frame of that view, call for call as R_GenerateReflectionRawData draws a
// cubemap face (without the effects update), then the draw counts and, when
// readBack is set, the measured back buffer. Requires the pre-tessellated index
// buffers to agree before the frame and a ready device after it.
FrameResult DrawRefdef(refdef_s &refdef, bool readBack = true);

// ---- the map ----------------------------------------------------------------

// The map zone, then the cgame's map load (CG_Init): the collision map as the
// client's CL_CM_LoadMap loads it when no local server runs, the shared world
// data, then the renderer's own world registration (R_LoadWorld). Returns the
// world's name. The map name must be allowlisted.
std::string LoadMap(const char *mapName);
// The reverse, in the order the zone unload needs (each owner refuses to unload
// an asset still in use): R_ShutdownWorld, Com_ShutdownWorld, CM_Shutdown, then
// the map zone (DB_ZONE_GAME) is freed. Safe on a partly loaded map: each part
// is released only when it is held.
void UnloadMap();
// Whether any part of a map is held: the zone, the collision map, the shared
// world data or the renderer's world.
bool MapHeld();
GfxWorld *FindPackagedWorld(const char *mapName);
// View A is the map author's intermission camera when the map has one, otherwise
// the first player spawn. View B is the first player spawn that is not view A.
void ChooseCameras(const GfxWorld &world, Camera &a, Camera &b);

std::string CameraJson(const Camera &camera);
std::string FrameJson(const FrameResult &frame);
// One world_draw_frame line.
void SayFrame(const char *label, int draw, const FrameResult &frame);

} // namespace kisak_world_draw_engine
