// The D3D9 front's GPU on Vulkan: a Vulkan core per device (vk_d3d9_backend.h).

#include "vk_d3d9_backend.h"

#include "vk_d3d9_internal.h"

#include <gfx_vk/modern/vk_modern.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gfx_vk::d3d9
{
namespace
{
// One of the frames that took longest (BackendLogStats' `slow frame` lines):
// the time from one Present to the next, and what the device did in it.
struct SlowFrame
{
    double ms = 0.0;          // 0: no frame
    uint64_t number = 0;      // among the frames since the statistics were reset, from 1
    double seconds = 0.0;     // since then, at its end
    uint64_t presentedAs = 0; // the core's number for it (FrameGpuTime)
    uint64_t draws = 0;
    uint64_t passes = 0;
    uint64_t submissions = 0;
    uint64_t dynamicRenames = 0;
    uint32_t pipelines = 0;
    uint32_t pipelineCacheHits = 0;
    double pipelineMs = 0.0;
    uint32_t pixelModules = 0;
    double pixelModuleMs = 0.0;
    uint32_t aheadWaits = 0; // pipelines a draw waited for a worker to finish
    double aheadWaitMs = 0.0;
    uint32_t shaders = 0;
    double shaderMs = 0.0;
    uint64_t uploads = 0;
    double uploadMegabytes = 0.0;
    double uploadMs = 0.0;
    uint64_t readbacks = 0;
    uint64_t memoryAllocations = 0;
    double memoryAllocateMs = 0.0;
    uint32_t swapchainBuilds = 0;
    double imageWaitMs = 0.0;
    double frameLimitWaitMs = 0.0;
    double otherGpuWaitMs = 0.0;
    double recordWaitMs = 0.0;
    uint64_t frontSleeps = 0; // waits for the front lock that slept, any thread
    double frontSleepMs = 0.0;
    bool gpuKnown = false;
    FrameGpuTimes gpu{};
};
constexpr uint32_t kSlowFrames = 3;
// The extra light passes' (muzzle flashes, explosions) strength as physical
// light: brighter than the game's colours alone, which were set for light
// added in display values (David, 2026-11-15: "the physical light with the
// flash made brighter").
constexpr float kLightPassStrength = 2.0f;
// How much brighter than white the white parts of lamps, lanterns and lit
// signs (unlit materials) are made, so that they glow and bloom as the light
// sources they are (David, 2026-11-15: the maps should look lit by their own
// lights).
constexpr float kEmissiveStrength = 4.0f;
} // namespace

struct Backend
{
    const Host *host = nullptr;
    WindowSystem windowSystem{};
    Core *core = nullptr;
    lg::Device *legacy = nullptr;
    // The GPU feed the device chose for itself, and the host's settings as
    // last taken up (BackendFrame).
    lg::GpuFeed ownFeed{};
    Tuning tuning{};
    // The counters when the statistics were last reset (BackendLogStats),
    // and when that was.
    lg::DeviceStats statsBase{};
    CoreStats coreBase{};
    std::chrono::steady_clock::time_point statsSince = std::chrono::steady_clock::now();
    // The frames that took longest since then, the longest first, and what
    // tells them: when the last Present began, the counters as they stood
    // there, and the frames counted since the reset (TrackFrame).
    SlowFrame slowFrames[kSlowFrames]{};
    bool presentSeen = false;
    std::chrono::steady_clock::time_point presentAt{};
    lg::DeviceStats presentStats{};
    CoreStats presentCore{};
    FrontLockSleeps presentSlept{};
    uint64_t framesTracked = 0;
    // The Modern path's anti-aliasing, made at its first scene; its counts
    // at the last reset; the reasons it could not run, each logged once.
    modern::Taa *taa = nullptr;
    modern::TaaStats taaBase{};
    bool taaFailed = false;
    std::string modernRefusals;
    // Linear light (vk_d3d9.h ModernSceneBegin): the scene image, made at the
    // first scene and again when the back buffer changes size; the tonemap;
    // whether it is wanted (KISAK_VK_MODERN_LINEAR=0 turns it off) or failed;
    // the scenes drawn in linear light and the copies read back from them, and
    // their counts at the last reset.
    bool linearWanted = true;
    bool linearFailed = false;
    // The tonemap's curve (KISAK_VK_MODERN_TONEMAP: clamp, neutral, agx or
    // aces; clamp when unset), and whether the variable named none of them.
    // Neutral unless KISAK_VK_MODERN_TONEMAP names another (David chose it by
    // eye, tonemap comparison 1, 2026-11-13).
    modern::TonemapCurve tonemapCurve = modern::TonemapCurve::Neutral;
    bool tonemapCurveUnknown = false;
    lg::Texture *linearScene = nullptr;
    lg::Surface *linearSceneSurface = nullptr;
    modern::Tonemap *tonemap = nullptr;
    uint64_t linearScenes = 0, linearScenesBase = 0;
    uint64_t linearCopies = 0, linearCopiesBase = 0;
    // Effects (the renderer's emissive surfaces) blend in linear light with
    // the rest of the scene with KISAK_VK_MODERN_EFFECTS=linear; otherwise
    // the scene is finished before them and they draw on the display image
    // (vk_d3d9.h ModernSceneLit).
    bool effectsInLinear = false;
    // Lit surfaces computing in linear light (vk_d3d9.h SetModernLitInputs),
    // unless KISAK_VK_MODERN_LIT=0: the linear-light scenes with lit draws and
    // the changes of lit inputs, and their counts at the last reset.
    bool litWanted = true;
    bool litThisScene = false;
    bool litOverWhite = false; // the last lit inputs were over white (not per input)
    // Lit passes drawn as Modern lit shaders of ours (vk_d3d9.h
    // ModernLitInputs::shader), unless KISAK_VK_MODERN_SHADERS=0, with the
    // sun's and the lightmap's strengths in linear light, 1 as bright as the
    // game's (KISAK_VK_MODERN_SUN_STRENGTH and _SKY_STRENGTH override them); the
    // changes of lit inputs that named one, and their count at the last reset.
    bool shadersWanted = true;
    // Models' (lp_) shaders of ours, unless KISAK_VK_MODERN_MODEL_SHADERS=0;
    // those for surfaces without the sun (interiors), unless
    // KISAK_VK_MODERN_NO_SUN_SHADERS=0; those for surfaces lit by a spot or
    // omni light, unless KISAK_VK_MODERN_LIGHT_SHADERS=0.
    bool modelShadersWanted = true;
    bool noSunShadersWanted = true;
    bool lightShadersWanted = true;
    // Water's (water_l_sun) shader of ours, unless KISAK_VK_MODERN_WATER=0
    // (at start-up only).
    bool waterShaderWanted = true;
    // The extra light passes' (l_) shader of ours, unless
    // KISAK_VK_MODERN_LIGHT_PASS=0, and its strength as physical light
    // (KISAK_VK_MODERN_LIGHT_PASS_STRENGTH; at start-up only).
    bool lightPassWanted = true;
    float lightPassStrength = kLightPassStrength;
    // Lamps and lit signs (ModernLitInputs::emissive) made brighter than
    // white, unless KISAK_VK_MODERN_EMISSIVE=0, by
    // KISAK_VK_MODERN_EMISSIVE_STRENGTH (at start-up only).
    bool emissiveWanted = true;
    float emissiveStrength = kEmissiveStrength;
    uint64_t emissiveChanges = 0, emissiveChangesBase = 0;
    float sunStrength = 1.0f;
    float skyStrength = 1.0f;
    uint64_t litShaderChanges = 0, litShaderChangesBase = 0;
    uint64_t litScenes = 0, litScenesBase = 0;
    uint64_t litChanges = 0, litChangesBase = 0;
    // Bloom from the linear-light scene in place of the renderer's glow
    // (vk_d3d9.h ModernSceneDone): made at the first scene with glow, unless
    // KISAK_VK_MODERN_BLOOM=0; the scenes it ran for, and at the last reset.
    // Auto exposure of the linear-light scene before the tonemap (vk_modern.h
    // MeasureExposure), unless KISAK_VK_MODERN_EXPOSURE=0; its parameters
    // (KISAK_VK_MODERN_EXPOSURE_KEY and _RANGE "min,max" in EV override the
    // key and range); when the last scene was measured.
    bool exposureWanted = true;
    bool exposureFailed = false;
    modern::Exposure *exposure = nullptr;
    modern::ExposureParams exposureParams{};
    std::chrono::steady_clock::time_point exposureLast{};
    // The exposure passes' GPU time at the last reset (pass timings add up
    // over the device's life, the exposure's scene count from the reset).
    double exposureMsBase = 0.0;
    bool bloomWanted = true;
    bool bloomFailed = false;
    modern::Bloom *bloom = nullptr;
    uint64_t bloomScenes = 0, bloomScenesBase = 0;
    uint64_t bloomPassesBase = 0;
    // The bloom passes' GPU time at the last reset (pass timings add up over
    // the device's life).
    double bloomMsBase = 0.0;
    // The velocity of what moves on its own (vk_d3d9.h ModernSurfaceMoved):
    // the pass, made at the first scene that has such surfaces; whether it is
    // wanted (KISAK_VK_MODERN_VELOCITY=0 turns it off) or failed; the
    // surfaces of the scene being drawn; its counts at the last reset.
    bool velocityWanted = true;
    bool velocityFailed = false;
    modern::Velocity *velocity = nullptr;
    std::vector<modern::MovedSurface> moved;
    modern::VelocityStats velocityBase{};
    // The pass was destroyed to be made again (ApplyModernTuning): its
    // counts stay in the statistics until the next is made.
    bool velocityRemade = false;
    // Skinned surfaces (vk_d3d9.h ModernSkinnedSurfaceMoved): those of the
    // scene being drawn, each with its key and whether it has a last matrix;
    // their vertices kept from one scene to the next; the counts at the last
    // reset.
    struct SkinnedSurface
    {
        modern::MovedSurface draw;
        uint64_t key;
        bool hasLast;
    };
    std::vector<SkinnedSurface> skinned;
    modern::KeptVertices *kept = nullptr;
    modern::KeptStats keptBase{};
    // The normals buffer (vk_modern.h BuildNormals), built from the depth the
    // game's depth prepass draws before the lit surfaces (vk_d3d9.h
    // ModernSceneDepth): wanted when something reads it (for now
    // KISAK_VK_MODERN_NORMALS=1 or the normals debug view);
    // KISAK_VK_MODERN_NORMALS=0 keeps it off whatever asks. The pass is made
    // at its first scene; sceneNormals is the scene's own until it is
    // finished; the counts at the last reset.
    bool normalsWanted = false;
    bool normalsFailed = false;
    modern::Normals *normals = nullptr;
    Image *sceneNormals = nullptr;
    modern::NormalsStats normalsBase{};
    // Ambient occlusion (vk_modern.h BuildAmbientOcclusion) from the depth and
    // the normals, recorded with them before the lit surfaces: wanted when
    // something reads it (our lit shaders, so with linear light, lit inputs
    // and our shaders on; KISAK_VK_MODERN_AO=1 or the ambient occlusion debug
    // view), which makes the normals buffer wanted too; KISAK_VK_MODERN_AO=0
    // keeps it off whatever asks. Its parameters (KISAK_VK_MODERN_AO_RADIUS,
    // _THICKNESS, _POWER, _GRAZING, _FLOOR, _EDGE); the scene-sized image it writes, a Legacy texture the lit
    // shaders read (lg::SetModernAmbientOcclusion) until the scene is lit;
    // whether this scene has it; the scenes counted at the last reset.
    bool aoWanted = false;
    bool aoFailed = false;
    modern::AmbientOcclusion *ao = nullptr;
    modern::AoParams aoParams{};
    lg::Texture *aoTexture = nullptr;
    lg::Surface *aoSurface = nullptr;
    bool sceneAo = false;
    uint32_t aoFrame = 0;
    modern::AoStats aoBase{};
    // Volumetric fog (vk_modern.h DrawFog): the map's fog lit by the sun and
    // the shadowed spot lights, drawn over the linear-light scene before its
    // anti-aliasing, where the view has fog. Off unless switched on
    // (r_modernFog, KISAK_VK_MODERN_FOG=1) or its debug view is; its
    // parameters (KISAK_VK_MODERN_FOG_SUN, _SCATTER, _SPOTS, _FROXELS live;
    // _STEPS, _MARCH, _HALF start-up only); the scenes counted at the last
    // reset.
    bool fogWanted = false;
    bool fogFailed = false;
    modern::Fog *fog = nullptr;
    modern::FogParams fogParams{};
    uint32_t fogFrame = 0;
    modern::FogStats fogBase{};
    // God rays (vk_modern.h DrawRays): shafts of the sky's light from the sun
    // on the screen, added to the linear-light scene after the fog, before
    // its anti-aliasing, where the view has a sun. Off unless switched on
    // (r_modernGodRays, KISAK_VK_MODERN_GOD_RAYS=1); their strength
    // (KISAK_VK_MODERN_GOD_RAYS_STRENGTH live; _SAMPLES start-up only); the
    // scenes counted at the last reset.
    bool raysWanted = false;
    bool raysFailed = false;
    modern::Rays *rays = nullptr;
    modern::RaysParams raysParams{};
    uint32_t raysFrame = 0;
    modern::RaysStats raysBase{};
    // The last Modern scene's fog and sun as the engine gave them, for the
    // statistics: what each map sets, to choose where the air could be
    // thicker.
    bool lastSceneSeen = false;
    bool lastFogOn = false;
    float lastFogStart = 0.0f, lastFogDensity = 0.0f, lastFogColor[3] = {};
    bool lastSunOn = false;
    float lastSunColor[3] = {}, lastSunDirection[3] = {};
    // Screen-space reflections (vk_modern.h TraceReflections), traced right
    // after the normals buffer from the prepass depth and last scene's
    // anti-aliased linear-light colour: wanted when something reads it (our
    // lit shaders, so with linear light, lit inputs and our shaders on;
    // KISAK_VK_MODERN_SSR=1 or the reflections debug view), which wants the
    // normals buffer too; KISAK_VK_MODERN_SSR=0 keeps it off. Its settings
    // (KISAK_VK_MODERN_SSR_DISTANCE, _THICKNESS, _STEPS, _LEVELS); the pass,
    // made at its first scene; sceneReflections is the scene's own until it
    // is finished, which the lit shaders read (lg::SetModernReflections)
    // until the scene is lit; the counts at the last reset.
    bool reflectionsWanted = false;
    bool reflectionsFailed = false;
    modern::ReflectionParams reflectionParams{};
    modern::Reflections *reflections = nullptr;
    Image *sceneReflections = nullptr;
    modern::ReflectionStats reflectionsBase{};
    // The game's reflection probes, filtered again in linear light as the
    // world loads (modern::FilterReflectionProbe): wanted with linear light
    // or KISAK_VK_MODERN_PROBES=1, never with KISAK_VK_MODERN_PROBES=0. The
    // counts since the device was made (a world loads before statistics are
    // started): filtered, left as they are (not 8-bit colour, too small),
    // and the time taken.
    // Soft sun shadows (docs/vulkan-modern.md "Soft sun shadows"): wanted
    // with our lit shaders unless switched on or off outright, and the sun's
    // apparent size in degrees; the scenes that used them, and the shadow
    // parameters' frame count, which turns the filter's discs.
    bool softShadowsWanted = false;
    float shadowSoftness = 1.5f;
    uint64_t softShadowScenes = 0, softShadowScenesBase = 0;
    // Soft spot-light shadows (docs/vulkan-modern.md "Soft spot-light
    // shadows"), wanted as the sun's, and the lights' size in world units;
    // the scenes that drew a shadowed spot or omni light's surfaces with our
    // lit shaders while they were on (not every scene with the switch on: a
    // view with no shadowed lamp in it counts none).
    bool softSpotShadowsWanted = false;
    float spotLightSize = 4.0f;
    uint64_t softSpotScenes = 0, softSpotScenesBase = 0;
    bool softSpotThisScene = false;
    uint32_t shadowFrame = 0;
    // Contact shadows (docs/vulkan-modern.md "Contact shadows"): wanted with
    // our lit shaders unless switched on or off outright, which makes the
    // normals buffer wanted too; traced into the green of the ambient
    // occlusion's texture (made for them when the occlusion is off) right
    // after the occlusion. Their parameters, the sun's direction from the
    // view's sunShadow (before its scene depth; contactSun false: no sun),
    // and whether this scene has them.
    bool contactWanted = false;
    modern::ContactParams contactParams{};
    bool contactSun = false;
    bool sceneContact = false;
    uint32_t contactFrame = 0;
    bool probesWanted = false;
    uint32_t probesFiltered = 0;
    uint32_t probesLeft = 0;
    double probesMs = 0.0;
    // Re-capturing the probes with the Modern path's own light (on with the
    // filter above, off with KISAK_VK_MODERN_PROBES_CAPTURE=0): while the
    // renderer captures (BackendModernProbeCapture), each scene's linear-light
    // image is read back before its bloom, exposure and tonemap, for
    // BackendModernProbeCaptureRead. Counts since the device was made: scenes
    // read back, and rectangles taken from them.
    bool probeCaptureWanted = false;
    bool probeCapturing = false;
    Readback *probeCaptureReadback = nullptr;
    uint32_t probeCaptureScenes = 0;
    uint32_t probeCaptureReads = 0;
    // KISAK_VK_MODERN_DEBUG=velocity: the velocity debug view over each
    // anti-aliased scene (docs/vulkan-modern.md), made at its first use.
    // velocity-all: the same, with every surface that moves on its own drawn
    // wherever it lands, hidden or not (the velocity pass skips its depth
    // test), to find them on the screen.
    // KISAK_VK_MODERN_DEBUG=normals: the normals debug view over each
    // finished scene (on the display image, after the tonemap).
    bool debugVelocity = false;
    bool debugVelocityAll = false;
    bool debugNormals = false;
    // KISAK_VK_MODERN_DEBUG=ao: the ambient occlusion as grey over each
    // finished scene.
    bool debugAo = false;
    // KISAK_VK_MODERN_DEBUG=reflections: the reflections debug view over each
    // finished scene that has them (on the display image).
    bool debugReflections = false;
    // KISAK_VK_MODERN_DEBUG=fog: the light the volumetric fog adds, over
    // grey, in place of each scene.
    bool debugFog = false;
    // Planar reflections (docs/vulkan-modern.md "Planar reflections"): wanted
    // with KISAK_VK_MODERN_PLANAR=1 or KISAK_VK_MODERN_DEBUG=planar or
    // planar-read, in linear light only. The renderer draws the planar view into planarColor (linear
    // light) with its own depth after each main view; planarReady once one
    // was drawn, planarView the last. KISAK_VK_MODERN_DEBUG=planar draws the
    // planar image (as it is, upside down) over the top left of
    // each finished scene.
    bool planarWanted = false;
    bool planarFailed = false;
    bool debugPlanar = false;
    bool debugPlanarRead = false; // KISAK_VK_MODERN_DEBUG=planar-read
    lg::Texture *planarColor = nullptr;
    lg::Surface *planarColorSurface = nullptr;
    lg::Surface *planarDepth = nullptr;
    bool planarReady = false;
    ModernPlanarView planarView{};
    uint64_t planarViews = 0, planarViewsBase = 0;
    // How far from the plane (units) a lit surface still reads the planar
    // image, and how far its normal map's tilt moves the point it reads
    // (KISAK_VK_MODERN_PLANAR_REACH, _RIPPLE).
    float planarReach = 4.0f;
    float planarRipple = 4.0f;
    // Scenes whose lit shaders read the planar image (SetScenePlanar).
    uint64_t planarReads = 0, planarReadsBase = 0;
    bool debugFailed = false;
    modern::DebugView *debug = nullptr;
    // KISAK_VK_MODERN_DEBUG=compute (start-up only): the compute check
    // (vk_modern.h) builds its volume at each scene's lit point and draws it
    // over the finished scene, made at its first use.
    bool debugCompute = false;
    bool computeCheckFailed = false;
    modern::ComputeCheck *computeCheck = nullptr;
    modern::ComputeCheckStats computeCheckBase{};
    // The Modern switches the device started with (the KISAK_VK_MODERN_*
    // variables, or the built-in defaults), which a ModernTuning field of -1
    // keeps (ApplyModernTuning), and the Modern tuning last taken up. The
    // variables that turn a pass on or off whatever asks are kept as -1
    // (unset: on when something reads it), 0 or 1.
    struct ModernStart
    {
        bool bloom = true, exposure = true, velocity = true;
        bool shaders = true, modelShaders = true, noSunShaders = true, lightShaders = true;
        int ao = -1, reflections = -1, normals = -1, fog = -1, rays = -1, softShadows = -1, contactShadows = -1,
            softSpotShadows = -1;
        float shadowSoftness = 1.5f;
        float spotLightSize = 4.0f;
        float contactLength = 12.0f;
        ModernDebugView debug = ModernDebugNone;
        modern::TonemapCurve curve = modern::TonemapCurve::Neutral;
        float sunStrength = 1.0f, skyStrength = 1.0f;
        modern::ExposureParams exposureParams{};
        modern::AoParams aoParams{};
        modern::ReflectionParams reflectionParams{};
        modern::FogParams fogParams{};
        modern::RaysParams raysParams{};
    } modernStart;
    ModernTuning modernTuning{};
};

namespace
{
// Called as a Present begins. The time since the Present before is a frame as
// the player sees it: the engine's work on it, the device's, and every wait
// in between (the Present before included). The longest few are kept with
// what the device's counters say happened in them, so that one long frame in
// a run of thousands can be told from its cause: a pipeline built, a shader
// translated, a texture uploaded or read back, memory allocated, a wait for
// the GPU, the swapchain or the record thread, or none of those, which
// leaves the engine and the driver.
void TrackFrame(Backend &backend)
{
    const auto now = std::chrono::steady_clock::now();
    const lg::DeviceStats &stats = lg::Stats(backend.legacy);
    const CoreStats &core = Stats(backend.core);
    // A frame's GPU times can be read a few frames after it.
    for (SlowFrame &frame : backend.slowFrames)
    {
        if (frame.ms > 0.0 && !frame.gpuKnown)
            frame.gpuKnown = FrameGpuTime(backend.core, frame.presentedAs, frame.gpu);
    }
    if (backend.presentSeen)
    {
        ++backend.framesTracked;
        const double ms = std::chrono::duration<double, std::milli>(now - backend.presentAt).count();
        if (ms > backend.slowFrames[kSlowFrames - 1].ms)
        {
            const lg::DeviceStats &was = backend.presentStats;
            const CoreStats &coreWas = backend.presentCore;
            SlowFrame frame;
            frame.ms = ms;
            frame.number = backend.framesTracked;
            frame.seconds = std::chrono::duration<double>(now - backend.statsSince).count();
            // The Present that begins now gives the frame this number, unless
            // it shows nothing (a minimised window): then the next frame gets
            // it, and the GPU times printed are that frame's.
            frame.presentedAs = core.framesPresented + 1;
            frame.draws = stats.draws - was.draws;
            frame.passes = stats.passes - was.passes;
            frame.submissions = core.submissions - coreWas.submissions;
            frame.dynamicRenames = stats.dynamicRenames - was.dynamicRenames;
            frame.pipelines = stats.pipelines - was.pipelines;
            frame.pipelineCacheHits = core.pipelineCacheHits - coreWas.pipelineCacheHits;
            frame.pipelineMs = double(stats.pipelineMicros - was.pipelineMicros) / 1000.0;
            frame.pixelModules = stats.pixelModules - was.pixelModules;
            frame.pixelModuleMs = double(stats.pixelModuleMicros - was.pixelModuleMicros) / 1000.0;
            frame.aheadWaits = stats.aheadWaits - was.aheadWaits;
            frame.aheadWaitMs = double(stats.aheadWaitMicros - was.aheadWaitMicros) / 1000.0;
            frame.shaders = stats.shaders - was.shaders;
            frame.shaderMs = double(stats.shaderMicros - was.shaderMicros) / 1000.0;
            frame.uploads = stats.textureUploads - was.textureUploads;
            frame.uploadMegabytes = double(stats.uploadBytes - was.uploadBytes) / (1024.0 * 1024.0);
            frame.uploadMs = double(stats.uploadMicros - was.uploadMicros) / 1000.0;
            frame.readbacks = stats.textureReadbacks - was.textureReadbacks;
            frame.memoryAllocations = core.memoryAllocations - coreWas.memoryAllocations;
            frame.memoryAllocateMs = core.memoryAllocateMs - coreWas.memoryAllocateMs;
            frame.swapchainBuilds = core.swapchainBuilds - coreWas.swapchainBuilds;
            frame.imageWaitMs = core.imageWaitMs - coreWas.imageWaitMs;
            frame.frameLimitWaitMs = core.frameLimitWaitMs - coreWas.frameLimitWaitMs;
            frame.otherGpuWaitMs =
                std::max(0.0, (core.gpuWaitMs - coreWas.gpuWaitMs) - frame.frameLimitWaitMs);
            frame.recordWaitMs = core.recordWaitMs - coreWas.recordWaitMs;
            const FrontLockSleeps slept = FrontLockSlept();
            frame.frontSleeps = slept.sleeps - backend.presentSlept.sleeps;
            frame.frontSleepMs = double(slept.micros - backend.presentSlept.micros) / 1000.0;
            uint32_t place = kSlowFrames - 1;
            for (; place > 0 && backend.slowFrames[place - 1].ms < ms; --place)
                backend.slowFrames[place] = backend.slowFrames[place - 1];
            backend.slowFrames[place] = frame;
        }
    }
    backend.presentSeen = true;
    backend.presentAt = now;
    backend.presentStats = stats;
    backend.presentCore = core;
    backend.presentSlept = FrontLockSlept();
}

void LogSlowFrames(const Backend &backend)
{
    const Host &h = *backend.host;
    for (uint32_t i = 0; i < kSlowFrames && backend.slowFrames[i].ms > 0.0; ++i)
    {
        const SlowFrame &frame = backend.slowFrames[i];
        char gpu[160];
        if (frame.gpuKnown && !Features(backend.core).passTimestamps)
            std::snprintf(gpu, sizeof(gpu), "GPU %.2f ms (passes not timed), %.2f without work before it",
                          frame.gpu.frameMs, frame.gpu.gapMs);
        else if (frame.gpuKnown)
            std::snprintf(gpu, sizeof(gpu),
                          "GPU %.2f ms: %.2f in passes, %.2f between them, %.2f without work before it",
                          frame.gpu.frameMs, frame.gpu.passesMs,
                          std::max(0.0, frame.gpu.frameMs - frame.gpu.passesMs), frame.gpu.gapMs);
        else
            std::snprintf(gpu, sizeof(gpu), "GPU time not read");
        HostLog(h,
                "vulkan d3d9: slow frame %u: %.2f ms, frame %llu at %.2f s; %llu draws, %llu passes, %llu submissions, "
                "%llu dynamic renames, %u swapchains built; of the time: %u pipelines (%u from the cache) %.2f ms "
                "(%u pixel shaders built for them %.2f ms), %u waited for from workers %.2f ms, "
                "%u shaders %.2f ms, %llu texture uploads (%.2f MB) %.2f ms, %llu memory blocks %.2f ms; waits: "
                "swapchain image %.2f ms, frames in flight %.2f ms, GPU otherwise %.2f ms (%llu textures read back), "
                "record thread %.2f ms, front lock %llu sleeps %.2f ms (any thread); %s",
                i + 1, frame.ms, static_cast<unsigned long long>(frame.number), frame.seconds,
                static_cast<unsigned long long>(frame.draws), static_cast<unsigned long long>(frame.passes),
                static_cast<unsigned long long>(frame.submissions),
                static_cast<unsigned long long>(frame.dynamicRenames), frame.swapchainBuilds, frame.pipelines,
                frame.pipelineCacheHits, frame.pipelineMs, frame.pixelModules, frame.pixelModuleMs, frame.aheadWaits,
                frame.aheadWaitMs, frame.shaders,
                frame.shaderMs,
                static_cast<unsigned long long>(frame.uploads), frame.uploadMegabytes, frame.uploadMs,
                static_cast<unsigned long long>(frame.memoryAllocations), frame.memoryAllocateMs, frame.imageWaitMs,
                frame.frameLimitWaitMs, frame.otherGpuWaitMs, static_cast<unsigned long long>(frame.readbacks),
                frame.recordWaitMs, static_cast<unsigned long long>(frame.frontSleeps), frame.frontSleepMs, gpu);
    }
}
} // namespace

namespace
{
// Start-up switches the program set in place of the environment
// (SetStartSetting): looked up before the environment.
std::mutex s_startSettingsLock;
std::map<std::string, std::string> s_startSettings;

bool StartSetting(const char *name, std::string &out)
{
    std::lock_guard<std::mutex> lock(s_startSettingsLock);
    const auto found = s_startSettings.find(name);
    if (found == s_startSettings.end())
        return false;
    out = found->second;
    return true;
}

// Whether the environment variable is set to "0".
bool EnvIs(const char *name, const char *value)
{
    std::string set;
    if (StartSetting(name, set))
        return set == value;
#if defined(_MSC_VER)
    char *text = nullptr;
    size_t length = 0;
    const bool same = _dupenv_s(&text, &length, name) == 0 && text && std::strcmp(text, value) == 0;
    std::free(text);
    return same;
#else
    const char *text = std::getenv(name);
    return text && std::strcmp(text, value) == 0;
#endif
}

bool EnvIsZero(const char *name) { return EnvIs(name, "0"); }

// The variable's text, empty when unset.
std::string EnvText(const char *name)
{
    std::string set;
    if (StartSetting(name, set))
        return set;
#if defined(_MSC_VER)
    char *text = nullptr;
    size_t length = 0;
    std::string value = _dupenv_s(&text, &length, name) == 0 && text ? text : "";
    std::free(text);
    return value;
#else
    const char *text = std::getenv(name);
    return text ? text : "";
#endif
}

const char *DebugViewName(ModernDebugView view)
{
    switch (view)
    {
    case ModernDebugVelocity:
        return "velocity";
    case ModernDebugVelocityAll:
        return "velocity-all";
    case ModernDebugNormals:
        return "normals";
    case ModernDebugAo:
        return "ao";
    case ModernDebugReflections:
        return "reflections";
    case ModernDebugFog:
        return "fog";
    default:
        return "none";
    }
}

// Takes up the Modern switches: each field of `tuning` that is not negative
// (or, for the numbers, not outside what the variable of the same name
// takes) in place of what the device started with. Called as the device
// opens and then at a Present whose tuning changed (BackendFrame), so a scene
// is drawn with one set of switches throughout. A pass turned off is not
// destroyed (turned on again, it carries on); its image stops being read at
// once. With `log`, says what the switches are now.
void ApplyModernTuning(Backend *backend, const ModernTuning &tuning, bool log = false)
{
    const Backend::ModernStart &start = backend->modernStart;
    const auto pick = [](int value, bool startValue) { return value >= 0 ? value != 0 : startValue; };
    backend->bloomWanted = pick(tuning.bloom, start.bloom);
    backend->exposureWanted = pick(tuning.exposure, start.exposure);
    backend->velocityWanted = pick(tuning.velocity, start.velocity);
    backend->shadersWanted = pick(tuning.shaders, start.shaders);
    backend->modelShadersWanted = pick(tuning.modelShaders, start.modelShaders);
    backend->noSunShadersWanted = pick(tuning.noSunShaders, start.noSunShaders);
    backend->lightShadersWanted = pick(tuning.lightShaders, start.lightShaders);

    const ModernDebugView view = tuning.debug >= ModernDebugNone && tuning.debug <= ModernDebugFog
                                     ? static_cast<ModernDebugView>(tuning.debug)
                                     : start.debug;
    const bool velocityAllBefore = backend->debugVelocityAll;
    backend->debugVelocityAll = view == ModernDebugVelocityAll;
    backend->debugVelocity = view == ModernDebugVelocity || view == ModernDebugVelocityAll;
    backend->debugNormals = view == ModernDebugNormals;
    backend->debugAo = view == ModernDebugAo;
    backend->debugReflections = view == ModernDebugReflections;
    backend->debugFog = view == ModernDebugFog;
    // The velocity pass takes its depth test when it is made: made again at
    // its next scene (a wait for the GPU, once; only the debug view asks).
    if (velocityAllBefore != backend->debugVelocityAll && backend->velocity)
    {
        // Its counts carry over to the next (unsigned: the base wraps back
        // when the new pass counts up again).
        const modern::VelocityStats was = modern::Stats(backend->velocity);
        backend->velocityBase.passes -= was.passes;
        backend->velocityBase.surfaces -= was.surfaces;
        backend->velocityBase.skinned -= was.skinned;
        backend->velocityBase.world -= was.world;
        backend->velocityRemade = true;
        modern::DestroyVelocity(backend->core, backend->velocity);
        backend->velocity = nullptr;
    }

    // Ambient occlusion and reflections are wanted for what reads them unless
    // switched on or off outright; the normals buffer for those.
    const bool ours = backend->linearWanted && backend->litWanted && backend->shadersWanted;
    const int ao = tuning.ao >= 0 ? tuning.ao : start.ao;
    backend->aoWanted = ao == 1 || (ao < 0 && (backend->debugAo || ours));
    const int reflections = tuning.reflections >= 0 ? tuning.reflections : start.reflections;
    backend->reflectionsWanted = reflections == 1 || (reflections < 0 && (backend->debugReflections || ours));
    const int contact = tuning.contactShadows >= 0 ? tuning.contactShadows : start.contactShadows;
    backend->contactWanted = contact == 1 || (contact < 0 && ours);
    backend->normalsWanted = start.normals != 0 && (start.normals == 1 || backend->debugNormals || backend->aoWanted
                                                    || backend->reflectionsWanted || backend->planarWanted
                                                    || backend->contactWanted);
    const int softShadows = tuning.softShadows >= 0 ? tuning.softShadows : start.softShadows;
    backend->softShadowsWanted = softShadows == 1 || (softShadows < 0 && ours);
    const int softSpot = tuning.softSpotShadows >= 0 ? tuning.softSpotShadows : start.softSpotShadows;
    backend->softSpotShadowsWanted = softSpot == 1 || (softSpot < 0 && ours);
    if (!backend->softShadowsWanted && !backend->softSpotShadowsWanted && backend->legacy)
        lg::SetModernShadow(backend->legacy, nullptr);
    if (backend->legacy)
    {
        if (!backend->aoWanted || !backend->normalsWanted)
            backend->sceneAo = false;
        if (!backend->contactWanted || !backend->normalsWanted)
            backend->sceneContact = false;
        if (!backend->sceneAo && !backend->sceneContact)
            lg::SetModernAmbientOcclusion(backend->legacy, nullptr);
        if (!backend->reflectionsWanted || !backend->normalsWanted)
        {
            backend->sceneReflections = nullptr;
            lg::SetModernReflections(backend->legacy, nullptr);
        }
    }
    if (!backend->normalsWanted)
        backend->sceneNormals = nullptr;

    const auto number = [](float value, float startValue, float low, float high, bool lowIn) {
        return (lowIn ? value >= low : value > low) && value <= high ? value : startValue;
    };
    constexpr float kAny = 1.0e9f;
    const float sun = number(tuning.sunStrength, start.sunStrength, 0.0f, kAny, true);
    const float sky = number(tuning.skyStrength, start.skyStrength, 0.0f, kAny, true);
    if (sun != backend->sunStrength || sky != backend->skyStrength)
    {
        backend->sunStrength = sun;
        backend->skyStrength = sky;
        if (backend->legacy)
            lg::SetModernLitStrengths(backend->legacy, sun, sky);
    }
    backend->exposureParams.key = number(tuning.exposureKey, start.exposureParams.key, 0.0f, kAny, false);
    backend->exposureParams.minEv = start.exposureParams.minEv;
    backend->exposureParams.maxEv = start.exposureParams.maxEv;
    modern::AoParams &aoParams = backend->aoParams;
    aoParams = start.aoParams;
    aoParams.radius = number(tuning.aoRadius, start.aoParams.radius, 0.0f, kAny, false);
    aoParams.power = number(tuning.aoPower, start.aoParams.power, 0.0f, kAny, true);
    aoParams.thickness = number(tuning.aoThickness, start.aoParams.thickness, 0.0f, kAny, false);
    aoParams.grazing = number(tuning.aoGrazing, start.aoParams.grazing, 0.0f, 1.0f, false);
    aoParams.floor = tuning.aoFloor >= 0.0f && tuning.aoFloor < 1.0f ? tuning.aoFloor : start.aoParams.floor;
    aoParams.edge = number(tuning.aoEdge, start.aoParams.edge, 0.0f, 10.0f, false);
    aoParams.half = tuning.aoHalf >= 0 ? tuning.aoHalf == 1 : start.aoParams.half;
    modern::ReflectionParams &reflectionParams = backend->reflectionParams;
    reflectionParams = start.reflectionParams;
    reflectionParams.maxDistance = number(tuning.ssrDistance, start.reflectionParams.maxDistance, 0.0f, kAny, false);
    reflectionParams.thickness = number(tuning.ssrThickness, start.reflectionParams.thickness, 0.0f, kAny, false);
    // Volumetric fog is off unless switched on (its debug view switches it on).
    const int fog = tuning.fog >= 0 ? tuning.fog : start.fog;
    backend->fogWanted = backend->linearWanted && (fog == 1 || (fog < 0 && backend->debugFog));
    modern::FogParams &fogParams = backend->fogParams;
    fogParams = start.fogParams;
    fogParams.sunShare = number(tuning.fogSun, start.fogParams.sunShare, 0.0f, 1.0f, true);
    fogParams.scatter = tuning.fogScatter >= 0.0f && tuning.fogScatter < 0.95f ? tuning.fogScatter
                                                                                 : start.fogParams.scatter;
    fogParams.spotShare = number(tuning.fogSpots, start.fogParams.spotShare, 0.0f, 4.0f, true);
    fogParams.froxels = tuning.fogFroxels >= 0 ? tuning.fogFroxels == 1 : start.fogParams.froxels;
    // God rays are off unless switched on.
    const int rays = tuning.godRays >= 0 ? tuning.godRays : start.rays;
    backend->raysWanted = backend->linearWanted && rays == 1;
    modern::RaysParams &raysParams = backend->raysParams;
    raysParams = start.raysParams;
    raysParams.strength = number(tuning.godRaysStrength, start.raysParams.strength, 0.0f, 8.0f, true);
    raysParams.spread = number(tuning.godRaysSpread, start.raysParams.spread, 1.0f, 512.0f, true);
    raysParams.threshold = number(tuning.godRaysThreshold, start.raysParams.threshold, 0.0f, 16.0f, true);
    raysParams.sky = number(tuning.godRaysSky, start.raysParams.sky, 0.0f, 1.0f, true);
    backend->shadowSoftness = number(tuning.shadowSoftness, start.shadowSoftness, 0.0f, 20.0f, false);
    backend->contactParams.length = number(tuning.contactLength, start.contactLength, 0.0f, 256.0f, false);
    backend->spotLightSize = number(tuning.spotLightSize, start.spotLightSize, 0.0f, 64.0f, false);

    backend->tonemapCurve = tuning.tonemap >= static_cast<int>(modern::TonemapCurve::Clamp)
                                    && tuning.tonemap <= static_cast<int>(modern::TonemapCurve::Aces)
                                ? static_cast<modern::TonemapCurve>(tuning.tonemap)
                                : start.curve;
    modern::SetCurve(backend->tonemap, backend->tonemapCurve);

    if (!log)
        return;
    const auto onOff = [](bool on) { return on ? "on" : "off"; };
    HostLog(*backend->host,
            "vulkan d3d9: modern: switches now: bloom %s, exposure %s (key %.3f), velocity %s, our shaders %s "
            "(models %s, without the sun %s, lights %s; sun %.2f, sky %.2f), ambient occlusion %s (radius %.1f, power %.2f, "
            "thickness %.1f, grazing %.2f, floor %.2f, edge %.3f%s), reflections %s (distance %.0f, thickness %.1f), normals %s, "
            "volumetric fog %s (%s, sun %.2f, scatter %.2f, spot lights %.2f), god rays %s (strength %.2f, "
            "spread %.0f, threshold %.2f, sky %.2f), soft shadows %s (sun %.2f degrees), "
            "soft spot-light shadows %s (lights %.1f units), contact shadows %s (length %.1f), tonemap %s, debug view %s",
            onOff(backend->bloomWanted), onOff(backend->exposureWanted), backend->exposureParams.key,
            onOff(backend->velocityWanted), onOff(backend->shadersWanted), onOff(backend->modelShadersWanted),
            onOff(backend->noSunShadersWanted), onOff(backend->lightShadersWanted), backend->sunStrength, backend->skyStrength, onOff(backend->aoWanted),
            aoParams.radius, aoParams.power, aoParams.thickness, aoParams.grazing, aoParams.floor, aoParams.edge,
            aoParams.half ? ", half resolution" : "",
            onOff(backend->reflectionsWanted), reflectionParams.maxDistance, reflectionParams.thickness,
            onOff(backend->normalsWanted), onOff(backend->fogWanted), fogParams.froxels ? "froxels" : "march",
            fogParams.sunShare, fogParams.scatter, fogParams.spotShare, onOff(backend->raysWanted), raysParams.strength,
            raysParams.spread, raysParams.threshold, raysParams.sky, onOff(backend->softShadowsWanted),
            backend->shadowSoftness, onOff(backend->softSpotShadowsWanted), backend->spotLightSize,
            onOff(backend->contactWanted), backend->contactParams.length, modern::CurveName(backend->tonemapCurve),
            DebugViewName(view));
}
} // namespace

void SetStartSetting(const char *name, const char *value)
{
    std::lock_guard<std::mutex> lock(s_startSettingsLock);
    if (!value || !*value)
        s_startSettings.erase(name);
    else
        s_startSettings[name] = value;
}

Backend *OpenBackend(const BackendDesc &desc, std::string &error)
{
    const Host &h = *desc.host;
    Backend *backend = new Backend;
    backend->host = desc.host;
    // The Modern path draws its scenes in linear light unless this says not to
    // (docs/vulkan-modern.md); without r_visuals modern nothing asks for it.
    // Linear light, lit inputs and where effects blend are kept for the
    // device's life; the rest are where the device starts, and the engine's
    // r_modern* dvars may change them from one frame to the next
    // (ApplyModernTuning).
    backend->linearWanted = !EnvIsZero("KISAK_VK_MODERN_LINEAR");
    backend->effectsInLinear = EnvIs("KISAK_VK_MODERN_EFFECTS", "linear");
    backend->litWanted = !EnvIsZero("KISAK_VK_MODERN_LIT");
    Backend::ModernStart &start = backend->modernStart;
    start.bloom = !EnvIsZero("KISAK_VK_MODERN_BLOOM");
    // And gives what moves on its own a velocity of its own unless this says not to.
    start.velocity = !EnvIsZero("KISAK_VK_MODERN_VELOCITY");
    start.exposure = !EnvIsZero("KISAK_VK_MODERN_EXPOSURE");
    start.shaders = !EnvIsZero("KISAK_VK_MODERN_SHADERS");
    start.modelShaders = !EnvIsZero("KISAK_VK_MODERN_MODEL_SHADERS");
    start.noSunShaders = !EnvIsZero("KISAK_VK_MODERN_NO_SUN_SHADERS");
    start.lightShaders = !EnvIsZero("KISAK_VK_MODERN_LIGHT_SHADERS");
    {
        const std::string sun = EnvText("KISAK_VK_MODERN_SUN_STRENGTH");
        const float sunValue = sun.empty() ? -1.0f : std::strtof(sun.c_str(), nullptr);
        if (sunValue >= 0.0f)
            start.sunStrength = sunValue;
        const std::string sky = EnvText("KISAK_VK_MODERN_SKY_STRENGTH");
        const float skyValue = sky.empty() ? -1.0f : std::strtof(sky.c_str(), nullptr);
        if (skyValue >= 0.0f)
            start.skyStrength = skyValue;
    }
    {
        const std::string key = EnvText("KISAK_VK_MODERN_EXPOSURE_KEY");
        const float value = key.empty() ? 0.0f : std::strtof(key.c_str(), nullptr);
        if (value > 0.0f)
            start.exposureParams.key = value;
        const std::string range = EnvText("KISAK_VK_MODERN_EXPOSURE_RANGE");
        char *end = nullptr;
        const float low = range.empty() ? 0.0f : std::strtof(range.c_str(), &end);
        const float high = end && *end == ',' ? std::strtof(end + 1, nullptr) : low - 1.0f;
        if (!range.empty() && low <= high)
        {
            start.exposureParams.minEv = low;
            start.exposureParams.maxEv = high;
        }
    }
    {
        const std::string curve = EnvText("KISAK_VK_MODERN_TONEMAP");
        if (!curve.empty() && !modern::CurveFromName(curve.c_str(), start.curve))
            backend->tonemapCurveUnknown = true;
    }
    {
        const std::string debug = EnvText("KISAK_VK_MODERN_DEBUG");
        start.debug = debug == "velocity"       ? ModernDebugVelocity
                      : debug == "velocity-all" ? ModernDebugVelocityAll
                      : debug == "normals"      ? ModernDebugNormals
                      : debug == "ao"           ? ModernDebugAo
                      : debug == "reflections"  ? ModernDebugReflections
                      : debug == "fog"          ? ModernDebugFog
                                                : ModernDebugNone;
    }
    const auto onOff = [](const char *name) { return EnvIs(name, "0") ? 0 : EnvIs(name, "1") ? 1 : -1; };
    start.ao = onOff("KISAK_VK_MODERN_AO");
    {
        const std::string radius = EnvText("KISAK_VK_MODERN_AO_RADIUS");
        const float value = radius.empty() ? 0.0f : std::strtof(radius.c_str(), nullptr);
        if (value > 0.0f)
            start.aoParams.radius = value;
        const std::string power = EnvText("KISAK_VK_MODERN_AO_POWER");
        const float strength = power.empty() ? -1.0f : std::strtof(power.c_str(), nullptr);
        if (strength >= 0.0f)
            start.aoParams.power = strength;
        const std::string thickness = EnvText("KISAK_VK_MODERN_AO_THICKNESS");
        const float depth = thickness.empty() ? 0.0f : std::strtof(thickness.c_str(), nullptr);
        if (depth > 0.0f)
            start.aoParams.thickness = depth;
        const std::string grazing = EnvText("KISAK_VK_MODERN_AO_GRAZING");
        const float angle = grazing.empty() ? 0.0f : std::strtof(grazing.c_str(), nullptr);
        if (angle > 0.0f && angle <= 1.0f)
            start.aoParams.grazing = angle;
        const std::string floorText = EnvText("KISAK_VK_MODERN_AO_FLOOR");
        const float floorValue = floorText.empty() ? -1.0f : std::strtof(floorText.c_str(), nullptr);
        if (floorValue >= 0.0f && floorValue < 1.0f)
            start.aoParams.floor = floorValue;
        const std::string edgeText = EnvText("KISAK_VK_MODERN_AO_EDGE");
        const float edge = edgeText.empty() ? 0.0f : std::strtof(edgeText.c_str(), nullptr);
        if (edge > 0.0f && edge <= 10.0f)
            start.aoParams.edge = edge;
        start.aoParams.half = !EnvIsZero("KISAK_VK_MODERN_AO_HALF");
    }
    start.fog = onOff("KISAK_VK_MODERN_FOG");
    {
        modern::FogParams &fog = start.fogParams;
        const std::string sun = EnvText("KISAK_VK_MODERN_FOG_SUN");
        const float share = sun.empty() ? -1.0f : std::strtof(sun.c_str(), nullptr);
        if (share >= 0.0f && share <= 1.0f)
            fog.sunShare = share;
        const std::string scatter = EnvText("KISAK_VK_MODERN_FOG_SCATTER");
        const float g = scatter.empty() ? -1.0f : std::strtof(scatter.c_str(), nullptr);
        if (g >= 0.0f && g < 0.95f)
            fog.scatter = g;
        const std::string spots = EnvText("KISAK_VK_MODERN_FOG_SPOTS");
        const float spotShare = spots.empty() ? -1.0f : std::strtof(spots.c_str(), nullptr);
        if (spotShare >= 0.0f && spotShare <= 4.0f)
            fog.spotShare = spotShare;
        fog.froxels = EnvIs("KISAK_VK_MODERN_FOG_FROXELS", "1");
        const std::string steps = EnvText("KISAK_VK_MODERN_FOG_STEPS");
        const long count = steps.empty() ? 0 : std::strtol(steps.c_str(), nullptr, 10);
        if (count >= 4 && count <= 128)
            fog.steps = static_cast<uint32_t>(count);
        const std::string march = EnvText("KISAK_VK_MODERN_FOG_MARCH");
        const float length = march.empty() ? 0.0f : std::strtof(march.c_str(), nullptr);
        if (length > 0.0f)
            fog.march = length;
        fog.half = !EnvIsZero("KISAK_VK_MODERN_FOG_HALF");
    }
    start.rays = onOff("KISAK_VK_MODERN_GOD_RAYS");
    {
        modern::RaysParams &rays = start.raysParams;
        const std::string strength = EnvText("KISAK_VK_MODERN_GOD_RAYS_STRENGTH");
        const float value = strength.empty() ? -1.0f : std::strtof(strength.c_str(), nullptr);
        if (value >= 0.0f && value <= 8.0f)
            rays.strength = value;
        const std::string samples = EnvText("KISAK_VK_MODERN_GOD_RAYS_SAMPLES");
        const long count = samples.empty() ? 0 : std::strtol(samples.c_str(), nullptr, 10);
        if (count >= 8 && count <= 128)
            rays.samples = static_cast<uint32_t>(count);
        const auto number = [](const char *name, float low, float high, float &out) {
            const std::string text = EnvText(name);
            const float read = text.empty() ? -1.0f : std::strtof(text.c_str(), nullptr);
            if (read >= low && read <= high)
                out = read;
        };
        number("KISAK_VK_MODERN_GOD_RAYS_SPREAD", 1.0f, 512.0f, rays.spread);
        number("KISAK_VK_MODERN_GOD_RAYS_THRESHOLD", 0.0f, 16.0f, rays.threshold);
        number("KISAK_VK_MODERN_GOD_RAYS_SKY", 0.0f, 1.0f, rays.sky);
    }
    // Reflection probes are filtered as their textures load, so this one
    // stays start-up only.
    backend->probesWanted = !EnvIsZero("KISAK_VK_MODERN_PROBES")
        && (EnvIs("KISAK_VK_MODERN_PROBES", "1") || backend->linearWanted);
    backend->probeCaptureWanted =
        backend->probesWanted && backend->linearWanted && !EnvIsZero("KISAK_VK_MODERN_PROBES_CAPTURE");
    // The planar view and its debug view are start-up only, as the probes.
    backend->debugPlanar = EnvIs("KISAK_VK_MODERN_DEBUG", "planar");
    backend->debugCompute = EnvIs("KISAK_VK_MODERN_DEBUG", "compute");
    backend->debugPlanarRead = EnvIs("KISAK_VK_MODERN_DEBUG", "planar-read");
    backend->waterShaderWanted = !EnvIsZero("KISAK_VK_MODERN_WATER");
    backend->lightPassWanted = !EnvIsZero("KISAK_VK_MODERN_LIGHT_PASS");
    {
        const std::string strength = EnvText("KISAK_VK_MODERN_LIGHT_PASS_STRENGTH");
        const float value = strength.empty() ? -1.0f : std::strtof(strength.c_str(), nullptr);
        backend->lightPassStrength = value >= 0.0f ? value : kLightPassStrength;
    }
    backend->emissiveWanted = !EnvIsZero("KISAK_VK_MODERN_EMISSIVE");
    {
        const std::string strength = EnvText("KISAK_VK_MODERN_EMISSIVE_STRENGTH");
        const float value = strength.empty() ? -1.0f : std::strtof(strength.c_str(), nullptr);
        backend->emissiveStrength = value >= 1.0f ? std::min(value, 63.75f) : kEmissiveStrength;
    }
    backend->planarWanted = backend->linearWanted && !EnvIsZero("KISAK_VK_MODERN_PLANAR")
                            && (EnvIs("KISAK_VK_MODERN_PLANAR", "1") || backend->debugPlanar
                                || backend->debugPlanarRead);
    start.reflections = onOff("KISAK_VK_MODERN_SSR");
    {
        modern::ReflectionParams &p = start.reflectionParams;
        const auto number = [](const char *name, float &out) {
            const std::string text = EnvText(name);
            const float value = text.empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
            if (value > 0.0f)
                out = value;
        };
        float steps = static_cast<float>(p.steps), levels = static_cast<float>(p.levels);
        number("KISAK_VK_MODERN_SSR_DISTANCE", p.maxDistance);
        number("KISAK_VK_MODERN_SSR_THICKNESS", p.thickness);
        number("KISAK_VK_MODERN_SSR_STEPS", steps);
        number("KISAK_VK_MODERN_SSR_LEVELS", levels);
        p.steps = static_cast<uint32_t>(std::min(steps, 256.0f));
        p.levels = static_cast<uint32_t>(std::min(levels, static_cast<float>(modern::kMaxReflectionLevels)));
        number("KISAK_VK_MODERN_PLANAR_REACH", backend->planarReach);
        const std::string ripple = EnvText("KISAK_VK_MODERN_PLANAR_RIPPLE");
        if (!ripple.empty())
            backend->planarRipple = std::max(0.0f, std::strtof(ripple.c_str(), nullptr));
    }
    // The normals buffer is built for what reads it; KISAK_VK_MODERN_NORMALS=0
    // keeps it (and so ambient occlusion and reflections) off whatever asks.
    start.normals = onOff("KISAK_VK_MODERN_NORMALS");
    // Soft sun shadows with our lit shaders, unless this says otherwise; the
    // sun's apparent size in degrees (bigger is softer).
    start.softShadows = onOff("KISAK_VK_MODERN_SOFT_SHADOWS");
    {
        const std::string softness = EnvText("KISAK_VK_MODERN_SHADOW_SOFTNESS");
        const float value = softness.empty() ? 0.0f : std::strtof(softness.c_str(), nullptr);
        if (value > 0.0f && value <= 20.0f)
            start.shadowSoftness = value;
    }
    // Soft spot-light shadows with our lit shaders, unless this says
    // otherwise; the lights' size in world units.
    start.softSpotShadows = onOff("KISAK_VK_MODERN_SOFT_SPOT_SHADOWS");
    {
        const std::string size = EnvText("KISAK_VK_MODERN_SPOT_LIGHT_SIZE");
        const float value = size.empty() ? 0.0f : std::strtof(size.c_str(), nullptr);
        if (value > 0.0f && value <= 64.0f)
            start.spotLightSize = value;
    }
    // Contact shadows with our lit shaders, unless this says otherwise; how
    // far towards the sun they look, in world units.
    start.contactShadows = onOff("KISAK_VK_MODERN_CONTACT_SHADOWS");
    {
        const std::string length = EnvText("KISAK_VK_MODERN_CONTACT_LENGTH");
        const float value = length.empty() ? 0.0f : std::strtof(length.c_str(), nullptr);
        if (value > 0.0f && value <= 256.0f)
            start.contactLength = value;
    }
    ApplyModernTuning(backend, backend->modernTuning);
    if (!h.windowSystem || !h.windowSystem(h.user, desc.window, backend->windowSystem))
    {
        error = "the host has no Vulkan window system for this window";
        delete backend;
        return nullptr;
    }

    CoreDesc coreDesc;
    coreDesc.validation = h.validation;
    coreDesc.deviceIndex = h.deviceIndex;
    coreDesc.vsync = desc.vsync;
    coreDesc.swapInterval = desc.swapInterval;
    coreDesc.pipelineCachePath = h.pipelineCachePath;
    // Vulkan calls on a thread of the core's own, off the render thread (as
    // DXVK does); KISAK_VK_RECORD_THREAD=0 turns it off.
    coreDesc.recordThread = true;
    coreDesc.log = h.log;
    coreDesc.logUser = h.user;
    char coreError[512] = {};
    backend->core = CreateCore(coreDesc, backend->windowSystem, coreError, sizeof(coreError));
    if (!backend->core)
    {
        error = coreError;
        delete backend;
        return nullptr;
    }

    lg::DeviceDesc legacyDesc;
    legacyDesc.width = desc.width;
    legacyDesc.height = desc.height;
    legacyDesc.log = h.log;
    legacyDesc.logUser = h.user;
    legacyDesc.shaderCachePath = h.shaderCachePath;
    // The pipeline list lives beside the pipeline cache: pipelines.bin gives
    // pipelines.list.
    std::string pipelineList;
    if (h.pipelineCachePath && *h.pipelineCachePath)
    {
        pipelineList = h.pipelineCachePath;
        const size_t dot = pipelineList.rfind('.');
        const size_t slash = pipelineList.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            pipelineList.resize(dot);
        pipelineList += ".list";
        legacyDesc.pipelineListPath = pipelineList.c_str();
    }
    legacyDesc.backBufferSamples = desc.samples;
    backend->legacy = lg::CreateDevice(backend->core, legacyDesc, error);
    if (!backend->legacy)
    {
        CloseBackend(backend);
        return nullptr;
    }
    backend->ownFeed = lg::GetGpuFeed(backend->legacy);
    lg::SetModernLitStrengths(backend->legacy, backend->sunStrength, backend->skyStrength);
    lg::SetModernLightPass(backend->legacy, backend->lightPassStrength);
    lg::SetModernEmissive(backend->legacy, backend->emissiveStrength);
    return backend;
}

void BackendFrame(Backend *backend)
{
    if (!backend || !backend->legacy)
        return;
    if (backend->core)
        TrackFrame(*backend);
    if (!backend->host->tuning)
        return;
    Tuning tuning;
    backend->host->tuning(backend->host->user, tuning);
    if (std::memcmp(&tuning.modern, &backend->modernTuning, sizeof(ModernTuning)) != 0)
    {
        backend->modernTuning = tuning.modern;
        ApplyModernTuning(backend, tuning.modern, true);
    }
    Tuning &old = backend->tuning;
    if (tuning.keepDrawState != old.keepDrawState)
    {
        old.keepDrawState = tuning.keepDrawState;
        lg::SetKeepDrawState(backend->legacy, tuning.keepDrawState);
    }
    if (tuning.gpuFeed == old.gpuFeed && tuning.gpuFeedDraws == old.gpuFeedDraws
        && tuning.gpuFeedIdleDraws == old.gpuFeedIdleDraws)
        return;
    old = tuning;
    lg::GpuFeed feed = backend->ownFeed;
    if (tuning.gpuFeed >= 0)
    {
        feed.on = tuning.gpuFeed != 0;
        feed.always = tuning.gpuFeed == 2;
    }
    if (tuning.gpuFeedDraws)
        feed.draws = tuning.gpuFeedDraws;
    feed.idleDraws = tuning.gpuFeedIdleDraws;
    lg::SetGpuFeed(backend->legacy, feed);
}

void BackendLogStats(Backend *backend, bool reset)
{
    if (!backend || !backend->legacy || !backend->core)
        return;
    const Host &h = *backend->host;
    // What each frame cost, for comparing frame rates: the work recorded per
    // presented frame, and where the GPU's time went. Counted from the last
    // reset, or from the device's start.
    const lg::DeviceStats &stats = lg::Stats(backend->legacy);
    const CoreStats &core = Stats(backend->core);
    const lg::DeviceStats &was = backend->statsBase;
    const CoreStats &coreWas = backend->coreBase;
    const uint64_t presented = core.framesPresented - coreWas.framesPresented;
    const double frames = presented ? double(presented) : 1.0;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - backend->statsSince).count();
    const auto average = [](const PassTimings &timings) {
        return timings.samples ? timings.totalMs / double(timings.samples) : 0.0;
    };
    HostLog(h,
            "vulkan d3d9: frame stats: %llu frames in %.2f s (%.1f fps); per frame %.0f draws, %.1f passes, "
            "%.0f descriptor "
            "sets (%.0f reused), %.1f KB constants, %.1f dynamic renames, %.2f submissions (%llu flushes in all, "
            "%llu of them ahead of Present at a swap fence, %llu to feed the GPU during a frame); "
            "GPU frame %.2f ms average, %.2f min, %.2f max over %llu frames",
            static_cast<unsigned long long>(presented), seconds, seconds > 0.0 ? double(presented) / seconds : 0.0,
            double(stats.draws - was.draws) / frames,
            double(stats.passes - was.passes) / frames, double(stats.descriptorSets - was.descriptorSets) / frames,
            double(stats.descriptorSetsReused - was.descriptorSetsReused) / frames,
            double(stats.constantBytes - was.constantBytes) / 1024.0 / frames,
            double(stats.dynamicRenames - was.dynamicRenames) / frames,
            double(core.submissions - coreWas.submissions) / frames,
            static_cast<unsigned long long>(core.flushes - coreWas.flushes),
            static_cast<unsigned long long>(stats.fenceFlushes - was.fenceFlushes),
            static_cast<unsigned long long>(stats.feedFlushes - was.feedFlushes), average(core.frame),
            core.frame.samples ? core.frame.minMs : 0.0, core.frame.samples ? core.frame.maxMs : 0.0,
            static_cast<unsigned long long>(core.frame.samples));
    // Whether the GPU is the limit, and what it waits for when it is not: a
    // GPU frame runs from a frame's first command to its last. The time in
    // its passes is the GPU at work on it; the rest of it is the GPU waiting
    // for the frame's next part. Before a frame is the time since the frame
    // before ended, with nothing of either to do.
    if (core.framePasses.samples)
        HostLog(h,
                "vulkan d3d9: GPU time: per frame %.2f ms in passes, %.2f ms between them, and %.2f ms without work "
                "before the frame (%.2f max); %llu parts sent early to a GPU with nothing left",
                average(core.framePasses), std::max(0.0, average(core.frame) - average(core.framePasses)),
                average(core.frameGap), core.frameGap.samples ? core.frameGap.maxMs : 0.0,
                static_cast<unsigned long long>(stats.feedIdleFlushes - was.feedIdleFlushes));
    else if (core.frame.samples)
        HostLog(h,
                "vulkan d3d9: GPU time: per frame %.2f ms from first command to last (passes not timed), and "
                "%.2f ms without work before the frame (%.2f max); %llu parts sent early to a GPU with nothing left",
                average(core.frame), average(core.frameGap), core.frameGap.samples ? core.frameGap.maxMs : 0.0,
                static_cast<unsigned long long>(stats.feedIdleFlushes - was.feedIdleFlushes));
    LogSlowFrames(*backend);
    BackendLogMemory(backend, nullptr);
    if (core.recordThread)
        HostLog(h,
                "vulkan d3d9: record thread: per frame %.0f commands in %.1f blocks; render thread waited "
                "%.3f ms per frame for it",
                double(core.recordPackets - coreWas.recordPackets) / frames,
                double(core.recordChunks - coreWas.recordChunks) / frames,
                (core.recordWaitMs - coreWas.recordWaitMs) / frames);
    // Whether the engine's occlusion queries were answered with counts (the
    // sun's flare: about one Vulkan query a frame while the sun is drawn),
    // how many pools they took, and what they answered: with the sun's place
    // on screen in the open the results are its sprite's samples, and behind
    // a wall every result is 0.
    if (Features(backend->core).occlusionCounts)
    {
        const uint64_t results = stats.occlusionResults - was.occlusionResults;
        char answers[160];
        if (results)
            std::snprintf(answers, sizeof(answers),
                          "%llu results, %llu of them with no sample, %.1f samples on average, the last %u",
                          static_cast<unsigned long long>(results),
                          static_cast<unsigned long long>(stats.occlusionResultsZero - was.occlusionResultsZero),
                          double(stats.occlusionSamples - was.occlusionSamples) / double(results),
                          stats.occlusionLast);
        else
            std::snprintf(answers, sizeof(answers), "no results");
        HostLog(h, "vulkan d3d9: occlusion queries: counted, with %llu Vulkan queries from %u pools; %s",
                static_cast<unsigned long long>(core.occlusionCounts - coreWas.occlusionCounts),
                core.occlusionPools, answers);
    }
    else
        HostLog(h, "vulkan d3d9: occlusion queries: not counted, every finished query reports all samples passed");
    // The fullscreen gamma ramp (r_gamma in fullscreen), when it was applied:
    // the frames shown through it and the GPU time of its pass.
    if (const uint64_t gammaFrames = stats.gammaPresents - was.gammaPresents)
    {
        const PassTimings pass = PassTimingsFor(backend->core, "gamma");
        HostLog(h, "vulkan d3d9: gamma: %llu frames shown through the gamma ramp, %.3f ms GPU each",
                static_cast<unsigned long long>(gammaFrames), pass.supported ? average(pass) : 0.0);
    }
    // The Modern path, when r_visuals modern has run it: the scenes it
    // anti-aliased, the ones of them that started the history again, and
    // the GPU time of its pass.
    if (backend->taa)
    {
        const modern::TaaStats taa = modern::Stats(backend->taa);
        const PassTimings pass = PassTimingsFor(backend->core, "modern taa");
        HostLog(h, "vulkan d3d9: modern: %llu scenes anti-aliased (%llu without history), %.3f ms GPU each",
                static_cast<unsigned long long>(taa.resolves - backend->taaBase.resolves),
                static_cast<unsigned long long>(taa.restarts - backend->taaBase.restarts),
                pass.supported ? average(pass) : 0.0);
    }
    // Linear light: the scenes drawn in it, and the GPU time of the tonemap
    // pass that writes each back as display values.
    if (backend->tonemap)
    {
        const PassTimings pass = PassTimingsFor(backend->core, "modern tonemap");
        HostLog(h, "vulkan d3d9: modern: %llu scenes in linear light (%llu copies read from them), "
                   "%.3f ms GPU each for the tonemap",
                static_cast<unsigned long long>(backend->linearScenes - backend->linearScenesBase),
                static_cast<unsigned long long>(backend->linearCopies - backend->linearCopiesBase),
                pass.supported ? average(pass) : 0.0);
    }
    // Auto exposure: the scenes measured, the exposures read back now and then,
    // and the GPU time of its passes.
    if (backend->exposure)
    {
        const modern::ExposureStats exposure = modern::Stats(backend->core, backend->exposure, true);
        const PassTimings pass = PassTimingsFor(backend->core, "modern exposure");
        const double samples = exposure.samples ? double(exposure.samples) : 1.0;
        HostLog(h, "vulkan d3d9: modern: %llu scenes with auto exposure (%llu taken at once), exposure %.2f EV "
                   "on average (%.2f to %.2f), scene log2 luminance %.2f on average (%.2f EV wanted), "
                   "%.3f ms GPU each",
                static_cast<unsigned long long>(exposure.scenes), static_cast<unsigned long long>(exposure.resets),
                exposure.evSum / samples, exposure.samples ? exposure.evMin : 0.0,
                exposure.samples ? exposure.evMax : 0.0, exposure.log2LuminanceSum / samples,
                exposure.wantedEvSum / samples,
                pass.supported && exposure.scenes ? (pass.totalMs - backend->exposureMsBase) / double(exposure.scenes)
                                                  : 0.0);
    }
    // Lit surfaces in linear light: the scenes with them, and how often the
    // lit inputs changed in each.
    if (backend->litScenes != backend->litScenesBase)
    {
        const uint64_t scenes = backend->litScenes - backend->litScenesBase;
        HostLog(h, "vulkan d3d9: modern: %llu scenes with lit surfaces in linear light, %s "
                   "(%.1f changes of lit inputs each)",
                static_cast<unsigned long long>(scenes),
                backend->litOverWhite ? "kept over white" : "computed from decoded inputs",
                double(backend->litChanges - backend->litChangesBase) / double(scenes));
        // Of those, the passes drawn as our own lit shaders, and the pixel
        // modules built as them (and that could not be).
        if (backend->shadersWanted)
            HostLog(h, "vulkan d3d9: modern: lit shaders of our own: %.1f changes to one each scene, %u pixel modules "
                       "built as one (%u could not be), sun strength %.2f, sky strength %.2f, light pass %s at %.2f",
                    double(backend->litShaderChanges - backend->litShaderChangesBase) / double(scenes),
                    stats.modernLitModules - was.modernLitModules, stats.modernLitRefused - was.modernLitRefused,
                    backend->sunStrength, backend->skyStrength, backend->lightPassWanted ? "on" : "off",
                    backend->lightPassStrength);
    }
    // Lamps and lit signs made brighter than white: how often a scene in
    // linear light switched to drawing one.
    if (backend->litWanted && backend->linearScenes != backend->linearScenesBase)
        HostLog(h, "vulkan d3d9: modern: lamps and lit signs: %s at %.2f, %.2f changes to one each scene",
                backend->emissiveWanted ? "on" : "off", backend->emissiveStrength,
                double(backend->emissiveChanges - backend->emissiveChangesBase)
                    / double(backend->linearScenes - backend->linearScenesBase));
    // Velocity: the surfaces drawn again with their own motion, and the
    // GPU time of that pass.
    if (backend->velocity || backend->velocityRemade)
    {
        const modern::VelocityStats velocity = modern::Stats(backend->velocity);
        const PassTimings pass = PassTimingsFor(backend->core, "modern velocity");
        HostLog(h, "vulkan d3d9: modern: %llu scenes with surfaces that move on their own (%llu surfaces, %llu "
                   "of them brush models), %.3f ms GPU each for their velocity",
                static_cast<unsigned long long>(velocity.passes - backend->velocityBase.passes),
                static_cast<unsigned long long>(velocity.surfaces - backend->velocityBase.surfaces),
                static_cast<unsigned long long>(velocity.world - backend->velocityBase.world),
                pass.supported ? average(pass) : 0.0);
        const modern::KeptStats kept = modern::Stats(backend->kept);
        const uint64_t keptScenes = kept.scenes - backend->keptBase.scenes;
        HostLog(h, "vulkan d3d9: modern: %llu scenes kept skinned vertices (%llu surfaces, %.1f KB each scene), "
                   "%llu skinned surfaces drawn from last scene's",
                static_cast<unsigned long long>(keptScenes),
                static_cast<unsigned long long>(kept.surfaces - backend->keptBase.surfaces),
                keptScenes ? double(kept.bytes - backend->keptBase.bytes) / 1024.0 / double(keptScenes) : 0.0,
                static_cast<unsigned long long>(velocity.skinned - backend->velocityBase.skinned));
    }
    // The normals buffer: the scenes it was built for and its pass's GPU time.
    if (backend->normals)
    {
        const PassTimings pass = PassTimingsFor(backend->core, "modern normals");
        HostLog(h, "vulkan d3d9: modern: %llu scenes with a normals buffer from the depth prepass, "
                   "%.3f ms GPU each",
                static_cast<unsigned long long>(modern::Stats(backend->normals).builds - backend->normalsBase.builds),
                pass.supported ? average(pass) : 0.0);
    }
    // Ambient occlusion: the scenes it was built for and its two passes' GPU time.
    if (backend->ao)
    {
        // Two passes: the trace (gtao.frag, "modern ao trace") and the blur
        // (ao_blur.frag, "modern ao").
        const PassTimings trace = PassTimingsFor(backend->core, "modern ao trace");
        const PassTimings pass = PassTimingsFor(backend->core, "modern ao");
        const double traceMs = trace.supported ? average(trace) : 0.0;
        const double blurMs = pass.supported ? average(pass) : 0.0;
        const uint64_t scenes = modern::Stats(backend->ao).builds - backend->aoBase.builds;
        HostLog(h, "vulkan d3d9: modern: %llu scenes with ambient occlusion (radius %.1f, thickness %.1f, "
                   "power %.2f, floor %.2f, edge %.2f%s), %.3f ms GPU each",
                static_cast<unsigned long long>(scenes), backend->aoParams.radius, backend->aoParams.thickness,
                backend->aoParams.power, backend->aoParams.floor, backend->aoParams.edge,
                backend->aoParams.half ? ", half resolution" : "", traceMs + blurMs);
        if (scenes)
            HostLog(h, "vulkan d3d9: modern: ambient occlusion passes: trace %.3f ms, blur %.3f ms GPU each", traceMs,
                    blurMs);
        // Contact shadows: the scenes traced and their pass's GPU time.
        const uint64_t contacts = modern::Stats(backend->ao).contacts - backend->aoBase.contacts;
        if (contacts)
        {
            const PassTimings contactPass = PassTimingsFor(backend->core, "modern contact shadows");
            HostLog(h, "vulkan d3d9: modern: %llu scenes with contact shadows (length %.1f, thickness %.1f, "
                       "strength %.2f), %.3f ms GPU each",
                    static_cast<unsigned long long>(contacts), backend->contactParams.length,
                    backend->contactParams.thickness, backend->contactParams.strength,
                    contactPass.supported ? average(contactPass) : 0.0);
        }
    }
    // Screen-space reflections: the scenes traced, the levels of the last,
    // the trace pass's GPU time (the levels are blits, not timed).
    if (backend->reflections)
    {
        const modern::ReflectionStats reflections = modern::Stats(backend->reflections);
        const PassTimings pass = PassTimingsFor(backend->core, "modern reflections");
        HostLog(h, "vulkan d3d9: modern: %llu scenes with screen-space reflections (%u levels), %.3f ms GPU each",
                static_cast<unsigned long long>(reflections.traces - backend->reflectionsBase.traces),
                reflections.levels, pass.supported ? average(pass) : 0.0);
    }
    // Volumetric fog: the scenes it was drawn over (those from a froxel
    // volume, those that marched the sun's shadow map, those with spot
    // lights in the air and how many) and its GPU time per scene: two
    // full-screen passes for a march, one and two compute passes for
    // froxels.
    if (backend->fog)
    {
        const modern::FogStats fog = modern::Stats(backend->fog);
        const PassTimings pass = PassTimingsFor(backend->core, "modern fog");
        const PassTimings froxelPass = PassTimingsFor(backend->core, "modern fog froxels");
        const uint64_t scenes = fog.scenes - backend->fogBase.scenes;
        const uint64_t froxelScenes = fog.froxels - backend->fogBase.froxels;
        const double each = scenes ? (average(pass) * double(2 * (scenes - froxelScenes) + froxelScenes)
                                      + average(froxelPass) * double(2 * froxelScenes))
                                         / double(scenes)
                                   : 0.0;
        HostLog(h, "vulkan d3d9: modern: %llu scenes with volumetric fog (%llu from froxels, %llu through the sun's "
                   "shadow map, %llu with %llu spot lights; sun %.2f, scatter %.2f, spot lights %.2f, %u steps over "
                   "%.0f units%s), %.3f ms GPU each",
                static_cast<unsigned long long>(scenes), static_cast<unsigned long long>(froxelScenes),
                static_cast<unsigned long long>(fog.shadowed - backend->fogBase.shadowed),
                static_cast<unsigned long long>(fog.spotScenes - backend->fogBase.spotScenes),
                static_cast<unsigned long long>(fog.spots - backend->fogBase.spots), backend->fogParams.sunShare,
                backend->fogParams.scatter, backend->fogParams.spotShare, backend->fogParams.steps,
                backend->fogParams.march, backend->fogParams.half ? ", half resolution" : "",
                pass.supported ? each : 0.0);
    }
    // God rays: the scenes they were drawn over (and those whose sun was out
    // of the view) and their three passes' GPU time per scene drawn.
    if (backend->rays)
    {
        const modern::RaysStats rays = modern::Stats(backend->rays);
        const PassTimings pass = PassTimingsFor(backend->core, "modern god rays");
        HostLog(h, "vulkan d3d9: modern: %llu scenes with god rays (%llu with the sun out of view; strength %.2f, "
                   "spread %.0f, threshold %.2f, sky %.2f, %u samples), %.3f ms GPU each",
                static_cast<unsigned long long>(rays.scenes - backend->raysBase.scenes),
                static_cast<unsigned long long>(rays.behind - backend->raysBase.behind), backend->raysParams.strength,
                backend->raysParams.spread, backend->raysParams.threshold, backend->raysParams.sky,
                backend->raysParams.samples, pass.supported ? 3.0 * average(pass) : 0.0);
    }
    // The last scene's fog and sun, as the map sets them.
    if (backend->lastSceneSeen)
    {
        if (backend->lastFogOn && backend->lastFogDensity > 0.0f)
            HostLog(h, "vulkan d3d9: modern: the last scene's fog: start %.0f, density %.7f (half the light gone "
                       "%.0f units out), colour %.3f %.3f %.3f",
                    backend->lastFogStart, backend->lastFogDensity,
                    backend->lastFogStart + 1.0f / backend->lastFogDensity, backend->lastFogColor[0],
                    backend->lastFogColor[1], backend->lastFogColor[2]);
        else
            HostLog(h, "vulkan d3d9: modern: the last scene's fog: none");
        if (backend->lastSunOn)
            HostLog(h, "vulkan d3d9: modern: the last scene's sun: colour %.3f %.3f %.3f, towards %.3f %.3f %.3f",
                    backend->lastSunColor[0], backend->lastSunColor[1], backend->lastSunColor[2],
                    backend->lastSunDirection[0], backend->lastSunDirection[1], backend->lastSunDirection[2]);
        else
            HostLog(h, "vulkan d3d9: modern: the last scene's sun: none");
    }
    // Reflection probes: filtered as the world loaded, since the device was made.
    if (backend->probesFiltered || backend->probesLeft)
        HostLog(h, "vulkan d3d9: modern: %u reflection probes filtered by gloss in linear light (%u left as they "
                   "are), %.1f ms each",
                backend->probesFiltered, backend->probesLeft,
                backend->probesFiltered ? backend->probesMs / double(backend->probesFiltered) : 0.0);
    if (backend->probeCaptureScenes)
        HostLog(h, "vulkan d3d9: modern: %u scenes read back in linear light to capture reflection probes (%u "
                   "faces taken)",
                backend->probeCaptureScenes, backend->probeCaptureReads);
    // Planar reflections: the views drawn into the planar image, and its size.
    if (backend->planarViews > backend->planarViewsBase)
        HostLog(h, "vulkan d3d9: modern: %llu planar reflection views (%ux%u)",
                static_cast<unsigned long long>(backend->planarViews - backend->planarViewsBase),
                backend->planarView.width, backend->planarView.height);
    if (backend->planarReads > backend->planarReadsBase)
        HostLog(h, "vulkan d3d9: modern: %llu scenes read the planar reflection",
                static_cast<unsigned long long>(backend->planarReads - backend->planarReadsBase));
    // Soft sun shadows: the scenes whose lit surfaces filtered the sun's
    // shadow map with them (their cost is in the lit passes' own time).
    if (backend->softShadowScenes > backend->softShadowScenesBase)
        HostLog(h, "vulkan d3d9: modern: %llu scenes with soft sun shadows (sun %.2f degrees)",
                static_cast<unsigned long long>(backend->softShadowScenes - backend->softShadowScenesBase),
                backend->shadowSoftness);
    if (backend->softSpotScenes > backend->softSpotScenesBase)
        HostLog(h, "vulkan d3d9: modern: %llu scenes drew soft spot-light shadows (lights %.1f units)",
                static_cast<unsigned long long>(backend->softSpotScenes - backend->softSpotScenesBase),
                backend->spotLightSize);
    // Bloom: the scenes it ran for in place of the glow, the passes of each,
    // and their GPU time per scene.
    if (backend->bloom)
    {
        const PassTimings pass = PassTimingsFor(backend->core, "modern bloom");
        const uint64_t scenes = backend->bloomScenes - backend->bloomScenesBase;
        const uint64_t passes = modern::Stats(backend->bloom).passes - backend->bloomPassesBase;
        HostLog(h, "vulkan d3d9: modern: %llu scenes with bloom in place of the glow (%.1f passes each), "
                   "%.3f ms GPU each",
                static_cast<unsigned long long>(scenes), scenes ? double(passes) / double(scenes) : 0.0,
                pass.supported && scenes ? (pass.totalMs - backend->bloomMsBase) / double(scenes) : 0.0);
    }
    // The compute check: the volumes built and drawn, and its passes' GPU time.
    if (backend->computeCheck)
    {
        const modern::ComputeCheckStats check = modern::Stats(backend->computeCheck);
        const PassTimings fill = PassTimingsFor(backend->core, "modern compute check fill");
        const PassTimings integrate = PassTimingsFor(backend->core, "modern compute check integrate");
        HostLog(h, "vulkan d3d9: modern: %llu compute check volumes built (%llu drawn), %.3f ms GPU each for "
                   "its compute passes",
                static_cast<unsigned long long>(check.builds - backend->computeCheckBase.builds),
                static_cast<unsigned long long>(check.draws - backend->computeCheckBase.draws),
                fill.supported && integrate.supported ? average(fill) + average(integrate) : 0.0);
    }
    if (reset)
    {
        if (backend->taa)
            backend->taaBase = modern::Stats(backend->taa);
        backend->computeCheckBase = modern::Stats(backend->computeCheck);
        backend->bloomScenesBase = backend->bloomScenes;
        backend->bloomPassesBase = modern::Stats(backend->bloom).passes;
        if (backend->bloom)
            backend->bloomMsBase = PassTimingsFor(backend->core, "modern bloom").totalMs;
        backend->velocityBase = modern::Stats(backend->velocity);
        backend->normalsBase = modern::Stats(backend->normals);
        backend->aoBase = modern::Stats(backend->ao);
        backend->fogBase = modern::Stats(backend->fog);
        backend->raysBase = modern::Stats(backend->rays);
        backend->reflectionsBase = modern::Stats(backend->reflections);
        backend->keptBase = modern::Stats(backend->kept);
        backend->linearScenesBase = backend->linearScenes;
        backend->planarViewsBase = backend->planarViews;
        backend->planarReadsBase = backend->planarReads;
        backend->softShadowScenesBase = backend->softShadowScenes;
        backend->softSpotScenesBase = backend->softSpotScenes;
        modern::ResetStats(backend->exposure);
        if (backend->exposure)
            backend->exposureMsBase = PassTimingsFor(backend->core, "modern exposure").totalMs;
        backend->litScenesBase = backend->litScenes;
        backend->litChangesBase = backend->litChanges;
        backend->litShaderChangesBase = backend->litShaderChanges;
        backend->emissiveChangesBase = backend->emissiveChanges;
        backend->linearCopiesBase = backend->linearCopies;
        backend->statsBase = stats;
        backend->coreBase = core;
        backend->statsSince = now;
        ResetFrameTimings(backend->core);
        for (SlowFrame &frame : backend->slowFrames)
            frame = SlowFrame();
        backend->framesTracked = 0;
    }
}

