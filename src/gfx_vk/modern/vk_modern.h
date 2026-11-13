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
// module the scene's colour and depth images. Velocity comes from the camera,
// and for surfaces that move on its own from a velocity pass that draws them
// again with this frame's and last frame's matrices (below); what neither
// covers is kept from smearing by clipping the history to this frame's
// colours. See docs/vulkan-modern.md.

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
// sampled-usage; `color` also a blit destination. `velocity`, when given, is
// DrawVelocity's image for this scene: where it holds a motion, that replaces
// the camera's. False with the reason, and nothing recorded, when they are
// not.
bool Resolve(Core *core, Taa *taa, Image *color, Image *depth, const SceneView &view, std::string &error,
             Image *velocity = nullptr);
// The view of the last scene Resolve recorded; false when there is no history.
bool LastView(const Taa *taa, SceneView &out);
// The image the last Resolve wrote (its LastView's area holds that scene,
// anti-aliased, as the colour image held it: linear light when the scene
// was); null when there is no history. Valid until the next Resolve, which
// writes the other image, or a change of size: take it again each scene.
// Make it Sampled (UseImage) before a pass reads it.
Image *LastHistory(const Taa *taa);

struct TaaStats
{
    uint64_t resolves;  // Resolve calls that recorded
    uint64_t restarts;  // of those, ones with no history (first, cut, resize)
};
TaaStats Stats(const Taa *taa);

// ---- Velocity of what moves on its own (E1, docs/vulkan-modern.md) ----
//
// A surface the scene drew that moves on its own (an entity, the view model)
// is drawn again into an R16G16_SFLOAT image, with this frame's and last
// frame's object-to-clip matrices. Where it is what the scene shows (its depth
// matches the scene depth), the image holds where it was last frame relative
// to where it is now, in NDC without either frame's jitter; everywhere else
// it holds kNoOwnMotion and TAA uses the camera's motion.

constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
// The clear value (the shaders test for at least 30000).
constexpr float kNoOwnMotion = 65504.0f;

// One indexed draw of the scene, by what the Legacy draw bound.
struct MovedSurface
{
    // Positions are three floats at the start of each vertex.
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    uint32_t stride = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexBase = 0;
    // The Legacy draw's viewport: D3D9's, which is flipped and moved by
    // half a pixel to Vulkan's as the Legacy adapter does.
    float viewport[4] = {}; // x, y, width, height in pixels
    float minDepth = 0.0f, maxDepth = 1.0f;
    // D3D9 row-vector matrices (p_clip = p_object * M): object to this
    // frame's clip space as the scene drew it (with its jitter), and to last
    // frame's as that scene drew it (with that frame's jitter).
    float clip[4][4];
    float prevClip[4][4];
    // A skinned surface: last frame's vertices, kept by KeepVertices (the
    // same stride, from the first vertex the draw uses), and the range of
    // vertices the draw reads, from vertexBase. Null: the surface's vertices
    // are the same in both frames.
    VkBuffer previous = VK_NULL_HANDLE;
    VkDeviceSize previousOffset = 0;
    uint32_t minIndex = 0;
    uint32_t vertexCount = 0;
};

struct Velocity;

