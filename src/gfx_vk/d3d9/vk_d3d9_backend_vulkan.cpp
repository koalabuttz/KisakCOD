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
    modern::TonemapCurve tonemapCurve = modern::TonemapCurve::Clamp;
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
    uint64_t litScenes = 0, litScenesBase = 0;
    uint64_t litChanges = 0, litChangesBase = 0;
    // Bloom from the linear-light scene in place of the renderer's glow
    // (vk_d3d9.h ModernSceneDone): made at the first scene with glow, unless
    // KISAK_VK_MODERN_BLOOM=0; the scenes it ran for, and at the last reset.
    bool bloomWanted = true;
    bool bloomFailed = false;
    modern::Bloom *bloom = nullptr;
    uint64_t bloomScenes = 0, bloomScenesBase = 0;
    uint64_t bloomPassesBase = 0;
    // The bloom passes' GPU time at the last reset (pass timings add up over
    // the device's life).
    double bloomMsBase = 0.0;
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
// Whether the environment variable is set to "0".
bool EnvIs(const char *name, const char *value)
{
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
} // namespace

Backend *OpenBackend(const BackendDesc &desc, std::string &error)
{
    const Host &h = *desc.host;
    Backend *backend = new Backend;
    backend->host = desc.host;
    // The Modern path draws its scenes in linear light unless this says not to
    // (docs/vulkan-modern.md); without r_visuals modern nothing asks for it.
    backend->linearWanted = !EnvIsZero("KISAK_VK_MODERN_LINEAR");
    backend->bloomWanted = !EnvIsZero("KISAK_VK_MODERN_BLOOM");
    backend->effectsInLinear = EnvIs("KISAK_VK_MODERN_EFFECTS", "linear");
    backend->litWanted = !EnvIsZero("KISAK_VK_MODERN_LIT");
    {
        const std::string curve = EnvText("KISAK_VK_MODERN_TONEMAP");
        if (!curve.empty() && !modern::CurveFromName(curve.c_str(), backend->tonemapCurve))
            backend->tonemapCurveUnknown = true;
    }
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
    }
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
    if (reset)
    {
        if (backend->taa)
            backend->taaBase = modern::Stats(backend->taa);
        backend->bloomScenesBase = backend->bloomScenes;
        backend->bloomPassesBase = modern::Stats(backend->bloom).passes;
        if (backend->bloom)
            backend->bloomMsBase = PassTimingsFor(backend->core, "modern bloom").totalMs;
        backend->linearScenesBase = backend->linearScenes;
        backend->litScenesBase = backend->litScenes;
        backend->litChangesBase = backend->litChanges;
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
    if (backend->bloom)
    {
        modern::DestroyBloom(backend->core, backend->bloom);
        backend->bloom = nullptr;
    }
    if (backend->tonemap)
    {
        modern::DestroyTonemap(backend->core, backend->tonemap);
        backend->tonemap = nullptr;
    }
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
    result = ModernResult::Done;
    return backend->linearSceneSurface;
}

void BackendModernLitInputs(Backend *backend, const ModernLitInputs *inputs)
{
    if (!backend || !backend->legacy || !backend->litWanted)
        return;
    if (inputs && backend->linearScene && !backend->linearFailed)
    {
        ++backend->litChanges;
        backend->litOverWhite = inputs->overWhite;
        if (!backend->litThisScene)
            ++backend->litScenes;
        backend->litThisScene = true;
    }
    lg::SetLinearInputs(backend->legacy, inputs ? inputs->constants : 0, inputs ? inputs->samplers : 0,
                        inputs ? inputs->doubled : 0, inputs && inputs->colorInput, inputs && inputs->overWhite);
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
                              error))
    {
        LogModernOnce(backend, "a copy of the linear-light scene is not tonemapped", error);
        return false;
    }
    ++backend->linearCopies;
    return true;
}

ModernResult BackendModernScene(Backend *backend, lg::Surface *color, lg::Surface *depth,
                                const ModernSceneView &view, lg::Surface *display, bool *glowDrawn)
{
    if (!backend || !backend->legacy || !backend->core)
        return ModernResult::Refused;
    // In linear light the scene must reach the back buffer whatever the
    // anti-aliasing does.
    const auto tonemap = [&](ModernResult passes) {
        if (!display)
            return passes;
        if (!lg::HandOverRecording(backend->legacy))
            return ModernResult::Skipped;
        const VkRect2D area = {{static_cast<int32_t>(view.x), static_cast<int32_t>(view.y)}, {view.width, view.height}};
        Image *scene = lg::SurfaceImage(color);
        float bloomScale = 0.0f;
        Image *bloom = DrawBloom(backend, scene, area, view, bloomScale);
        std::string error;
        if (!modern::WriteDisplay(backend->core, backend->tonemap, scene, area, lg::SurfaceImage(display), area,
                                  VK_FILTER_NEAREST, "modern tonemap", error, bloom, bloomScale))
        {
            LogModernOnce(backend, "the linear-light scene is not tonemapped", error);
            return ModernResult::Refused;
        }
        if (bloom && glowDrawn)
            *glowDrawn = true;
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
    modern::SceneView scene{};
    std::memcpy(scene.view, view.view, sizeof(scene.view));
    std::memcpy(scene.projection, view.projection, sizeof(scene.projection));
    scene.area = {{static_cast<int32_t>(view.x), static_cast<int32_t>(view.y)}, {view.width, view.height}};
    scene.cut = view.cut;
    std::string error;
    if (!modern::Resolve(backend->core, backend->taa, colorImage, depthImage, scene, error))
        return refuse(error);
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
