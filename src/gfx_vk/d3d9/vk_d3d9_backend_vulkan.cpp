// The D3D9 front's GPU on Vulkan: a Vulkan core per device (vk_d3d9_backend.h).

#include "vk_d3d9_backend.h"

#include "vk_d3d9_internal.h"

#include <algorithm>

namespace gfx_vk::d3d9
{
struct Backend
{
    const Host *host = nullptr;
    WindowSystem windowSystem{};
    Core *core = nullptr;
    lg::Device *legacy = nullptr;
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
    return backend;
}

void CloseBackend(Backend *backend)
{
    if (!backend)
        return;
    const Host &h = *backend->host;
    if (backend->legacy && backend->core)
    {
        // What each frame cost, for comparing frame rates: the work recorded
        // per presented frame, and the GPU's time from a frame's first
        // command to its last (idle gaps between its submissions included).
        const lg::DeviceStats &stats = lg::Stats(backend->legacy);
        const CoreStats &core = Stats(backend->core);
        const double frames = core.framesPresented ? double(core.framesPresented) : 1.0;
        const double gpuAverage = core.frame.samples ? core.frame.totalMs / double(core.frame.samples) : 0.0;
        HostLog(h,
                "vulkan d3d9: frame stats: %llu frames; per frame %.0f draws, %.1f passes, %.0f descriptor "
                "sets (%.0f reused), %.1f KB constants, %.1f dynamic renames, %.2f submissions (%llu flushes in all, "
                "%llu of them ahead of Present at a swap fence, %llu to feed the GPU during a frame); "
                "GPU frame %.2f ms average, %.2f min, %.2f max over %llu frames",
                static_cast<unsigned long long>(core.framesPresented), double(stats.draws) / frames,
                double(stats.passes) / frames, double(stats.descriptorSets) / frames,
                double(stats.descriptorSetsReused) / frames, double(stats.constantBytes) / 1024.0 / frames,
                double(stats.dynamicRenames) / frames, double(core.submissions) / frames,
                static_cast<unsigned long long>(core.flushes), static_cast<unsigned long long>(stats.fenceFlushes),
                static_cast<unsigned long long>(stats.feedFlushes), gpuAverage,
                core.frame.samples ? core.frame.minMs : 0.0, core.frame.samples ? core.frame.maxMs : 0.0,
                static_cast<unsigned long long>(core.frame.samples));
        if (core.recordThread)
            HostLog(h,
                    "vulkan d3d9: record thread: per frame %.0f commands in %.1f blocks; render thread waited "
                    "%.3f ms per frame for it",
                    double(core.recordPackets) / frames, double(core.recordChunks) / frames,
                    core.recordWaitMs / frames);
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
}

void BackendPollProgress(Backend *backend)
{
    // The core's one call for any thread (vk_core.h). The core is created
    // before the device has objects and destroyed after the last has gone.
    if (backend && backend->core)
        RefreshProgress(backend->core);
}
} // namespace gfx_vk::d3d9
