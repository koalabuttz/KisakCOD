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
// A surface the scene drew that moves on its own (an entity, the view model,
// a brush model such as a door) is drawn again into an R16G16_SFLOAT image, with this frame's and last
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
    // Positions are three floats at the start of each vertex; strides of 32
    // (models' vertices) and 44 (brush models') are drawn, others skipped.
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

// depthTest false draws every surface wherever it lands, hidden or not: only
// for the debug view that finds them (KISAK_VK_MODERN_DEBUG=velocity-all).
Velocity *CreateVelocity(Core *core, std::string &error, bool depthTest = true);
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
    uint64_t world;    // of those, surfaces drawn from the world's 44-byte vertices (brush models)
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

// ---- Ambient occlusion (docs/vulkan-modern.md "Ambient occlusion") ----
//
// Ground-truth ambient occlusion (Jimenez et al. 2016) from the scene depth
// and the normals buffer, recorded before the lit surfaces are drawn so that
// they can darken their indirect light alone: two slices of six steps a side
// per pixel, jittered per pixel and frame, then a 5x5 blur that keeps to each
// surface. The result is the visible fraction (1 unoccluded) in the red
// channel of a target the caller owns.

struct AoParams
{
    float radius = 48.0f;  // world units (inches): about 1.2 m
    float falloff = 0.6f;  // the outer share of the radius over which samples fade out
    // The visibility is raised to it. PC AO run 1: 1 weak, 2 about right in the
    // shade; with the thickness (PC AO run 3) 1.5 read like run 1's 1.
    float power = 2.0f;
    // World units a sample's surface is taken to reach behind it: what lies
    // further behind stays lit (a thin sign or arch edge in front of a wall
    // no longer darkens the wall beside it as a solid block would).
    float thickness = 8.0f;
    // |N.V| from which the effect is whole on walls; on walls seen at a
    // shallower angle it fades out from here to 0.02. Floors and ceilings
    // keep 0.08, so ground seen from far off keeps its contact shadows. PC
    // AO run 4: at 0.08 a corner on a wall seen nearly side-on (the crash-in
    // arch edge) read as a dark line; David chose to soften that alone.
    float grazing = 0.2f;
    // The darkest the visibility may go after the power, reached through a
    // soft knee 0.1 wide (0 turns it off). PC AO run 7: the crash-in arch
    // line is a narrow slot between the pillar's side and the arch's inside
    // face that went near black (0.13) and ended in a hard edge; the ground's
    // contact shadows stay far above it (about 0.85).
    float floor = 0.35f;
    // The blur's depth tolerance: a tap whose view depth is this share of
    // the pixel's away counts half. PC AO run 8: at 0.02 the blur stops dead
    // at the crash-in arch slot's edge (the pillar's side face against the
    // arch's inside face a little behind it), a hard step from 91 to 253
    // that reads as a line. PC AO run 9: at 0.08 it ramps over 4 pixels
    // (94, 120, 175, 225), with no halos round outlines and the ground
    // within 2 levels; 0.25 and 0.6 soften it no further but lighten the
    // ceiling beams' edges more.
    float edge = 0.08f;
    // Work the occlusion out at half resolution, one pixel of each 2x2 block
    // (which one moving from frame to frame), and blur it back up to full
    // resolution across the depth: about a quarter of the occlusion's work.
    bool half = true;
    uint32_t frame = 0;    // moves the noise
};

struct AmbientOcclusion;

