// The D3D9 front's GPU on Vulkan: a Vulkan core per device (vk_d3d9_backend.h).

#include "vk_d3d9_backend.h"

#include "vk_d3d9_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

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
    uint64_t framesTracked = 0;
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
}

void LogSlowFrames(const Backend &backend)
{
    const Host &h = *backend.host;
    for (uint32_t i = 0; i < kSlowFrames && backend.slowFrames[i].ms > 0.0; ++i)
    {
        const SlowFrame &frame = backend.slowFrames[i];
        char gpu[160];
        if (frame.gpuKnown)
            std::snprintf(gpu, sizeof(gpu),
                          "GPU %.2f ms: %.2f in passes, %.2f between them, %.2f without work before it",
                          frame.gpu.frameMs, frame.gpu.passesMs,
                          std::max(0.0, frame.gpu.frameMs - frame.gpu.passesMs), frame.gpu.gapMs);
        else
            std::snprintf(gpu, sizeof(gpu), "GPU time not read");
        HostLog(h,
                "vulkan d3d9: slow frame %u: %.2f ms, frame %llu at %.2f s; %llu draws, %llu passes, %llu submissions, "
                "%llu dynamic renames, %u swapchains built; of the time: %u pipelines (%u from the cache) %.2f ms, "
                "%u shaders %.2f ms, %llu texture uploads (%.2f MB) %.2f ms, %llu memory blocks %.2f ms; waits: "
                "swapchain image %.2f ms, frames in flight %.2f ms, GPU otherwise %.2f ms (%llu textures read back), "
                "record thread %.2f ms; %s",
                i + 1, frame.ms, static_cast<unsigned long long>(frame.number), frame.seconds,
                static_cast<unsigned long long>(frame.draws), static_cast<unsigned long long>(frame.passes),
                static_cast<unsigned long long>(frame.submissions),
                static_cast<unsigned long long>(frame.dynamicRenames), frame.swapchainBuilds, frame.pipelines,
                frame.pipelineCacheHits, frame.pipelineMs, frame.shaders, frame.shaderMs,
                static_cast<unsigned long long>(frame.uploads), frame.uploadMegabytes, frame.uploadMs,
                static_cast<unsigned long long>(frame.memoryAllocations), frame.memoryAllocateMs, frame.imageWaitMs,
                frame.frameLimitWaitMs, frame.otherGpuWaitMs, static_cast<unsigned long long>(frame.readbacks),
                frame.recordWaitMs, gpu);
    }
}
} // namespace

Backend *OpenBackend(const BackendDesc &desc, std::string &error)
{
    const Host &h = *desc.host;
    Backend *backend = new Backend;
    backend->host = desc.host;
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
    if (reset)
    {
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
}

void BackendPollProgress(Backend *backend)
{
    // The core's one call for any thread (vk_core.h). The core is created
    // before the device has objects and destroyed after the last has gone.
    if (backend && backend->core)
        RefreshProgress(backend->core);
}
} // namespace gfx_vk::d3d9
