#pragma once

// gfx_vk::modern: the Modern path of the Vulkan renderer (renderer plan
// sections 3 and 6), on the shared core (vk_core.h). It records its own passes
// into the frame the Legacy path draws, on the same core images.
//
// What it has so far: the first piece of the plan's early experiment (E1),
// temporal anti-aliasing over the scene the Legacy path drew with a jittered
// projection, and the first piece of linear light (MD1), the scene drawn
// into a linear-light image and tonemapped back (below). The engine jitters the projection (src/gfx_d3d/r_scene.cpp)
// and, when the 3D scene is done, the D3D9 front's Vulkan back end hands this
// module the scene's colour and depth images. Velocity comes from the camera
// alone for now: what moves on its own is kept from smearing by clipping the
// history to this frame's colours. See docs/vulkan-modern.md.

#include <gfx_vk/vk_core.h>

#include <cstdint>
#include <string>

namespace gfx_vk::modern
{
// The camera of one scene, in the engine's (D3D9's) convention: row vectors,
// p_clip = p_world * view * projection.
struct SceneView
{
    float view[4][4];
    // With this frame's jitter in [2][0] and [2][1] (NDC units: the
    // projection adds them to x / w and y / w).
    float projection[4][4];
    // The scene's viewport in the images.
    VkRect2D area;
    // A cut (a new map, a teleport, a spectator switch): start the history
    // again instead of reprojecting.
    bool cut = false;
};

// clipToPrevClip for the camera's motion: takes a point in this frame's clip
// space without the jitter to the same world point in the last frame's, in
// row-vector form (p_prev = p_clip * out). Worked in double, since world
// positions in the hundreds of thousands lose too much in float. False when
// this frame's matrix cannot be inverted.
bool Reprojection(const SceneView &current, const SceneView &previous, double out[4][4]);

// The jitter of frame n of the sequence the engine applies: Halton (2, 3)
// over 8 frames, in pixels, each in (-0.5, 0.5).
void JitterPixels(uint32_t frame, float &x, float &y);

struct Taa;

// The pipeline and the history images (made at the first Resolve). Null with
// the reason.
Taa *CreateTaa(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded (a device teardown calls it).
void DestroyTaa(Core *core, Taa *taa);

// Records the anti-aliasing of the scene in `color` (read through `depth`):
// the result replaces `color` within view.area and becomes the next frame's
// history. Outside a pass, inside a frame. Both images single-sampled and
// sampled-usage; `color` also a blit destination. False with the reason, and
// nothing recorded, when they are not.
bool Resolve(Core *core, Taa *taa, Image *color, Image *depth, const SceneView &view, std::string &error);

struct TaaStats
{
    uint64_t resolves;  // Resolve calls that recorded
    uint64_t restarts;  // of those, ones with no history (first, cut, resize)
};
TaaStats Stats(const Taa *taa);

// ---- Linear light (renderer plan MD1; docs/vulkan-modern.md) ----
//
// With linear light the 3D scene is drawn into an R16G16B16A16_SFLOAT image
// in linear light instead of the 8-bit back buffer: the Legacy adapter decodes
// its pixel shaders' colour output from sRGB as it draws there
// (legacy::SetLinearLight), so blending happens in linear light. When the
// scene is done, the tonemap writes it back as display values.

// The scene image's format.
constexpr VkFormat kSceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct Tonemap;

// The pipeline layout (pipelines are made per target format as needed).
// Null with the reason.
Tonemap *CreateTonemap(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded (a device teardown calls it).
void DestroyTonemap(Core *core, Tonemap *tonemap);

// Records one pass that writes the linear-light scene in `scene` (the
// rectangle `from`) into `target` (the rectangle `to`) as display values:
// today the neutral tonemap, the light clamped to 1 and encoded with the sRGB
// curve, the exact inverse of the Legacy adapter's decode. Alpha is copied.
// With different sizes the scene is sampled with the filter. `pass` names the
// pass in the core's pass timings. Outside a pass, inside a frame. False with
// the reason, and nothing recorded, when the images cannot take part.
bool WriteDisplay(Core *core, Tonemap *tonemap, Image *scene, const VkRect2D &from, Image *target,
                  const VkRect2D &to, VkFilter filter, const char *pass, std::string &error);

struct TonemapStats
{
    uint64_t passes; // WriteDisplay calls that recorded
};
TonemapStats Stats(const Tonemap *tonemap);
} // namespace gfx_vk::modern