// The pipelines (the module's own image is made at the first build). Null
// with the reason.
AmbientOcclusion *CreateAmbientOcclusion(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyAmbientOcclusion(Core *core, AmbientOcclusion *ao);
// Records the occlusion of view.area into `target` (a colour attachment as
// large as the area's corner, single-sampled, sampled usage), from `depth`
// and BuildNormals' `normals`. Outside the area `target` is left as it is.
// Outside a pass, inside a frame. False with the reason, and nothing
// recorded, when it cannot.
bool BuildAmbientOcclusion(Core *core, AmbientOcclusion *ao, Image *depth, Image *normals, const SceneView &view,
                           const AoParams &params, Image *target, std::string &error);

struct AoStats
{
    uint64_t builds; // BuildAmbientOcclusion calls that recorded
};
AoStats Stats(const AmbientOcclusion *ao);

// ---- Volumetric fog (docs/vulkan-modern.md "Volumetric fog") ----
//
// The map's own fog lit by the sun: a ray march through the sun's shadow map
// (fog_march.frag, at half resolution by default) sums how much of each view
// ray's fog is in shadow, then a full-screen pass (fog_apply.frag) adds the
// difference that makes to the scene in linear light. A share of the fog's
// light (sunShare) comes from the sun: air in shadow loses it, and looking
// towards the sun the air gets more of it (forward scattering), so with
// sunShare 0, or with all the air lit and no forward scattering, the scene
// is as the game fogged it. The view's shadowed spot lights light the air in
// their cones too, marched through their own shadow maps: light added on top
// of the game's fog (spotShare times what the fog would scatter of it).
//
// With `froxels` the same sums come from a volume instead of a march per
// pixel: compute passes fill a grid of froxels over the view (froxelTile
// pixels square, froxelSlices deep over the march length, spaced as the
// march's steps) with each froxel's share, add them up front to back, and
// the apply pass reads the volume at each pixel's distance.

struct FogParams
{
    // The share of the fog's light that comes from the sun, 0 to 1.
    float sunShare = 0.6f;
    // Henyey-Greenstein forward scattering, 0 (even) to below 1.
    float scatter = 0.35f;
    // World units of each ray marched through the shadow map; air past it
    // counts as lit.
    float march = 3000.0f;
    uint32_t steps = 24;
    // How brightly the spot lights light the fog: 0 not at all, 1 as the
    // fog scatters their light.
    float spotShare = 1.0f;
    // Steps marched through each spot light's reach.
    uint32_t spotSteps = 12;
    bool half = true;   // march at half resolution
    // Build a froxel volume (compute passes) in place of the march.
    bool froxels = false;
    uint32_t froxelTile = 8;    // pixels per froxel across
    uint32_t froxelSlices = 64; // froxels deep
    uint32_t frame = 0; // moves the noise and the half-resolution pixel from frame to frame
    // Write the light the fog adds alone, over mid grey, instead of adding it.
    bool debug = false;
};

// The view's fog and sun, as the engine gives them (front ModernSceneView::Fog).
struct FogInputs
{
    float color[3] = {};       // display values, 0 to 1
    float density = 0.0f;      // log2 units per world unit
    float start = 0.0f;        // world units
    float origin[3] = {};      // the camera, world units
    float sunDirection[3] = {}; // towards the sun, world space
    // The sun's shadow is in shadowMap (DrawFog): the lookup below is set.
    bool sunShadowed = false;
    // World (p * M) to the near partition's xy, depth z and partition value w.
    float shadowLookup[4][4] = {};
    float switchPartition[4] = {};
    float shadowmapScale[2] = {};
    // The view's shadowed spot lights (front ModernSceneView::Fog::Spot).
    struct Spot
    {
        float origin[3] = {};
        float radius = 0.0f;
        float dir[3] = {};             // from a lit point towards the light, the cone's axis
        float exponent = 0.0f;         // the cone's falloff power, 0 none
        float cosHalfFovOuter = 0.0f;
        float cosHalfFovInner = 0.0f;
        float fade = 0.0f;             // how far the shadow map counts, the rest lit
        float color[3] = {};           // diffuse colour, display values
        float shadowLookup[4][4] = {}; // world (p * M) to its tile, projective
        bool inSunMap = false;         // its tile is in shadowMap, else in spotShadowMap
    };
    uint32_t spotCount = 0; // at most kFogSpots
    Spot spots[4];
};
constexpr uint32_t kFogSpots = 4;

struct Fog;

// The pipelines (the march image is made at the first DrawFog and again when
// the depth's size changes). Null with the reason.
Fog *CreateFog(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyFog(Core *core, Fog *fog);
// Records the fog over view.area of `color` (a linear-light colour target),
// reading `depth` (single-sampled, sampled usage) and, when given, the sun's
// shadow map `shadowMap` and the spot lights' `spotShadowMap` (depth images).
// Without shadowMap the sun's air counts as lit; a spot light whose map is
// not given is left out. Outside a pass, inside a frame. The full-screen passes are
// timed under "modern fog", the froxel volume's two compute passes under
// "modern fog froxels". False with the reason, and nothing recorded, when it
// cannot.
bool DrawFog(Core *core, Fog *fog, Image *color, Image *depth, Image *shadowMap, Image *spotShadowMap,
             const SceneView &view, const FogInputs &inputs, const FogParams &params, std::string &error);

struct FogStats
{
    uint64_t scenes;     // DrawFog calls that recorded
    uint64_t shadowed;   // of those, ones that marched the sun's shadow map
    uint64_t spotScenes; // of those, ones with a spot light in the air
    uint64_t spots;      // spot lights marched, over all of them
    uint64_t froxels;    // of the scenes, ones drawn from a froxel volume
};
FogStats Stats(const Fog *fog);

// ---- God rays (docs/vulkan-modern.md "God rays") ----
//
// Shafts of the sky's light streaming from the sun past what blocks it, made
// on the screen (the classic radial blur): a pass at half resolution keeps
// the sky's light near the sun (rays_mask.frag: sky pixels, by how close
// they are to the sun's direction, plus a small glow of the sun's own
// colour), a second smears that along each pixel's line to the sun's place
// on the screen (rays_blur.frag), and a full-screen pass adds the result to
// the linear-light scene (rays_apply.frag), faded out as the sun leaves the
// view. Where buildings or trees stand in front of the sky the mask is dark,
// so the smear leaves dark gaps between the shafts. Independent of the map's
// fog: the air's own thickness does not change.

struct RaysParams
{
    // How much of the smeared light is added: 0 none.
    float strength = 0.6f;
    // Samples along each pixel's line towards the sun.
    uint32_t samples = 48;
    // How far towards the sun each line reaches, as a share of the way.
    float length = 0.85f;
    // Each sample counts this much less than the one before it.
    float decay = 0.96f;
    // How tightly the kept sky gathers round the sun: its light is weighted
    // by cos(angle to the sun) to this power.
    float spread = 12.0f;
    // The sun's own glow added to the kept sky (its colour times this, in a
    // disc a few degrees across), so a sky without a drawn sun still has
    // rays.
    float glow = 1.0f;
    // Light in the kept sky is held below this (linear light), so one very
    // bright pixel does not streak.
    float most = 8.0f;
    uint32_t frame = 0; // moves the samples' offset from frame to frame
};

// The view's sun (front ModernSceneView::Sun).
struct RaysInputs
{
    float sunDirection[3] = {}; // towards the sun, world space
    float sunColor[3] = {};     // display values (may pass 1)
};

struct Rays;

// The pipelines (the half-resolution images are made at the first DrawRays
// and again when the area's size changes). Null with the reason.
Rays *CreateRays(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyRays(Core *core, Rays *rays);
// Records the god rays over view.area of `color` (the linear-light scene,
// single-sampled, sampled and colour attachment usage), reading it and
// `depth` (single-sampled, sampled usage; the sky at SkyDepth). Outside a
// pass, inside a frame. When the sun is not in front of the camera nothing
// is recorded and `drawn` is false. The three passes are timed under
// "modern god rays". False with the reason, and nothing recorded, when it
// cannot.
bool DrawRays(Core *core, Rays *rays, Image *color, Image *depth, const SceneView &view, const RaysInputs &inputs,
              const RaysParams &params, bool &drawn, std::string &error);

struct RaysStats
{
    uint64_t scenes; // DrawRays calls that recorded
    uint64_t behind; // DrawRays calls with the sun out of the view, nothing recorded
};
RaysStats Stats(const Rays *rays);

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

// ---- Reflection probes (docs/vulkan-modern.md "Reflection probes") ----
//
// The game's reflection probes are cube maps baked when the map was made:
// level 0 the scene around the probe, the levels below it box-filtered, and
// the lit shaders pick level 6 - 8 gloss, so a rough surface reads a face's
// average (six flat colours at level 6) mixed in display values, which dims
// what is bright. FilterReflectionProbe makes those levels again for the
// Modern path from level 0 alone, in linear light.

// The power of the cosine lobe about the reflected ray that level `level` of
// a filtered probe sums: the gloss that picks the level (6 - level) / 8,
// whose Blinn-Phong power 2^(9.37752 gloss) + 7 (the sun's highlight in
// lm_sun.frag) is about four times the power about the reflected ray.
float ReflectionProbeLobePower(uint32_t level);
// A probe cube `edge` texels wide with `levels` levels (edge >> level each),
// texels of 4 bytes B, G, R, A as D3DFMT_A8R8G8B8 holds them, in the game's
// encoding (R_ColorCorrectBGRAPixel: the display value is rgb * a * 4).
// bits[face * levels + level] is that face's level (D3D9's face order),
// pitches[...] its bytes per row. Writes levels 1 on of every face: each
// texel the light of ReflectionProbeLobePower(level) about its direction,
// summed in linear light from level 0, stored in the same encoding. Level 0
// is left as it is, so filtering a probe again gives the same levels. False,
// and nothing written, when there is nothing to filter (edge or levels
// under 2) or bits are missing.
bool FilterReflectionProbe(uint32_t edge, uint32_t levels, uint8_t *const *bits, const uint32_t *pitches);

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
// Draws the ambient occlusion in `ao` (its red channel) as grey over `area`
// of `target`. Outside a pass, inside a frame.
bool DrawAoDebug(Core *core, DebugView *debug, Image *ao, Image *target, const VkRect2D &area, std::string &error);
// Draws the sharpest level of `reflections` (TraceReflections' image, the
// same size) over `area` of `target`: the stored light over black, through
// the sRGB curve. Outside a pass, inside a frame.
bool DrawReflectionsDebug(Core *core, DebugView *debug, Image *reflections, Image *target, const VkRect2D &area,
                          std::string &error);

// ---- Compute passes (docs/vulkan-modern.md "Compute passes") ----
//
// The core's compute passes (vk_core.h BeginComputePass) carry the Modern
// path's work that is not a full-screen draw: volumes such as a froxel fog's,
// and later AO or particles. The compute check is the smallest such user, and
// what checks the facility end to end (KISAK_VK_MODERN_DEBUG=compute): two
// compute passes build an RGBA16F 3D volume and a draw samples it.
//
//   "modern compute check fill"       one invocation per texel writes the
//                                     texel's centre (x, y, z in 0..1) and
//                                     alpha 0.5 into a 3D storage image, and
//                                     each slice's weights (1, 1, 1, 2) into
//                                     a storage buffer
//   "modern compute check integrate"  one invocation per column walks the
//                                     first volume front to back (as a froxel
//                                     fog integrates) and writes into a second
//                                     the weighted mean of the slices so far
//   "modern compute check draw"       a full-screen pass samples the second
//                                     volume with linear filtering at one
//                                     depth over an area: red and green the
//                                     pixel's place across the area (clamped
//                                     to the outer texel centres), blue the
//                                     mean slice centre, times alpha (1)

// The check's volume, in texels: x, y, z.
constexpr uint32_t kComputeCheckSize[3] = {16, 8, 8};

struct ComputeCheck;

// Its pipelines, volumes and buffer. Null with the reason.
ComputeCheck *CreateComputeCheck(Core *core, std::string &error);
// Waits for the GPU to finish what was recorded.
void DestroyComputeCheck(Core *core, ComputeCheck *check);
// Records the two compute passes. Outside a pass, inside a frame. False with
// the reason when nothing could be recorded.
bool BuildComputeCheck(Core *core, ComputeCheck *check, std::string &error);
// Draws the last volume BuildComputeCheck built over `area` of `target`,
// sampled at `depth` (0..1 along z). Outside a pass, inside a frame.
bool DrawComputeCheck(Core *core, ComputeCheck *check, Image *target, const VkRect2D &area, float depth,
                      std::string &error);

struct ComputeCheckStats
{
    uint64_t builds; // BuildComputeCheck calls that recorded
    uint64_t draws;  // DrawComputeCheck calls that recorded
};
ComputeCheckStats Stats(const ComputeCheck *check);

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
    float maxEv = 1.5f;
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
