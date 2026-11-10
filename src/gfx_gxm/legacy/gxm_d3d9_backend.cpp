// The D3D9 front's GPU on the PS Vita (src/gfx_vk/d3d9/vk_d3d9_backend.h):
// the GXM Legacy back end, which owns the one screen.

#if !defined(KISAK_GFX_GXM)
#error "gxm_d3d9_backend.cpp is the PS Vita's D3D9 front back end (KISAK_GFX_GXM)"
#endif

#include <gfx_vk/d3d9/vk_d3d9_backend.h>

#include <gfx_gxm/gxm_display.h>
#include <gfx_gxm/gxm_memory.h>

#include <cstdio>

namespace gfx_vk::d3d9
{
struct Backend
{
    const Host *host = nullptr;
    lg::Device *legacy = nullptr;
};

Backend *OpenBackend(const BackendDesc &desc, std::string &error)
{
    if (gfx_gxm::DisplayReady())
    {
        error = "the Vita's screen already has a device";
        return nullptr;
    }
    const Host &h = *desc.host;
    lg::DeviceDesc legacyDesc;
    legacyDesc.width = desc.width;
    legacyDesc.height = desc.height;
    legacyDesc.log = h.log;
    legacyDesc.logUser = h.user;
    legacyDesc.shaderCachePath = h.shaderCachePath;
    lg::Device *legacy = lg::CreateDevice(nullptr, legacyDesc, error);
    if (!legacy)
        return nullptr;
    Backend *backend = new Backend;
    backend->host = desc.host;
    backend->legacy = legacy;
    return backend;
}

void CloseBackend(Backend *backend)
{
    if (!backend)
        return;
    if (backend->legacy)
    {
        const lg::DeviceStats &stats = lg::Stats(backend->legacy);
        const gfx_gxm::DisplayStats display = gfx_gxm::GetDisplayStats();
        if (backend->host->log)
        {
            char line[256];
            std::snprintf(line, sizeof(line), "gxm d3d9: %llu frames, %llu draws, %llu clears",
                          static_cast<unsigned long long>(display.frames),
                          static_cast<unsigned long long>(stats.draws),
                          static_cast<unsigned long long>(stats.clears));
            backend->host->log(backend->host->user, line);
        }
        lg::DestroyDevice(backend->legacy);
    }
    delete backend;
}

lg::Device *LegacyDevice(Backend *backend)
{
    return backend ? backend->legacy : nullptr;
}

const char *BackendDeviceName(const Backend *)
{
    return "PS Vita SGX543MP4+ (sceGxm)";
}

void DetachBackendWindow(Backend *backend)
{
    if (backend && backend->legacy)
        lg::DetachWindow(backend->legacy);
}

uint64_t BackendTextureMemory(const Backend *)
{
    // What is left in the CDRAM pool, where textures go first.
    const gfx_gxm::PoolStats cdram = gfx_gxm::Stats(gfx_gxm::MemoryKind::Cdram);
    return cdram.size - cdram.used;
}

bool BackendWindowSize(const Backend *, uint32_t &width, uint32_t &height)
{
    width = gfx_gxm::kDisplayWidth;
    height = gfx_gxm::kDisplayHeight;
    return true;
}

void BackendCounters(const Backend *, DeviceCounters &out)
{
    out.validationErrors = 0;
    out.validation = false;
    out.submissions = 0;
    out.fenceFlushes = 0;
    out.feedFlushes = 0;
    out.feedIdleFlushes = 0;
    out.occlusionCounted = false;
    out.occlusionCounts = 0;
    out.occlusionPools = 0;
    out.occlusionResults = 0;
    out.occlusionResultsZero = 0;
    out.occlusionSamples = 0;
}

// sceGxm's queries need no system call under the front's lock.
void BackendPollProgress(Backend *) {}

// No frame statistics are kept here, and the host has no settings to take up.
void BackendLogStats(Backend *, bool) {}
void BackendFrame(Backend *) {}
} // namespace gfx_vk::d3d9