void CloseBackend(Backend *backend)
{
    if (!backend)
        return;
    const Host &h = *backend->host;
    BackendLogStats(backend, false);
    if (backend->taa)
    {
        modern::DestroyTaa(backend->core, backend->taa);
        backend->taa = nullptr;
    }
    if (backend->velocity)
    {
        modern::DestroyVelocity(backend->core, backend->velocity);
        backend->velocity = nullptr;
    }
    if (backend->kept)
    {
        modern::DestroyKeptVertices(backend->core, backend->kept);
        backend->kept = nullptr;
    }
    if (backend->ao)
    {
        modern::DestroyAmbientOcclusion(backend->core, backend->ao);
        backend->ao = nullptr;
    }
    if (backend->fog)
    {
        modern::DestroyFog(backend->core, backend->fog);
        backend->fog = nullptr;
    }
    if (backend->rays)
    {
        modern::DestroyRays(backend->core, backend->rays);
        backend->rays = nullptr;
    }
    if (backend->legacy && backend->aoTexture)
    {
        lg::DestroyTexture(backend->legacy, backend->aoTexture);
        backend->aoTexture = nullptr;
        backend->aoSurface = nullptr;
    }
    if (backend->normals)
    {
        modern::DestroyNormals(backend->core, backend->normals);
        backend->normals = nullptr;
        backend->sceneNormals = nullptr;
    }
    if (backend->legacy)
    {
        lg::SetModernReflections(backend->legacy, nullptr);
        lg::SetModernPlanar(backend->legacy, nullptr, nullptr);
    }
    if (backend->reflections)
    {
        modern::DestroyReflections(backend->core, backend->reflections);
        backend->reflections = nullptr;
        backend->sceneReflections = nullptr;
    }
    if (backend->debug)
    {
        modern::DestroyDebugView(backend->core, backend->debug);
        backend->debug = nullptr;
    }
    if (backend->computeCheck)
    {
        modern::DestroyComputeCheck(backend->core, backend->computeCheck);
        backend->computeCheck = nullptr;
    }
    if (backend->bloom)
    {
        modern::DestroyBloom(backend->core, backend->bloom);
        backend->bloom = nullptr;
    }
    if (backend->exposure)
    {
        modern::DestroyExposure(backend->core, backend->exposure);
        backend->exposure = nullptr;
    }
    if (backend->tonemap)
    {
        modern::DestroyTonemap(backend->core, backend->tonemap);
        backend->tonemap = nullptr;
    }
    if (backend->probeCaptureReadback)
    {
        DestroyReadback(backend->core, backend->probeCaptureReadback);
        backend->probeCaptureReadback = nullptr;
    }
    backend->probeCapturing = false;
    if (backend->legacy && backend->planarColor)
        lg::DestroyTexture(backend->legacy, backend->planarColor);
    if (backend->legacy && backend->planarDepth)
        lg::DestroySurface(backend->legacy, backend->planarDepth);
    backend->planarColor = nullptr;
    backend->planarColorSurface = nullptr;
    backend->planarDepth = nullptr;
    if (backend->legacy && backend->linearScene)
    {
        lg::DestroyTexture(backend->legacy, backend->linearScene);
        backend->linearScene = nullptr;
        backend->linearSceneSurface = nullptr;
    }
    if (backend->legacy)
    {
        lg::DestroyDevice(backend->legacy);
        backend->legacy = nullptr;
    }
    if (backend->core)
    {
        CoreStats final{};
        DestroyCore(backend->core, &final);
        backend->core = nullptr;
        if (h.coreDestroyed)
            h.coreDestroyed(h.user, final);
    }
    delete backend;
}

