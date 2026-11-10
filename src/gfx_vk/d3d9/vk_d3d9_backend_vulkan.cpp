// The D3D9 front's GPU on Vulkan: a Vulkan core per device (vk_d3d9_backend.h).

#include "vk_d3d9_backend.h"

#include "vk_d3d9_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace gfx_vk::d3d9
{
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
};

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
    if (!backend || !backend->legacy || !backend->host->tuning)
        return;
    Tuning tuning;
    backend->host->tuning(backend->host->user, tuning);
    Tuning &old = backend->tuning;
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
