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
//
// With `bloom` (an image from BuildBloom over `from`) and a bloomScale above
// 0, the bloom's light times bloomScale is added to the scene's before the
// tonemap.
bool WriteDisplay(Core *core, Tonemap *tonemap, Image *scene, const VkRect2D &from, Image *target,
                  const VkRect2D &to, VkFilter filter, const char *pass, std::string &error,
                  Image *bloom = nullptr, float bloomScale = 0.0f);

struct TonemapStats
{
    uint64_t passes; // WriteDisplay calls that recorded
};
TonemapStats Stats(const Tonemap *tonemap);

// ---- Bloom (renderer plan MD1; docs/vulkan-modern.md) ----
//
// Light above a threshold in the linear-light scene, spread over a chain of
// images each half the size of the one before (13-tap filter down, tent
// filter up, as in Jimenez, SIGGRAPH 2014), for the tonemap to add. The
// engine's vision set glow sets it: its cutoff (in display values) is the
// threshold, its radius chooses how many levels the chain has, its intensity
// the amount added and its desaturation the colour taken out.

struct BloomParams
{
    float threshold = 1.0f;    // linear light; below it no bloom
    float knee = 0.5f;         // soft knee width below the threshold
    float desaturation = 0.0f; // 0 keeps the colour, 1 is grey
    uint32_t levels = 5;       // the chain's images, the first at half size (1 to kMaxBloomLevels)
};
constexpr uint32_t kMaxBloomLevels = 7;

// How many levels spread light about as far as a Gaussian of `sigma` pixels of
// the scene (Legacy's glow radius in scene pixels) and a bit wider: at least
// 3, at most kMaxBloomLevels, never a level under 4 pixels on a side.
uint32_t BloomLevelsFor(float sigma, uint32_t width, uint32_t height);

struct Bloom;

// The pipelines (the images are made at the first BuildBloom and again when
// the area's size changes). Null with the reason.
Bloom *CreateBloom(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded (a device teardown calls it).
void DestroyBloom(Core *core, Bloom *bloom);

// Records the chain over `area` of `scene`: the image returned (sampled
// usage, half the area's size) holds the sum of every level's light, ready
// for WriteDisplay's `bloom` (scale it by intensity / levels). Outside a
// pass, inside a frame. Null with the reason, and nothing recorded, when the
// scene cannot take part. The passes are timed under "modern bloom".
Image *BuildBloom(Core *core, Bloom *bloom, Image *scene, const VkRect2D &area, const BloomParams &params,
                  std::string &error);

struct BloomStats
{
    uint64_t builds; // BuildBloom calls that recorded
    uint64_t passes; // the passes they recorded
};
BloomStats Stats(const Bloom *bloom);
} // namespace gfx_vk::modern