lg::Device *LegacyDevice(Backend *backend)
{
    return backend ? backend->legacy : nullptr;
}

const char *BackendDeviceName(const Backend *backend)
{
    return backend && backend->core ? gfx_vk::Device(backend->core).name : "?";
}

void DetachBackendWindow(Backend *backend)
{
    if (!backend)
        return;
    if (backend->legacy)
        lg::DetachWindow(backend->legacy);
    else if (backend->core)
        gfx_vk::DetachWindow(backend->core);
}

void BackendLogMemory(Backend *backend, const char *when)
{
    if (!backend || !backend->core)
        return;
    // What the core holds from the driver, and where: a heap that only grows
    // from one map load to the next is memory not given back; the small
    // device-local heap the CPU can map (256 MB without resizable BAR) is
    // the one that ran out on the PC (pc-water-run-4).
    const CoreStats &core = Stats(backend->core);
    VkPhysicalDeviceMemoryProperties memory{};
    Functions(backend->core).vkGetPhysicalDeviceMemoryProperties(PhysicalDeviceHandle(backend->core), &memory);
    std::string heaps;
    for (uint32_t i = 0; i < memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
    {
        char heap[96];
        std::snprintf(heap, sizeof(heap), "%s%u (%s%llu MB) %.1f MB", i ? ", " : "", i,
                      (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device-local, " : "",
                      static_cast<unsigned long long>(memory.memoryHeaps[i].size >> 20),
                      double(core.heapBytes[i]) / (1024.0 * 1024.0));
        heaps += heap;
    }
    HostLog(*backend->host,
            "vulkan d3d9: memory%s%s: %.1f MB device memory and %.1f MB mapped in %u blocks, %u buffers and %u "
            "images alive; by heap %s; %u blocks went to a later memory type, %u empty blocks and %u arena chunks "
            "given back",
            when ? " at " : "", when ? when : "", double(core.deviceMemoryBytes) / (1024.0 * 1024.0),
            double(core.hostMemoryBytes) / (1024.0 * 1024.0), core.memoryBlocks, core.buffersAlive,
            core.imagesAlive, heaps.c_str(), core.memoryFallbacks, core.emptyBlocksFreed, core.arenaChunksFreed);
}

uint64_t BackendTextureMemory(const Backend *backend)
{
    if (!backend || !backend->core)
        return 0;
    // Device-local memory, as DXVK reports it.
    VkPhysicalDeviceMemoryProperties memory{};
    Functions(backend->core).vkGetPhysicalDeviceMemoryProperties(PhysicalDeviceHandle(backend->core), &memory);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            bytes += memory.memoryHeaps[i].size;
    return bytes;
}

bool BackendWindowSize(const Backend *backend, uint32_t &width, uint32_t &height)
{
    if (!backend || !backend->windowSystem.getPixelSize)
        return false;
    backend->windowSystem.getPixelSize(backend->windowSystem.user, &width, &height);
    return true;
}

void BackendSetSwapInterval(Backend *backend, uint32_t interval)
{
    if (backend && backend->core)
        SetSwapInterval(backend->core, interval);
}

void BackendCounters(const Backend *backend, DeviceCounters &out)
{
    out.validationErrors = backend && backend->core ? Stats(backend->core).validationErrors : 0;
    out.validation = backend && backend->core && ValidationEnabled(backend->core);
    out.submissions = backend && backend->core ? Stats(backend->core).submissions : 0;
    out.fenceFlushes = backend && backend->legacy ? lg::Stats(backend->legacy).fenceFlushes : 0;
    out.feedFlushes = backend && backend->legacy ? lg::Stats(backend->legacy).feedFlushes : 0;
    out.feedIdleFlushes = backend && backend->legacy ? lg::Stats(backend->legacy).feedIdleFlushes : 0;
    out.occlusionCounted = backend && backend->core && Features(backend->core).occlusionCounts;
    out.occlusionCounts = backend && backend->core ? Stats(backend->core).occlusionCounts : 0;
    out.occlusionPools = backend && backend->core ? Stats(backend->core).occlusionPools : 0;
    out.occlusionResults = backend && backend->legacy ? lg::Stats(backend->legacy).occlusionResults : 0;
    out.occlusionResultsZero = backend && backend->legacy ? lg::Stats(backend->legacy).occlusionResultsZero : 0;
    out.occlusionSamples = backend && backend->legacy ? lg::Stats(backend->legacy).occlusionSamples : 0;
    out.textureUploads = backend && backend->legacy ? lg::Stats(backend->legacy).textureUploads : 0;
    out.textureUploadBytes = backend && backend->legacy ? lg::Stats(backend->legacy).uploadBytes : 0;
}

namespace
{
// Logs a reason the Modern path cannot do something, once per reason: a
// refusal would repeat every frame.
void LogModernOnce(Backend *backend, const char *what, const std::string &reason)
{
    const std::string line = std::string(what) + ": " + reason + "\n";
    if (backend->modernRefusals.find(line) != std::string::npos)
        return;
    backend->modernRefusals += line;
    HostLog(*backend->host, "vulkan d3d9: modern: %s: %s", what, reason.c_str());
}

bool EnsureTonemap(Backend *backend)
{
    if (backend->tonemap)
        return true;
    if (backend->linearFailed)
        return false;
    std::string error;
    backend->tonemap = modern::CreateTonemap(backend->core, error);
    if (!backend->tonemap)
    {
        backend->linearFailed = true;
        HostLog(*backend->host, "vulkan d3d9: modern: no linear light: %s", error.c_str());
        return false;
    }
    modern::SetCurve(backend->tonemap, backend->tonemapCurve);
    HostLog(*backend->host, "vulkan d3d9: modern: tonemap curve %s%s", modern::CurveName(backend->tonemapCurve),
            backend->tonemapCurveUnknown ? " (KISAK_VK_MODERN_TONEMAP names no curve: clamp, neutral, agx or aces)"
                                         : "");
    return true;
}

// The display value (0 to 1) as linear light: the sRGB curve, as the Legacy
// adapter decodes the scene's colours.
float DecodeSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// The bloom over the linear-light scene for the view's glow: null when the
// view has no glow, bloom is off or cannot run (logged once). The scale for
// the tonemap comes back in scale.
Image *DrawBloom(Backend *backend, Image *scene, const VkRect2D &area, const ModernSceneView &view, float &scale)
{
    scale = 0.0f;
    if (!view.glow.on || view.glow.intensity <= 0.0f || !backend->bloomWanted || backend->bloomFailed)
        return nullptr;
    if (!backend->bloom)
    {
        std::string error;
        backend->bloom = modern::CreateBloom(backend->core, error);
        if (!backend->bloom)
        {
            backend->bloomFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no bloom, the glow is drawn as before: %s", error.c_str());
            return nullptr;
        }
    }
    modern::BloomParams params;
    // The glow's cutoff is a display value: its light is where bloom starts,
    // with a narrow soft knee (a quarter of it) below, so that light just
    // past the cutoff does not switch on with a hard edge.
    params.threshold = DecodeSrgb(view.glow.cutoff);
    params.knee = params.threshold * 0.25f;
    params.desaturation = view.glow.desaturation;
    params.levels = modern::BloomLevelsFor(view.glow.radius, area.extent.width, area.extent.height);
    std::string error;
    Image *bloom = modern::BuildBloom(backend->core, backend->bloom, scene, area, params, error);
    if (!bloom)
    {
        LogModernOnce(backend, "no bloom, the glow is drawn as before", error);
        return nullptr;
    }
    // The chain adds every level up: the intensity is shared among them.
    scale = view.glow.intensity / static_cast<float>(params.levels);
    ++backend->bloomScenes;
    return bloom;
}

// This scene's auto exposure (a 1x1 image for the tonemap): null when it is
// off or cannot run (logged once).
Image *MeasureExposure(Backend *backend, Image *scene, const VkRect2D &area, bool cut)
{
    if (!backend->exposureWanted || backend->exposureFailed)
        return nullptr;
    if (!backend->exposure)
    {
        std::string error;
        backend->exposure = modern::CreateExposure(backend->core, error);
        if (!backend->exposure)
        {
            backend->exposureFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no auto exposure: %s", error.c_str());
            return nullptr;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    modern::ExposureParams params = backend->exposureParams;
    params.seconds = backend->exposureLast.time_since_epoch().count()
                         ? std::chrono::duration<float>(now - backend->exposureLast).count()
                         : 0.0f;
    params.reset = cut;
    backend->exposureLast = now;
    std::string error;
    Image *result = modern::MeasureExposure(backend->core, backend->exposure, scene, area, params, error);
    if (!result)
        LogModernOnce(backend, "no auto exposure", error);
    return result;
}

// The whole of a surface, or a rectangle of it, as a core rectangle.
VkRect2D AreaOf(lg::Surface *surface, const lg::Rect *rect)
{
    if (rect)
        return {{rect->left, rect->top},
                {static_cast<uint32_t>(std::max(0, rect->right - rect->left)),
                 static_cast<uint32_t>(std::max(0, rect->bottom - rect->top))}};
    uint32_t width = 0, height = 0;
    lg::d3d::Format format = lg::d3d::FMT_UNKNOWN;
    lg::SurfaceSize(surface, width, height, format);
    return {{0, 0}, {width, height}};
}
} // namespace

lg::Surface *BackendModernSceneTarget(Backend *backend, lg::Surface *backBuffer, ModernResult &result)
{
    result = ModernResult::Skipped;
    if (!backend || !backend->legacy || !backend->core || !backBuffer || !backend->linearWanted)
        return nullptr;
    result = ModernResult::Refused;
    if (backend->linearFailed)
        return nullptr;
    if (lg::SurfaceSamples(backBuffer) != 1)
    {
        LogModernOnce(backend, "the scene is not drawn in linear light", "the back buffer is multisampled");
        return nullptr;
    }
    if (!EnsureTonemap(backend))
        return nullptr;
    uint32_t width = 0, height = 0;
    lg::d3d::Format format = lg::d3d::FMT_UNKNOWN;
    lg::SurfaceSize(backBuffer, width, height, format);
    if (backend->linearScene)
    {
        const lg::TextureDesc &have = lg::Describe(backend->linearScene);
        if (have.width != width || have.height != height)
        {
            lg::DestroyTexture(backend->legacy, backend->linearScene);
            backend->linearScene = nullptr;
            backend->linearSceneSurface = nullptr;
        }
    }
    if (!backend->linearScene)
    {
        lg::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.levels = 1;
        desc.usage = lg::d3d::USAGE_RENDERTARGET;
        desc.format = lg::d3d::FMT_A16B16G16R16F;
        desc.pool = lg::d3d::POOL_DEFAULT;
        desc.name = "modern linear scene";
        backend->linearScene = lg::CreateTexture(backend->legacy, desc);
        backend->linearSceneSurface =
            backend->linearScene ? lg::GetSurfaceLevel(backend->legacy, backend->linearScene, 0, 0) : nullptr;
        if (!backend->linearSceneSurface)
        {
            if (backend->linearScene)
                lg::DestroyTexture(backend->legacy, backend->linearScene);
            backend->linearScene = nullptr;
            backend->linearFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no linear light: cannot make the %ux%u scene image",
                    width, height);
            return nullptr;
        }
        lg::SetLinearLight(backend->linearScene, true);
    }
    ++backend->linearScenes;
    backend->litThisScene = false;
    backend->softSpotThisScene = false;
    result = ModernResult::Done;
    return backend->linearSceneSurface;
}

void BackendModernLitInputs(Backend *backend, const ModernLitInputs *inputs)
{
    if (!backend || !backend->legacy || !backend->litWanted)
        return;
    uint16_t shader = inputs && backend->shadersWanted ? inputs->shader : uint16_t(lg::ModernLitNone);
    if ((lg::ModernLitModel(shader) && !backend->modelShadersWanted)
        || ((shader & lg::ModernLitFamilyMask) == lg::ModernLitNoSun && !backend->noSunShadersWanted)
        || (lg::ModernLitPointLight(shader) && !backend->lightShadersWanted)
        || (shader == lg::ModernLitWater && !backend->waterShaderWanted)
        || (lg::ModernLitLightPass(shader) && !backend->lightPassWanted))
        shader = lg::ModernLitNone;
    const bool emissive = inputs && !inputs->overWhite && inputs->emissive;
    if (emissive && backend->emissiveWanted && backend->linearScene && !backend->linearFailed)
        ++backend->emissiveChanges;
    else if (inputs && !emissive && backend->linearScene && !backend->linearFailed)
    {
        ++backend->litChanges;
        if (inputs->overWhite && shader)
            ++backend->litShaderChanges;
        backend->litOverWhite = inputs->overWhite;
        if (!backend->litThisScene)
            ++backend->litScenes;
        backend->litThisScene = true;
        if (inputs->overWhite && backend->softSpotShadowsWanted && lg::ModernLitPointLight(shader)
            && lg::ModernLitShadowed(shader) && !backend->softSpotThisScene)
        {
            ++backend->softSpotScenes;
            backend->softSpotThisScene = true;
        }
    }
    lg::SetLinearInputs(backend->legacy, inputs ? inputs->constants : 0, inputs ? inputs->samplers : 0,
                        inputs ? inputs->doubled : 0, inputs && inputs->colorInput, inputs && inputs->overWhite,
                        shader, emissive && backend->emissiveWanted);
}

bool BackendModernEffectsOnDisplay(Backend *backend) { return backend && !backend->effectsInLinear; }

bool BackendModernCopyScene(Backend *backend, lg::Surface *scene, const lg::Rect *from, lg::Surface *dst,
                            const lg::Rect *to, bool linear)
{
    if (!backend || !backend->tonemap || !scene || !dst)
        return false;
    Image *sceneImage = lg::SurfaceImage(scene);
    Image *dstImage = lg::SurfaceImage(dst);
    if (!sceneImage || !dstImage)
    {
        LogModernOnce(backend, "a copy of the linear-light scene is not tonemapped",
                      "the target is not level 0 of a 2D texture");
        return false;
    }
    if (!lg::HandOverRecording(backend->legacy))
        return true; // the frame is not drawn: neither is the copy
    std::string error;
    if (!modern::WriteDisplay(backend->core, backend->tonemap, sceneImage, AreaOf(scene, from), dstImage,
                              AreaOf(dst, to), linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, "modern scene copy",
                              error, nullptr, 0.0f,
                              backend->exposureWanted ? modern::CurrentExposure(backend->exposure) : nullptr))
    {
        LogModernOnce(backend, "a copy of the linear-light scene is not tonemapped", error);
        return false;
    }
    ++backend->linearCopies;
    return true;
}

namespace
{
// A scene has a few hundred surfaces that move on their own at most; more
// means no scene ends.
constexpr size_t kMostSurfaces = 8192;

// The legacy device's last indexed draw as a surface for the velocity pass;
// false when it was not an indexed triangle list. prevClip may be null.
bool MovedFromLastDraw(lg::Device *legacy, const float clip[4][4], const float prevClip[4][4],
                       modern::MovedSurface &surface)
{
    lg::IndexedDraw draw;
    if (!lg::LastIndexedDraw(legacy, draw))
        return false;
    // The velocity pass reads these buffers at the scene's end; a dynamic
    // one must not be written again before then (BackendModernScene ends the
    // hold once the pass is recorded).
    lg::HoldLastIndexedBuffers(legacy);
    surface.vertices = draw.vertices;
    surface.vertexOffset = draw.vertexOffset;
    surface.stride = draw.stride;
    surface.indices = draw.indices;
    surface.indexType = draw.indexType;
    surface.firstIndex = draw.firstIndex;
    surface.indexCount = draw.indexCount;
    surface.vertexBase = draw.vertexBase;
    surface.viewport[0] = static_cast<float>(draw.viewport.x);
    surface.viewport[1] = static_cast<float>(draw.viewport.y);
    surface.viewport[2] = static_cast<float>(draw.viewport.width);
    surface.viewport[3] = static_cast<float>(draw.viewport.height);
    // As the Legacy adapter does: a depth range that is never empty.
    surface.minDepth = std::clamp(draw.viewport.minZ, 0.0f, 1.0f);
    surface.maxDepth =
        std::clamp(std::max(draw.viewport.maxZ, draw.viewport.minZ + (draw.viewport.minZ >= 0.5f ? 0.0f : 0.001f)),
                   0.0f, 1.0f);
    surface.minIndex = draw.minIndex;
    surface.vertexCount = draw.vertexCount;
    std::memcpy(surface.clip, clip, sizeof(surface.clip));
    if (prevClip)
        std::memcpy(surface.prevClip, prevClip, sizeof(surface.prevClip));
    return true;
}
} // namespace

void BackendModernSurfaceMoved(Backend *backend, lg::Device *legacy, const float clip[4][4],
                               const float prevClip[4][4])
{
    if (!backend || !legacy || !backend->velocityWanted || backend->velocityFailed || !prevClip)
        return;
    if (backend->moved.size() >= kMostSurfaces)
        return;
    modern::MovedSurface surface;
    if (MovedFromLastDraw(legacy, clip, prevClip, surface))
        backend->moved.push_back(surface);
}

void BackendModernSkinnedSurfaceMoved(Backend *backend, lg::Device *legacy, uint64_t key, const float clip[4][4],
                                      const float prevClip[4][4])
{
    if (!backend || !legacy || !backend->velocityWanted || backend->velocityFailed)
        return;
    if (backend->skinned.size() >= kMostSurfaces)
        return;
    Backend::SkinnedSurface surface{};
    if (!MovedFromLastDraw(legacy, clip, prevClip, surface.draw) || !surface.draw.vertexCount)
        return;
    surface.key = key;
    surface.hasLast = prevClip != nullptr;
    backend->skinned.push_back(surface);
}

namespace
{
// The velocity of the scene's surfaces that move on their own, recorded now
// (between passes); null when there are none or it cannot run (the reason
// is logged once), and TAA then uses the camera's motion alone.
Image *SceneVelocity(Backend *backend, Image *depth, const modern::SceneView &scene,
                     const std::vector<modern::MovedSurface> &moved)
{
    modern::SceneView previous{};
    if (moved.empty() || scene.cut || !modern::LastView(backend->taa, previous))
        return nullptr;
    if (!backend->velocity)
    {
        std::string error;
        backend->velocity = modern::CreateVelocity(backend->core, error, !backend->debugVelocityAll);
        if (!backend->velocity)
        {
            backend->velocityFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no velocity for what moves on its own: %s", error.c_str());
            return nullptr;
        }
    }
    std::string error;
    Image *image = modern::DrawVelocity(backend->core, backend->velocity, depth, scene, previous, moved.data(),
                                        static_cast<uint32_t>(moved.size()), error);
    if (!image && !error.empty())
        LogModernOnce(backend, "what moves on its own has no velocity of its own", error);
    return image;
}
} // namespace

namespace
{
// Keeps the scene's skinned surfaces' vertices for the next scene (between
// passes); false when they are not kept (the reason is logged once).
bool KeepSkinned(Backend *backend, const std::vector<Backend::SkinnedSurface> &skinned)
{
    if (skinned.empty() || !backend->velocityWanted || backend->velocityFailed)
        return false;
    if (!backend->kept)
        backend->kept = modern::CreateKeptVertices();
    std::vector<modern::KeepRequest> requests;
    requests.reserve(skinned.size());
    for (const Backend::SkinnedSurface &surface : skinned)
    {
        const modern::MovedSurface &draw = surface.draw;
        const int64_t first = static_cast<int64_t>(draw.vertexBase) + draw.minIndex;
        if (first < 0 || draw.stride != 32)
            continue;
        modern::KeepRequest &request = requests.emplace_back();
        request.key = surface.key;
        request.vertices = draw.vertices;
        request.offset = draw.vertexOffset + static_cast<VkDeviceSize>(first) * draw.stride;
        request.vertexCount = draw.vertexCount;
    }
    std::string error;
    if (!modern::KeepVertices(backend->core, backend->kept, requests.data(), static_cast<uint32_t>(requests.size()),
                              error))
    {
        LogModernOnce(backend, "skinned surfaces have no velocity of their own", error);
        return false;
    }
    return true;
}
} // namespace

namespace
{
// The velocity debug view over the anti-aliased scene (KISAK_VK_MODERN_DEBUG=velocity).
void DrawVelocityDebug(Backend *backend, Image *velocity, Image *color, const VkRect2D &area)
{
    if (backend->debugFailed)
        return;
    std::string error;
    if (!backend->debug)
    {
        backend->debug = modern::CreateDebugView(backend->core, error);
        if (!backend->debug)
        {
            backend->debugFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no velocity debug view: %s", error.c_str());
            return;
        }
    }
    if (!modern::DrawVelocityDebug(backend->core, backend->debug, velocity, color, area, error))
        LogModernOnce(backend, "the velocity debug view is not drawn", error);
}
} // namespace

namespace
{
// The normals debug view over a finished scene (KISAK_VK_MODERN_DEBUG=normals).
void DrawNormalsDebug(Backend *backend, Image *normals, Image *target, const VkRect2D &area)
{
    if (backend->debugFailed || !normals || !target)
        return;
    std::string error;
    if (!backend->debug)
    {
        backend->debug = modern::CreateDebugView(backend->core, error);
        if (!backend->debug)
        {
            backend->debugFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no normals debug view: %s", error.c_str());
            return;
        }
    }
    if (!modern::DrawNormalsDebug(backend->core, backend->debug, normals, target, area, error))
        LogModernOnce(backend, "the normals debug view is not drawn", error);
}

// The ambient occlusion debug view over a finished scene (KISAK_VK_MODERN_DEBUG=ao).
void DrawAoDebug(Backend *backend, Image *ao, Image *target, const VkRect2D &area)
{
    if (backend->debugFailed || !ao || !target)
        return;
    std::string error;
    if (!backend->debug)
    {
        backend->debug = modern::CreateDebugView(backend->core, error);
        if (!backend->debug)
        {
            backend->debugFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no ambient occlusion debug view: %s", error.c_str());
            return;
        }
    }
    if (!modern::DrawAoDebug(backend->core, backend->debug, ao, target, area, error))
        LogModernOnce(backend, "the ambient occlusion debug view is not drawn", error);
}

// The reflections debug view over a finished scene
// (KISAK_VK_MODERN_DEBUG=reflections); with `plain`, an image shown as it is
// (the planar view's, KISAK_VK_MODERN_DEBUG=planar).
void DrawReflectionsDebug(Backend *backend, Image *reflections, Image *target, const VkRect2D &area,
                          bool plain = false)
{
    if (backend->debugFailed || !reflections || !target)
        return;
    std::string error;
    if (!backend->debug)
    {
        backend->debug = modern::CreateDebugView(backend->core, error);
        if (!backend->debug)
        {
            backend->debugFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no reflections debug view: %s", error.c_str());
            return;
        }
    }
    const bool drawn = plain ? modern::DrawImageDebug(backend->core, backend->debug, reflections, target, area, error)
                             : modern::DrawReflectionsDebug(backend->core, backend->debug, reflections, target, area,
                                                            error);
    if (!drawn)
        LogModernOnce(backend, "the reflections debug view is not drawn", error);
}

modern::SceneView SceneViewOf(const ModernSceneView &view)
{
    modern::SceneView scene{};
    std::memcpy(scene.view, view.view, sizeof(scene.view));
    std::memcpy(scene.projection, view.projection, sizeof(scene.projection));
    scene.area = {{static_cast<int32_t>(view.x), static_cast<int32_t>(view.y)}, {view.width, view.height}};
    scene.cut = view.cut;
    return scene;
}
} // namespace

namespace
{
// The ambient occlusion module (pipelines for the occlusion and the contact
// shadows), made at its first use; false (logged once, and aoFailed) when it
// cannot be.
bool EnsureAoModule(Backend *backend)
{
    if (backend->ao)
        return true;
    if (backend->aoFailed)
        return false;
    std::string error;
    backend->ao = modern::CreateAmbientOcclusion(backend->core, error);
    if (!backend->ao)
    {
        backend->aoFailed = true;
        HostLog(*backend->host, "vulkan d3d9: modern: no ambient occlusion or contact shadows: %s", error.c_str());
        return false;
    }
    return true;
}

// The texture the lit shaders of ours read the ambient occlusion (red) and
// the contact shadows (green) from, made, or made again, at the depth's
// size; false (logged once, and aoFailed) when it cannot be.
bool EnsureAoTexture(Backend *backend, Image *depth)
{
    const ImageDesc &depthInfo = ImageInfo(depth);
    if (backend->aoTexture)
    {
        const lg::TextureDesc &have = lg::Describe(backend->aoTexture);
        if (have.width != depthInfo.width || have.height != depthInfo.height)
        {
            lg::DestroyTexture(backend->legacy, backend->aoTexture);
            backend->aoTexture = nullptr;
            backend->aoSurface = nullptr;
        }
    }
    if (backend->aoTexture)
        return true;
    lg::TextureDesc desc;
    desc.width = depthInfo.width;
    desc.height = depthInfo.height;
    desc.levels = 1;
    desc.usage = lg::d3d::USAGE_RENDERTARGET;
    desc.format = lg::d3d::FMT_A8R8G8B8;
    desc.pool = lg::d3d::POOL_DEFAULT;
    desc.name = "modern ambient occlusion";
    backend->aoTexture = lg::CreateTexture(backend->legacy, desc);
    backend->aoSurface = backend->aoTexture ? lg::GetSurfaceLevel(backend->legacy, backend->aoTexture, 0, 0) : nullptr;
    if (!backend->aoSurface || !lg::SurfaceImage(backend->aoSurface))
    {
        if (backend->aoTexture)
            lg::DestroyTexture(backend->legacy, backend->aoTexture);
        backend->aoTexture = nullptr;
        backend->aoSurface = nullptr;
        backend->aoFailed = true;
        HostLog(*backend->host, "vulkan d3d9: modern: no ambient occlusion or contact shadows: cannot make the %ux%u "
                                "image",
                depthInfo.width, depthInfo.height);
        return false;
    }
    return true;
}

// The scene's ambient occlusion into the back end's texture, right after its
// normals; sceneAo tells whether it is there. Off or failing, the reason is
// logged once.
void SceneAmbientOcclusion(Backend *backend, Image *depth, const modern::SceneView &scene)
{
    if (!backend->aoWanted || !EnsureAoModule(backend) || !EnsureAoTexture(backend, depth))
        return;
    std::string error;
    modern::AoParams params = backend->aoParams;
    params.frame = backend->aoFrame++;
    backend->sceneAo = modern::BuildAmbientOcclusion(backend->core, backend->ao, depth, backend->sceneNormals,
                                                     scene, params, lg::SurfaceImage(backend->aoSurface), error);
    if (!backend->sceneAo)
        LogModernOnce(backend, "no ambient occlusion", error);
    // The lit shaders of ours read it until the scene is lit (BackendModernScene).
    lg::SetModernAmbientOcclusion(backend->legacy, backend->sceneAo ? backend->aoTexture : nullptr);
}

// The scene's contact shadows into the green of the same texture, after the
// occlusion (into a texture cleared to white without it); sceneContact
// tells whether they are there.
void SceneContactShadows(Backend *backend, Image *depth, const modern::SceneView &scene)
{
    if (!backend->contactWanted || !backend->contactSun || !EnsureAoModule(backend)
        || !EnsureAoTexture(backend, depth))
        return;
    std::string error;
    modern::ContactParams params = backend->contactParams;
    params.frame = backend->contactFrame++;
    backend->sceneContact =
        modern::TraceContactShadows(backend->core, backend->ao, depth, backend->sceneNormals, scene, params,
                                    lg::SurfaceImage(backend->aoSurface), !backend->sceneAo, error);
    if (!backend->sceneContact)
        LogModernOnce(backend, "no contact shadows", error);
    else
        lg::SetModernAmbientOcclusion(backend->legacy, backend->aoTexture);
}

// The scene's screen-space reflections, after its normals buffer. They read
// last scene's anti-aliased colour, so they need that scene in linear light
// and kept by the anti-aliasing (none on a cut or the first scene; the
// readers then have none for this scene).
void TraceSceneReflections(Backend *backend, Image *depth, const modern::SceneView &view)
{
    backend->sceneReflections = nullptr;
    if (!backend->reflectionsWanted || backend->reflectionsFailed)
        return;
    if (!backend->linearWanted || backend->linearFailed || !backend->linearScene)
    {
        LogModernOnce(backend, "no screen-space reflections", "the scene is not drawn in linear light");
        return;
    }
    modern::SceneView previous{};
    Image *history = modern::LastHistory(backend->taa);
    if (view.cut || !history || !modern::LastView(backend->taa, previous))
        return;
    if (!backend->reflections)
    {
        std::string error;
        backend->reflections = modern::CreateReflections(backend->core, error);
        if (!backend->reflections)
        {
            backend->reflectionsFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no screen-space reflections: %s", error.c_str());
            return;
        }
    }
    std::string error;
    backend->sceneReflections = modern::TraceReflections(backend->core, backend->reflections, depth,
                                                         backend->sceneNormals, history, view, previous,
                                                         backend->reflectionParams, error);
    if (!backend->sceneReflections)
        LogModernOnce(backend, "no screen-space reflections", error);
    // The lit shaders of ours read them until the scene is lit (BackendModernScene).
    lg::SetModernReflections(backend->legacy, backend->sceneReflections);
}

// The planar reflection the scene's lit shaders read: last frame's planar
// image (drawn after last frame's main view), with where this frame's points
// are in it. None on a cut (last frame's camera was elsewhere) or before the
// first planar view.
void SetScenePlanar(Backend *backend, const ModernSceneView &view)
{
    lg::SetModernPlanar(backend->legacy, nullptr, nullptr);
    if (!BackendModernPlanarWanted(backend) || !backend->planarReady || !backend->planarColorSurface || view.cut)
        return;
    Image *image = lg::SurfaceImage(backend->planarColorSurface);
    if (!image)
        return;
    // The eye: the view matrix takes it to the origin (row vectors,
    // p_view = p * R + t, so eye = -t * R^T).
    const float (*v)[4] = view.view;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    // [p - eye, 1] * T(eye) * view * projection, T(eye) the translation by eye.
    const ModernPlanarView &planar = backend->planarView;
    float viewProjection[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += planar.view[r][k] * planar.projection[k][c];
            viewProjection[r][c] = sum;
        }
    lg::ModernPlanarParams params{};
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 3; ++r)
            params.clip[r][c] = viewProjection[r][c];
        params.clip[3][c] = viewProjection[3][c] + eye[0] * viewProjection[0][c] + eye[1] * viewProjection[1][c]
                            + eye[2] * viewProjection[2][c];
    }
    params.plane[0] = planar.plane[0];
    params.plane[1] = planar.plane[1];
    params.plane[2] = planar.plane[2];
    params.plane[3] = planar.plane[0] * eye[0] + planar.plane[1] * eye[1] + planar.plane[2] * eye[2] + planar.plane[3];
    params.on = backend->debugPlanarRead ? 2.0f : 1.0f;
    params.reach = backend->planarReach;
    params.facing = 0.95f;
    params.ripple = backend->planarRipple;
    lg::SetModernPlanar(backend->legacy, image, &params);
    // With the read's debug view, the matrices behind it now and then, to
    // check a reflection's place against the scene's own geometry.
    if (backend->debugPlanarRead && backend->planarReads % 600 == 0)
    {
        const float (*pv)[4] = planar.view;
        const float (*pp)[4] = planar.projection;
        float mirrorEye[3];
        for (int i = 0; i < 3; ++i)
            mirrorEye[i] = -(pv[3][0] * pv[i][0] + pv[3][1] * pv[i][1] + pv[3][2] * pv[i][2]);
        HostLog(*backend->host,
                "vulkan d3d9: modern: planar read: eye %.2f %.2f %.2f, mirror eye %.2f %.2f %.2f, plane %.3f %.3f "
                "%.3f %.2f",
                eye[0], eye[1], eye[2], mirrorEye[0], mirrorEye[1], mirrorEye[2], planar.plane[0], planar.plane[1],
                planar.plane[2], planar.plane[3]);
        for (int r = 0; r < 4; ++r)
            HostLog(*backend->host,
                    "vulkan d3d9: modern: planar read: row %d: view %.4f %.4f %.4f %.4f, projection %.4f %.4f %.4f "
                    "%.4f, main view %.4f %.4f %.4f %.4f, main projection %.4f %.4f %.4f %.4f",
                    r, pv[r][0], pv[r][1], pv[r][2], pv[r][3], pp[r][0], pp[r][1], pp[r][2], pp[r][3], v[r][0],
                    v[r][1], v[r][2], v[r][3], view.projection[r][0], view.projection[r][1], view.projection[r][2],
                    view.projection[r][3]);
    }
    ++backend->planarReads;
}
} // namespace

bool BackendModernReflectionProbe(Backend *backend, lg::Device *legacy, lg::Texture *probe)
{
    if (!backend || !backend->probesWanted || !legacy || !probe)
        return false;
    const lg::TextureDesc &desc = lg::Describe(probe);
    const uint32_t levels = desc.levels;
    if (desc.kind != lg::TextureKind::Cube || desc.width < 2 || levels < 2 || levels > 16
        || (desc.format != lg::d3d::FMT_A8R8G8B8 && desc.format != lg::d3d::FMT_X8R8G8B8))
    {
        ++backend->probesLeft;
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    // Every level read back (level 0 is what the filter reads; the others
    // are written whole), filtered, and the levels below 0 written back.
    std::vector<std::vector<uint8_t>> copies(6 * levels);
    std::vector<uint8_t *> bits(6 * levels);
    std::vector<uint32_t> pitches(6 * levels);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t size = std::max(desc.width >> level, 1u);
            std::vector<uint8_t> &copy = copies[face * levels + level];
            copy.assign(static_cast<size_t>(size) * size * 4, 0);
            if (level == 0)
            {
                lg::LockedRect locked;
                if (!lg::LockRect(legacy, probe, face, 0, nullptr, lg::d3d::LOCK_READONLY, locked))
                {
                    ++backend->probesLeft;
                    return false;
                }
                for (uint32_t y = 0; y < size; ++y)
                    std::memcpy(copy.data() + static_cast<size_t>(y) * size * 4,
                                static_cast<const uint8_t *>(locked.bits) + static_cast<size_t>(y) * locked.pitch,
                                static_cast<size_t>(size) * 4);
                lg::UnlockRect(legacy, probe, face, 0);
                if (desc.format == lg::d3d::FMT_X8R8G8B8)
                    for (size_t i = 3; i < copy.size(); i += 4)
                        copy[i] = 255;
            }
            bits[face * levels + level] = copy.data();
            pitches[face * levels + level] = size * 4;
        }
    if (!modern::FilterReflectionProbe(desc.width, levels, bits.data(), pitches.data()))
    {
        ++backend->probesLeft;
        return false;
    }
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t level = 1; level < levels; ++level)
        {
            const uint32_t size = std::max(desc.width >> level, 1u);
            lg::LockedRect locked;
            if (!lg::LockRect(legacy, probe, face, level, nullptr, lg::d3d::LOCK_DISCARD, locked))
                continue;
            const std::vector<uint8_t> &copy = copies[face * levels + level];
            for (uint32_t y = 0; y < size; ++y)
                std::memcpy(static_cast<uint8_t *>(locked.bits) + static_cast<size_t>(y) * locked.pitch,
                            copy.data() + static_cast<size_t>(y) * size * 4, static_cast<size_t>(size) * 4);
            lg::UnlockRect(legacy, probe, face, level);
        }
    ++backend->probesFiltered;
    backend->probesMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

namespace
{
// An IEEE half (binary16) as a float.
float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    const uint32_t mantissa = half & 0x3ffu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            // Subnormal: 2^-14 times mantissa / 1024.
            const float value = std::ldexp(static_cast<float>(mantissa), -24);
            return sign ? -value : value;
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7f800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

bool BackendModernProbeCaptureWanted(Backend *backend)
{
    return backend && backend->legacy && backend->core && backend->probeCaptureWanted && !backend->linearFailed;
}

void BackendModernProbeCapture(Backend *backend, bool on)
{
    if (!backend)
        return;
    backend->probeCapturing = on && BackendModernProbeCaptureWanted(backend);
    if (!backend->probeCapturing && backend->probeCaptureReadback && backend->core)
    {
        DestroyReadback(backend->core, backend->probeCaptureReadback);
        backend->probeCaptureReadback = nullptr;
    }
}

bool BackendModernProbeCaptureRead(Backend *backend, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   float *rgb)
{
    if (!backend || !backend->core || !backend->probeCaptureReadback || !rgb || !width || !height)
        return false;
    ReadbackData data{};
    if (!WaitReadback(backend->core, backend->probeCaptureReadback, &data) || !data.pixels
        || data.format != modern::kSceneFormat || uint64_t(x) + width > data.width
        || uint64_t(y) + height > data.height)
        return false;
    for (uint32_t row = 0; row < height; ++row)
    {
        const uint8_t *from = data.pixels + static_cast<size_t>(y + row) * data.rowBytes + static_cast<size_t>(x) * 8;
        float *to = rgb + static_cast<size_t>(row) * width * 3;
        for (uint32_t column = 0; column < width; ++column)
            for (uint32_t c = 0; c < 3; ++c)
            {
                uint16_t half;
                std::memcpy(&half, from + column * 8 + c * 2, sizeof(half));
                to[column * 3 + c] = HalfToFloat(half);
            }
    }
    ++backend->probeCaptureReads;
    return true;
}

bool BackendModernPlanarWanted(Backend *backend)
{
    return backend && backend->legacy && backend->planarWanted && !backend->planarFailed && !backend->linearFailed;
}

bool BackendModernPlanarBegin(Backend *backend, const ModernPlanarView &view, lg::Surface *&color, lg::Surface *&depth)
{
    color = nullptr;
    depth = nullptr;
    if (!BackendModernPlanarWanted(backend) || !view.width || !view.height)
        return false;
    if (backend->planarColor)
    {
        const lg::TextureDesc &have = lg::Describe(backend->planarColor);
        if (have.width != view.width || have.height != view.height)
        {
            lg::DestroyTexture(backend->legacy, backend->planarColor);
            lg::DestroySurface(backend->legacy, backend->planarDepth);
            backend->planarColor = nullptr;
            backend->planarColorSurface = nullptr;
            backend->planarDepth = nullptr;
            backend->planarReady = false;
        }
    }
    if (!backend->planarColor)
    {
        lg::TextureDesc desc;
        desc.width = view.width;
        desc.height = view.height;
        desc.levels = 1;
        desc.usage = lg::d3d::USAGE_RENDERTARGET;
        desc.format = lg::d3d::FMT_A16B16G16R16F;
        desc.pool = lg::d3d::POOL_DEFAULT;
        desc.name = "modern planar reflection";
        backend->planarColor = lg::CreateTexture(backend->legacy, desc);
        backend->planarColorSurface =
            backend->planarColor ? lg::GetSurfaceLevel(backend->legacy, backend->planarColor, 0, 0) : nullptr;
        backend->planarDepth = backend->planarColorSurface
                                   ? lg::CreateDepthStencil(backend->legacy, view.width, view.height,
                                                            lg::d3d::FMT_D24S8, "modern planar reflection depth")
                                   : nullptr;
        if (!backend->planarDepth)
        {
            if (backend->planarColor)
                lg::DestroyTexture(backend->legacy, backend->planarColor);
            backend->planarColor = nullptr;
            backend->planarColorSurface = nullptr;
            backend->planarFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no planar reflections: cannot make the %ux%u images",
                    view.width, view.height);
            return false;
        }
        lg::SetLinearLight(backend->planarColor, true);
    }
    // The planar view's own lit shaders do not read the image they draw into.
    lg::SetModernPlanar(backend->legacy, nullptr, nullptr);
    ++backend->planarViews;
    color = backend->planarColorSurface;
    depth = backend->planarDepth;
    return true;
}

void BackendModernSunShadow(Backend *backend, lg::Device *legacy, const ModernSunShadowView &view)
{
    if (!backend || !legacy)
        return;
    // The sun, for the contact shadows of the view's scene depth (next).
    backend->contactSun = view.sun;
    for (int i = 0; i < 3; ++i)
        backend->contactParams.sun[i] = view.sunDir[i];
    const bool usable = view.on && view.texel[0] > 0.0f && view.texel[1] > 0.0f && view.depthUnits[0] > 0.0f
                        && view.depthUnits[1] > 0.0f;
    const bool sun = backend->softShadowsWanted && usable;
    const bool spot = backend->softSpotShadowsWanted;
    if (!sun && !spot)
    {
        lg::SetModernShadow(legacy, nullptr);
        return;
    }
    // The filter's radius (texels) is the receiver's depth below its
    // blockers times the tangent of half the sun's apparent size, over a
    // texel's width (a spot light's: its size, in the shader); it reaches at
    // most 16 texels, the blocker search with it, so a soft edge costs the
    // same taps everywhere.
    constexpr float kWidestTexels = 16.0f;
    constexpr float kPi = 3.14159265f;
    lg::ModernShadowParams params{};
    params.maxRadius = kWidestTexels;
    if (sun)
    {
        const float tanHalf = std::tan(0.5f * backend->shadowSoftness * kPi / 180.0f);
        for (int i = 0; i < 2; ++i)
        {
            params.penumbra[i] = view.depthUnits[i] * tanHalf / view.texel[i];
            // The steepest receiver the per-tap bias follows: two units of
            // height a unit across (63 degrees), in depth per texel.
            params.slopeCap[i] = 2.0f * view.texel[i] / view.depthUnits[i];
        }
        params.on += lg::kModernSoftSun;
        ++backend->softShadowScenes;
    }
    if (spot)
    {
        params.spotSize = backend->spotLightSize;
        params.on += lg::kModernSoftSpot;
    }
    params.frame = static_cast<float>(backend->shadowFrame++ & 63u);
    lg::SetModernShadow(legacy, &params);
}

void BackendModernPlanarDone(Backend *backend, const ModernPlanarView &view)
{
    if (!backend || !backend->planarColor)
        return;
    backend->planarView = view;
    backend->planarReady = true;
}

// ---- Scene passes ----------------------------------------------------------------
//
// Where the Modern path's own passes (render or compute) slot into a scene,
// beside the ones above that have places of their own (normals, AO and
// reflections after the depth prepass; velocity, TAA, bloom, exposure and
// the tonemap at the end).

// Once per scene, between passes: the lit surfaces are drawn into `scene`
// (the linear-light image when `linear`), with its depth, and nothing has
// read it yet: before velocity, TAA, bloom, exposure and the tonemap, and
// before the effects unless they are drawn in linear light. Passes that
// change the lit scene, or build what a later point draws, go here. Not run
// when the scene is refused before the tonemap without linear light.
void LitScenePasses(Backend *backend, Image *scene, Image *depth, const modern::SceneView &view, bool linear)
{
    (void)scene;
    (void)depth;
    (void)view;
    (void)linear;
    if (backend->debugCompute && !backend->computeCheckFailed)
    {
        std::string error;
        if (!backend->computeCheck)
        {
            backend->computeCheck = modern::CreateComputeCheck(backend->core, error);
            if (!backend->computeCheck)
            {
                backend->computeCheckFailed = true;
                HostLog(*backend->host, "vulkan d3d9: modern: no compute check: %s", error.c_str());
                return;
            }
        }
        if (!modern::BuildComputeCheck(backend->core, backend->computeCheck, error))
            LogModernOnce(backend, "the compute check is not built", error);
    }
}

// Once per scene, between passes: the scene is finished in `target` (the
// display image after the tonemap, or the anti-aliased scene without linear
// light) within `area`. Debug views go here.
void FinishedScenePasses(Backend *backend, Image *target, const VkRect2D &area)
{
    if (backend->debugCompute && backend->computeCheck && target)
    {
        std::string error;
        if (!modern::DrawComputeCheck(backend->core, backend->computeCheck, target, area, 0.5f, error))
            LogModernOnce(backend, "the compute check is not drawn", error);
    }
}

bool BackendModernWantsSceneDepth(Backend *backend)
{
    return backend && backend->legacy && backend->core && backend->normalsWanted && !backend->normalsFailed;
}

ModernResult BackendModernSceneDepth(Backend *backend, lg::Surface *depth, const ModernSceneView &view)
{
    if (!BackendModernWantsSceneDepth(backend))
        return ModernResult::Skipped;
    backend->sceneNormals = nullptr;
    backend->sceneAo = false;
    backend->sceneContact = false;
    lg::SetModernAmbientOcclusion(backend->legacy, nullptr);
    lg::SetModernReflections(backend->legacy, nullptr);
    Image *depthImage = depth ? lg::SurfaceImage(depth) : nullptr;
    if (!depthImage)
    {
        LogModernOnce(backend, "no normals buffer",
                      depth ? "the depth surface is not level 0 of a 2D texture" : "no depth surface is set");
        return ModernResult::Refused;
    }
    if (!backend->normals)
    {
        std::string error;
        backend->normals = modern::CreateNormals(backend->core, error);
        if (!backend->normals)
        {
            backend->normalsFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no normals buffer: %s", error.c_str());
            return ModernResult::Refused;
        }
    }
    // Ends the Legacy pass; the lit draws begin a new one on the same images.
    if (!lg::HandOverRecording(backend->legacy))
        return ModernResult::Skipped;
    std::string error;
    const modern::SceneView scene = SceneViewOf(view);
    backend->sceneNormals = modern::BuildNormals(backend->core, backend->normals, depthImage, scene, error);
    if (!backend->sceneNormals)
    {
        LogModernOnce(backend, "no normals buffer", error);
        return ModernResult::Refused;
    }
    SceneAmbientOcclusion(backend, depthImage, scene);
    SceneContactShadows(backend, depthImage, scene);
    TraceSceneReflections(backend, depthImage, scene);
    SetScenePlanar(backend, view);
    return ModernResult::Done;
}

namespace
{
// Volumetric fog over the linear-light scene (vk_modern.h DrawFog), when it
// is wanted and the view has fog. A failure is logged once and the scene
// goes on without it.
void DrawSceneFog(Backend *backend, Image *color, Image *depth, Image *sunShadow, Image *spotShadow,
                  const ModernSceneView &view, const modern::SceneView &scene)
{
    if (!backend->fogWanted || backend->fogFailed || !view.fog.on)
        return;
    if (!backend->fog)
    {
        std::string error;
        backend->fog = modern::CreateFog(backend->core, error);
        if (!backend->fog)
        {
            backend->fogFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no volumetric fog: %s", error.c_str());
            return;
        }
    }
    modern::FogInputs inputs;
    std::memcpy(inputs.color, view.fog.color, sizeof(inputs.color));
    inputs.density = view.fog.density;
    inputs.start = view.fog.start;
    std::memcpy(inputs.origin, view.fog.origin, sizeof(inputs.origin));
    std::memcpy(inputs.sunDirection, view.fog.sunDirection, sizeof(inputs.sunDirection));
    inputs.sunShadowed = view.fog.sunShadowed;
    std::memcpy(inputs.shadowLookup, view.fog.shadowLookup, sizeof(inputs.shadowLookup));
    std::memcpy(inputs.switchPartition, view.fog.switchPartition, sizeof(inputs.switchPartition));
    std::memcpy(inputs.shadowmapScale, view.fog.shadowmapScale, sizeof(inputs.shadowmapScale));
    inputs.spotCount = 0;
    for (int i = 0; i < view.fog.spotCount && i < static_cast<int>(modern::kFogSpots); ++i)
    {
        const ModernSceneView::Fog::Spot &from = view.fog.spots[i];
        modern::FogInputs::Spot &to = inputs.spots[inputs.spotCount++];
        std::memcpy(to.origin, from.origin, sizeof(to.origin));
        to.radius = from.radius;
        std::memcpy(to.dir, from.dir, sizeof(to.dir));
        to.exponent = from.exponent;
        to.cosHalfFovOuter = from.cosHalfFovOuter;
        to.cosHalfFovInner = from.cosHalfFovInner;
        to.fade = from.fade;
        std::memcpy(to.color, from.color, sizeof(to.color));
        std::memcpy(to.shadowLookup, from.shadowLookup, sizeof(to.shadowLookup));
        to.inSunMap = from.inSunMap;
    }
    modern::FogParams params = backend->fogParams;
    params.frame = backend->fogFrame++;
    params.debug = backend->debugFog;
    std::string error;
    if (!modern::DrawFog(backend->core, backend->fog, color, depth, sunShadow, spotShadow, scene, inputs, params,
                         error))
        LogModernOnce(backend, "no volumetric fog", error);
}

// God rays over the linear-light scene (vk_modern.h DrawRays), when they are
// wanted and the view has a sun. A failure is logged once and the scene goes
// on without them.
void DrawSceneRays(Backend *backend, Image *color, Image *depth, const ModernSceneView &view,
                   const modern::SceneView &scene)
{
    if (!backend->raysWanted || backend->raysFailed || !view.sun.on)
        return;
    if (!backend->rays)
    {
        std::string error;
        backend->rays = modern::CreateRays(backend->core, error);
        if (!backend->rays)
        {
            backend->raysFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no god rays: %s", error.c_str());
            return;
        }
    }
    modern::RaysInputs inputs;
    std::memcpy(inputs.sunDirection, view.sun.direction, sizeof(inputs.sunDirection));
    std::memcpy(inputs.sunColor, view.sun.color, sizeof(inputs.sunColor));
    modern::RaysParams params = backend->raysParams;
    params.frame = backend->raysFrame++;
    bool drawn = false;
    std::string error;
    if (!modern::DrawRays(backend->core, backend->rays, color, depth, scene, inputs, params, drawn, error))
        LogModernOnce(backend, "no god rays", error);
}
} // namespace

ModernResult BackendModernScene(Backend *backend, lg::Surface *color, lg::Surface *depth,
                                const ModernSceneView &view, lg::Surface *display, bool *glowDrawn,
                                lg::Surface *sunShadow, lg::Surface *spotShadow)
{
    if (!backend || !backend->legacy || !backend->core)
        return ModernResult::Refused;
    // The scene's normals buffer, if its depth prepass built one; the next
    // scene builds its own.
    Image *normals = backend->sceneNormals;
    backend->sceneNormals = nullptr;
    Image *ao = backend->sceneAo ? lg::SurfaceImage(backend->aoSurface) : nullptr;
    backend->sceneAo = false;
    backend->sceneContact = false;
    lg::SetModernAmbientOcclusion(backend->legacy, nullptr);
    Image *reflections = backend->sceneReflections;
    backend->sceneReflections = nullptr;
    lg::SetModernReflections(backend->legacy, nullptr);
    lg::SetModernPlanar(backend->legacy, nullptr, nullptr);
    // The surfaces that moved in this scene; the next scene starts a new list.
    std::vector<modern::MovedSurface> moved;
    moved.swap(backend->moved);
    backend->moved.reserve(moved.capacity());
    std::vector<Backend::SkinnedSurface> skinned;
    skinned.swap(backend->skinned);
    backend->skinned.reserve(skinned.capacity());
    // Unless this scene keeps its skinned vertices, the next has none of
    // last scene's to draw from (what was kept before is older than that).
    struct ForgetUnlessKept
    {
        modern::KeptVertices *kept;
        bool done = false;
        ~ForgetUnlessKept()
        {
            if (!done)
                modern::ForgetKept(kept);
        }
    } keptGuard{backend->kept};
    // The buffers the scene's moved surfaces were drawn from are held until
    // the velocity pass and the copy of skinned vertices are recorded, and
    // so to the end of this scene whatever it does.
    struct ReleaseHeld
    {
        lg::Device *legacy;
        ~ReleaseHeld() { lg::ReleaseHeldBuffers(legacy); }
    } heldGuard{backend->legacy};
    // The scene passes at its lit point (LitScenePasses), once: before the
    // anti-aliasing, or before the tonemap when that is refused.
    bool litPassesRun = false;
    const auto litPasses = [&]() {
        if (litPassesRun)
            return;
        litPassesRun = true;
        LitScenePasses(backend, color ? lg::SurfaceImage(color) : nullptr, depth ? lg::SurfaceImage(depth) : nullptr,
                       SceneViewOf(view), display != nullptr);
    };
    // In linear light the scene must reach the back buffer whatever the
    // anti-aliasing does.
    const auto tonemap = [&](ModernResult passes) {
        if (!display)
            return passes;
        if (!lg::HandOverRecording(backend->legacy))
            return ModernResult::Skipped;
        litPasses();
        const VkRect2D area = {{static_cast<int32_t>(view.x), static_cast<int32_t>(view.y)}, {view.width, view.height}};
        Image *scene = lg::SurfaceImage(color);
        if (backend->probeCapturing && scene)
        {
            // A reflection probe's face: the light before bloom, exposure
            // and the tonemap (BackendModernProbeCaptureRead).
            if (backend->probeCaptureReadback)
                DestroyReadback(backend->core, backend->probeCaptureReadback);
            backend->probeCaptureReadback = ReadImage(backend->core, scene, 0, 0);
            if (backend->probeCaptureReadback)
                ++backend->probeCaptureScenes;
        }
        float bloomScale = 0.0f;
        Image *bloom = DrawBloom(backend, scene, area, view, bloomScale);
        Image *exposure = MeasureExposure(backend, scene, area, view.cut);
        std::string error;
        if (!modern::WriteDisplay(backend->core, backend->tonemap, scene, area, lg::SurfaceImage(display), area,
                                  VK_FILTER_NEAREST, "modern tonemap", error, bloom, bloomScale, exposure))
        {
            LogModernOnce(backend, "the linear-light scene is not tonemapped", error);
            return ModernResult::Refused;
        }
        if (bloom && glowDrawn)
            *glowDrawn = true;
        if (backend->debugNormals)
            DrawNormalsDebug(backend, normals, lg::SurfaceImage(display), area);
        if (backend->debugAo)
            DrawAoDebug(backend, ao, lg::SurfaceImage(display), area);
        if (backend->debugReflections)
            DrawReflectionsDebug(backend, reflections, lg::SurfaceImage(display), area);
        if (backend->debugPlanar && backend->planarReady && backend->planarColorSurface)
        {
            // Last frame's planar view, as it is, over the scene's top left.
            const VkRect2D planarArea = {area.offset,
                                         {std::min(area.extent.width, backend->planarView.width),
                                          std::min(area.extent.height, backend->planarView.height)}};
            DrawReflectionsDebug(backend, lg::SurfaceImage(backend->planarColorSurface), lg::SurfaceImage(display),
                                 planarArea, true);
        }
        FinishedScenePasses(backend, lg::SurfaceImage(display), area);
        return passes;
    };
    if (backend->taaFailed)
        return tonemap(ModernResult::Refused);
    const auto refuse = [&](const std::string &reason) {
        LogModernOnce(backend, "the scene is not anti-aliased", reason);
        return tonemap(ModernResult::Refused);
    };
    if (!color || !depth)
        return refuse(color ? "no depth surface is set" : "no render target 0 is set");
    Image *colorImage = lg::SurfaceImage(color);
    Image *depthImage = lg::SurfaceImage(depth);
    if (!colorImage || !depthImage)
        return refuse("render target 0 or the depth surface is not level 0 of a 2D texture");
    if (!backend->taa)
    {
        std::string error;
        backend->taa = modern::CreateTaa(backend->core, error);
        if (!backend->taa)
        {
            backend->taaFailed = true;
            HostLog(*backend->host, "vulkan d3d9: modern: no anti-aliasing: %s", error.c_str());
            return tonemap(ModernResult::Refused);
        }
    }
    if (!lg::HandOverRecording(backend->legacy))
        return ModernResult::Skipped; // nothing of this frame is drawn either
    if (display)
    {
        backend->lastSceneSeen = true;
        backend->lastFogOn = view.fog.on;
        backend->lastFogStart = view.fog.start;
        backend->lastFogDensity = view.fog.density;
        std::memcpy(backend->lastFogColor, view.fog.color, sizeof(backend->lastFogColor));
        backend->lastSunOn = view.sun.on;
        std::memcpy(backend->lastSunColor, view.sun.color, sizeof(backend->lastSunColor));
        std::memcpy(backend->lastSunDirection, view.sun.direction, sizeof(backend->lastSunDirection));
    }
    const modern::SceneView scene = SceneViewOf(view);
    std::string error;
    // The fog is lit before the anti-aliasing, which averages its noise away.
    if (display)
        DrawSceneFog(backend, colorImage, depthImage, sunShadow ? lg::SurfaceImage(sunShadow) : nullptr,
                     spotShadow ? lg::SurfaceImage(spotShadow) : nullptr, view, scene);
    // The god rays after it, before the anti-aliasing, which averages their noise away.
    if (display)
        DrawSceneRays(backend, colorImage, depthImage, view, scene);
    // Skinned surfaces whose vertices the last scene kept are drawn from
    // those and this scene's.
    if (!scene.cut)
        for (const Backend::SkinnedSurface &surface : skinned)
        {
            if (!surface.hasLast)
                continue;
            modern::MovedSurface draw = surface.draw;
            if (modern::FindKept(backend->kept, surface.key, draw.vertexCount, draw.previous, draw.previousOffset))
                moved.push_back(draw);
        }
    litPasses();
    Image *velocity = SceneVelocity(backend, depthImage, scene, moved);
    keptGuard.done = KeepSkinned(backend, skinned);
    if (!modern::Resolve(backend->core, backend->taa, colorImage, depthImage, scene, error, velocity))
        return refuse(error);
    if (backend->debugVelocity && velocity)
        DrawVelocityDebug(backend, velocity, colorImage, scene.area);
    // Without linear light the scene is finished here.
    if (backend->debugNormals && !display)
        DrawNormalsDebug(backend, normals, colorImage, scene.area);
    if (backend->debugAo && !display)
        DrawAoDebug(backend, ao, colorImage, scene.area);
    if (!display)
        FinishedScenePasses(backend, colorImage, scene.area);
    return tonemap(ModernResult::Done);
}

void BackendPollProgress(Backend *backend)
{
    // The core's one call for any thread (vk_core.h). The core is created
    // before the device has objects and destroyed after the last has gone.
    if (backend && backend->core)
        RefreshProgress(backend->core);
}
} // namespace gfx_vk::d3d9