Velocity *CreateVelocity(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyVelocity(Core *core, Velocity *velocity);

// Records the velocity of `surfaces` within view.area into the module's
// image (the size of `depth`, made or made again as needed) and returns it,
// ready to sample. `previous` is last frame's view (for its jitter). Null
// with nothing recorded when there are no surfaces; null with the reason when
// it cannot. Outside a pass, inside a frame.
Image *DrawVelocity(Core *core, Velocity *velocity, Image *depth, const SceneView &view, const SceneView &previous,
                    const MovedSurface *surfaces, uint32_t count, std::string &error);

struct VelocityStats
{
    uint64_t passes;   // DrawVelocity calls that recorded
    uint64_t surfaces; // surfaces they drew
    uint64_t skinned;  // of those, skinned surfaces drawn from last scene's vertices
};
VelocityStats Stats(const Velocity *velocity);

// Skinned surfaces' vertices are written again each frame (by the CPU, into a
// buffer locked with DISCARD), so last frame's are gone by the time the
// velocity pass runs. The pass keeps a copy of each skinned surface's
// vertices at the end of its scene, in a buffer of its own, for the next
// scene's velocity: half of the buffer is written each scene while the other
// half holds the last scene's.
struct KeptVertices;

// The vertices of one skinned surface the scene drew: vertexCount vertices of
// 32 bytes from `offset` in `vertices`, kept under `key` (the renderer's key
// for the surface, the same from one scene to the next).
struct KeepRequest
{
    uint64_t key = 0;
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t vertexCount = 0;
};

KeptVertices *CreateKeptVertices();
// The buffer is released once the GPU is done with it.
void DestroyKeptVertices(Core *core, KeptVertices *kept);
// Where the last KeepVertices kept `key`'s vertices; false when it did not
// keep them, or kept another number of them.
bool FindKept(const KeptVertices *kept, uint64_t key, uint32_t vertexCount, VkBuffer &buffer, VkDeviceSize &offset);
// Records copies of these surfaces' vertices for the next scene (the buffer
// is made at the first call) and forgets what the last call kept. Surfaces
// that do not fit, or are asked for twice, are left out. Outside a pass,
// inside a frame. False with the reason when nothing could be recorded.
bool KeepVertices(Core *core, KeptVertices *kept, const KeepRequest *requests, uint32_t count, std::string &error);
// Forgets what was kept (a camera cut, a scene without skinned surfaces).
void ForgetKept(KeptVertices *kept);

struct KeptStats
{
    uint64_t scenes;   // KeepVertices calls that recorded copies
    uint64_t surfaces; // surfaces they kept
    uint64_t bytes;    // bytes copied
};
KeptStats Stats(const KeptVertices *kept);

// ---- Normals buffer (docs/vulkan-modern.md "Normals buffer") ----
//
// The view-space normal of the surface each pixel of the scene shows, worked
// out from the scene depth (normals.frag) once the game's depth prepass has
// drawn it, before the lit surfaces: what ambient occlusion and screen-space
// reflections read. The normals are those of the geometry (no normal maps).
// Each texel holds normal * 0.5 + 0.5 in the engine's view space (x right,
// y up, z forward, so a surface facing the camera has z < 0) and alpha 1;
// a pixel with no surface (the sky) holds (0.5, 0.5, 0.5, 0).

constexpr VkFormat kNormalsFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

// The scene's projection inverted (NDC x, y, depth and 1 to homogeneous view
// space), row vectors as the engine's matrices; worked in double. False when
// it cannot be inverted.
bool ClipToView(const SceneView &view, float out[4][4]);
// The depth at or past which the scene has no surface: just short of a point
// at infinity's (or of 1, for a projection without one).
float SkyDepth(const SceneView &view);

struct Normals;

// The pipeline (the image is made at the first BuildNormals and again when
// the depth's size changes). Null with the reason.
Normals *CreateNormals(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyNormals(Core *core, Normals *normals);
// Records the normals of view.area of `depth` (single-sampled, sampled usage)
// into the module's image (the size of `depth`; outside the area it holds "no
// surface") and returns it, ready to sample. Outside a pass, inside a frame.
// Null with the reason, and nothing recorded, when it cannot.
Image *BuildNormals(Core *core, Normals *normals, Image *depth, const SceneView &view, std::string &error);
// The image the last BuildNormals wrote; null before the first.
Image *CurrentNormals(const Normals *normals);

struct NormalsStats
{
    uint64_t builds; // BuildNormals calls that recorded
};
NormalsStats Stats(const Normals *normals);

// ---- Screen-space reflections (docs/vulkan-modern.md "Screen-space reflections") ----
//
// What each pixel's surface would mirror, found on the screen: the view ray
// is reflected about the normals buffer's normal and marched through the
// scene depth (reflections.frag), right after the normals are built, before
// the lit surfaces. The light at a hit is last scene's anti-aliased colour
// (LastHistory), reprojected by the camera's motion. The result is an
// R16G16B16A16_SFLOAT image with mip levels, each half the size of the one
// before (a 2x2 average): rgb = reflected light (linear light, before
// exposure) times the confidence, a = the confidence (0 where nothing was
// met). The lit shaders are meant to read it in place of the reflection
// probe, a blurrier level for a rougher surface.

constexpr VkFormat kReflectionsFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kMaxReflectionLevels = 8;

struct ReflectionParams
{
    float maxDistance = 2000.0f; // the longest ray, in world units
    float thickness = 16.0f;     // how far behind a surface a ray still meets it, in world units
    uint32_t steps = 32;         // the most steps a ray takes (one a pixel at most)
    uint32_t levels = 5;         // mip levels, the first at full size (1 to kMaxReflectionLevels)
};

struct Reflections;

// The pipeline (the images are made at the first TraceReflections and again
// when the depth's size changes). Null with the reason.
Reflections *CreateReflections(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyReflections(Core *core, Reflections *reflections);
// Records the reflections of view.area of `depth` (single-sampled, sampled
// usage) with `normals` (BuildNormals' image of the same depth) and the
// light of `history` (LastHistory, whose scene was `historyView`) into the
// module's image (the size of `depth`; outside the area it holds no
// reflection) and returns it, ready to sample. Outside a pass, inside a
// frame. Null with the reason, and nothing recorded, when it cannot.
Image *TraceReflections(Core *core, Reflections *reflections, Image *depth, Image *normals, Image *history,
                        const SceneView &view, const SceneView &historyView, const ReflectionParams &params,
                        std::string &error);

struct ReflectionStats
{
    uint64_t traces; // TraceReflections calls that recorded
    uint32_t levels; // the last image's mip levels
};
ReflectionStats Stats(const Reflections *reflections);

// ---- Debug views (KISAK_VK_MODERN_DEBUG; docs/vulkan-modern.md) ----
//
// Drawn over the anti-aliased scene (TAA's history is kept apart, so a view
// never feeds back into it).

struct DebugView;

// Null with the reason.
DebugView *CreateDebugView(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyDebugView(Core *core, DebugView *debug);
// Colours each pixel of `area` in `target` that has motion of its own in
// `velocity` (DrawVelocity's image, the same size), by the direction and
// length of that motion, and leaves the rest. Outside a pass, inside a frame.
bool DrawVelocityDebug(Core *core, DebugView *debug, Image *velocity, Image *target, const VkRect2D &area,
                       std::string &error);
// Colours each pixel of `area` in `target` that has a surface in `normals`
// (BuildNormals' image, the same size) by its normal (normal * 0.5 + 0.5, as
// stored), and leaves the rest. Outside a pass, inside a frame.
bool DrawNormalsDebug(Core *core, DebugView *debug, Image *normals, Image *target, const VkRect2D &area,
                      std::string &error);
// Draws the sharpest level of `reflections` (TraceReflections' image, the
// same size) over `area` of `target`: the stored light over black, through
// the sRGB curve. Outside a pass, inside a frame.
bool DrawReflectionsDebug(Core *core, DebugView *debug, Image *reflections, Image *target, const VkRect2D &area,
                          std::string &error);

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
// the light through the tonemap's curve (SetCurve; Clamp, the exact inverse
// of the Legacy adapter's decode, until set), then the sRGB curve. Alpha is
// copied.
// With different sizes the scene is sampled with the filter. `pass` names the
// pass in the core's pass timings. Outside a pass, inside a frame. False with
// the reason, and nothing recorded, when the images cannot take part.
//
// With `bloom` (an image from BuildBloom over `from`) and a bloomScale above
// 0, the bloom's light times bloomScale is added to the scene's before the
// tonemap.
//
// With `exposure` (a 1x1 result of MeasureExposure) the light, the bloom's
// included, is multiplied by exp2 of its red first; without, by 1.
bool WriteDisplay(Core *core, Tonemap *tonemap, Image *scene, const VkRect2D &from, Image *target,
                  const VkRect2D &to, VkFilter filter, const char *pass, std::string &error,
                  Image *bloom = nullptr, float bloomScale = 0.0f, Image *exposure = nullptr);

// The curve the tonemap takes the light through before the sRGB curve
// (tonemap.frag): Clamp (light over 1 clips; the exact inverse of the Legacy
// adapter's decode), Neutral (the same up to 0.76, then a soft shoulder: the
// Khronos PBR Neutral tonemap's, without its toe), Agx (AgX's base look) or
// Aces (Stephen Hill's fit of the ACES transforms). Clamp until set.
enum class TonemapCurve : uint32_t
{
    Clamp = 0,
    Neutral = 1,
    Agx = 2,
    Aces = 3,
};
void SetCurve(Tonemap *tonemap, TonemapCurve curve);
// "clamp", "neutral", "agx", "aces"; CurveFromName is false for anything else.
const char *CurveName(TonemapCurve curve);
bool CurveFromName(const char *name, TonemapCurve &curve);

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

// ---- Auto exposure (docs/vulkan-modern.md "Auto exposure") ----
//
// Each scene the linear-light scene's area is measured on a 64x64 grid (the
// log2 of its luminance, clamped to [-12, 4] per tap and weighted towards the
// centre of the view), and an exposure in EV moves from last scene's towards
// the one that brings the weighted average to the key, within a range. The
// result is a 1x1 image for WriteDisplay's `exposure`: the tonemap multiplies
// the light by exp2 of its red. Nothing is read back to adapt.

struct ExposureParams
{
    // The average luminance that needs no change (exposure 0 EV): the mean of
    // mp_crash's and mp_backlot's outdoor views (PC exposure run 1).
    float key = 0.115f;
    float minEv = -1.0f;   // the range the exposure stays in
    float maxEv = 1.0f; // David 2026-11-13: +1, so dark interiors keep more of their mood
    // Time constants, in seconds: towards a lower exposure (the scene got
    // brighter) and towards a higher one (it got darker).
    float brighterSeconds = 1.0f;
    float darkerSeconds = 2.5f;
    float seconds = 0.0f;  // since the last scene measured
    bool reset = false;    // take the target at once (a camera cut)
};

struct Exposure;

// The pipelines and the small images. Null with the reason.
Exposure *CreateExposure(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded (a device teardown calls it).
void DestroyExposure(Core *core, Exposure *exposure);

// Records the measure, reduce and adapt passes over `area` of `scene` and
// returns this scene's 1x1 result (red: exposure in EV; green: the measured
// weighted average log2 luminance; blue: the EV the scene asked for before
// the range). Outside a pass, inside a frame. Null with the reason, and
// nothing recorded, when the scene cannot take part. The passes are timed
// under "modern exposure".
Image *MeasureExposure(Core *core, Exposure *exposure, Image *scene, const VkRect2D &area,
                       const ExposureParams &params, std::string &error);
// The last result MeasureExposure returned; null before the first.
Image *CurrentExposure(const Exposure *exposure);

struct ExposureStats
{
    uint64_t scenes;  // MeasureExposure calls that recorded
    uint64_t resets;  // of those, ones that took the target at once
    uint64_t passes;  // the passes they recorded
    // Results read back now and then (one in 16 scenes), as they came in.
    uint64_t samples;
    double evSum, evMin, evMax;     // the exposure used, in EV
    double log2LuminanceSum;        // the measured weighted average log2 luminance
    double wantedEvSum;             // the EV asked for before the range
};
// With waitForReading, a read back still on its way is waited for first.
ExposureStats Stats(Core *core, Exposure *exposure, bool waitForReading = false);
// Starts the statistics again (a read back on its way still counts).
void ResetStats(Exposure *exposure);
} // namespace gfx_vk::modern
