// gfx_vk pipelines: shader modules, graphics pipelines through an on-disk
// pipeline cache with a compile counter, and cached samplers. See vk_core.h.

#include "vk_core_internal.h"

#include <chrono>
#include <cstdio>

namespace gfx_vk
{
namespace
{
// MSVC rejects fopen as unsafe (C4996, an error under /WX).
FILE *OpenFile(const char *path, const char *mode)
{
#ifdef _MSC_VER
    FILE *file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

bool ReadFile(const char *path, std::vector<uint8_t> &out)
{
    FILE *file = OpenFile(path, "rb");
    if (!file)
        return false;
    out.clear();
    uint8_t buffer[65536];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.insert(out.end(), buffer, buffer + got);
    const bool ok = !std::ferror(file);
    std::fclose(file);
    return ok;
}

// The cache header (VkPipelineCacheHeaderVersionOne) must name this device,
// or the data is someone else's: start empty instead of handing it over.
bool MatchesDevice(const Core &core, const std::vector<uint8_t> &data)
{
    if (data.size() < 16 + VK_UUID_SIZE)
        return false;
    uint32_t words[4];
    std::memcpy(words, data.data(), sizeof(words));
    return words[0] >= 16 + VK_UUID_SIZE && words[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        && words[2] == core.properties.vendorID && words[3] == core.properties.deviceID
        && std::memcmp(data.data() + 16, core.properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}
} // namespace

bool CreatePipelineCache(Core &core)
{
    std::vector<uint8_t> data;
    const char *path = core.desc.pipelineCachePath;
    if (path && ReadFile(path, data))
    {
        if (MatchesDevice(core, data))
        {
            core.stats.pipelineCacheLoaded = true;
            Log(core, "pipeline cache: %zu bytes from %s", data.size(), path);
        }
        else
        {
            Log(core, "pipeline cache: %s is for another device or driver; starting empty", path);
            data.clear();
        }
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    core.pipelineCacheLoadedData = data;
    return Check(core, core.vk.vkCreatePipelineCache(core.dev, &info, nullptr, &core.pipelineCache),
                 "vkCreatePipelineCache");
}

void DestroyPipelineObjects(Core &core)
{
    for (auto &entry : core.samplers)
        core.vk.vkDestroySampler(core.dev, entry.second, nullptr);
    core.samplers.clear();
    if (core.pipelineCache)
        core.vk.vkDestroyPipelineCache(core.dev, core.pipelineCache, nullptr);
    core.pipelineCache = VK_NULL_HANDLE;
}

bool SavePipelineCache(Core *core)
{
    const char *path = core->desc.pipelineCachePath;
    if (!path || !core->pipelineCache)
        return true;
    size_t size = 0;
    if (core->vk.vkGetPipelineCacheData(core->dev, core->pipelineCache, &size, nullptr) != VK_SUCCESS || !size)
        return false;
    std::vector<uint8_t> data(size);
    if (core->vk.vkGetPipelineCacheData(core->dev, core->pipelineCache, &size, data.data()) != VK_SUCCESS)
        return false;
    data.resize(size);
    if (data == core->pipelineCacheLoadedData)
    {
        Log(*core, "pipeline cache: %s unchanged", path);
        return true;
    }
    // Write a temporary file and move it into place, so a crash never leaves a
    // half-written cache behind.
    const std::string temporary = std::string(path) + ".tmp";
    FILE *file = OpenFile(temporary.c_str(), "wb");
    if (!file)
    {
        Log(*core, "pipeline cache: cannot write %s", temporary.c_str());
        return false;
    }
    const bool written = std::fwrite(data.data(), 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        std::remove(temporary.c_str());
        return false;
    }
    std::remove(path); // rename does not replace an existing file on Windows
    if (std::rename(temporary.c_str(), path) != 0)
    {
        Log(*core, "pipeline cache: cannot move %s into place", temporary.c_str());
        return false;
    }
    Log(*core, "pipeline cache: %zu bytes to %s", size, path);
    return true;
}

VkShaderModule CreateShaderModule(Core *core, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (!Check(*core, core->vk.vkCreateShaderModule(core->dev, &info, nullptr, &module), "vkCreateShaderModule"))
        return VK_NULL_HANDLE;
    return module;
}

void DestroyShaderModule(Core *core, VkShaderModule module)
{
    // Modules are only read while pipelines are created, so no GPU work can
    // hold one; deferred anyway so the rule is the same for every object.
    if (module)
        core->deferred.push_back({DeferredKind::ShaderModule, PendingSerial(*core), nullptr, (uint64_t)module});
}

VkPipeline CreateGraphicsPipeline(Core *core, const VkGraphicsPipelineCreateInfo &info)
{
    VkPipelineCreationFeedback pipelineFeedback{};
    VkPipelineCreationFeedbackCreateInfo feedback{};
    feedback.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
    feedback.pNext = info.pNext;
    feedback.pPipelineCreationFeedback = &pipelineFeedback;
    VkGraphicsPipelineCreateInfo chained = info;
    chained.pNext = &feedback;

    const auto start = std::chrono::steady_clock::now();
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        core->vk.vkCreateGraphicsPipelines(core->dev, core->pipelineCache, 1, &chained, nullptr, &pipeline);
    core->stats.pipelineCreateMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!Check(*core, result, "vkCreateGraphicsPipelines"))
        return VK_NULL_HANDLE;
    ++core->stats.pipelinesCreated;
    if (core->inFrame)
        ++core->stats.pipelinesCreatedInFrame;
    if ((pipelineFeedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
        && (pipelineFeedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))
        ++core->stats.pipelineCacheHits;
    return pipeline;
}

void DestroyPipeline(Core *core, VkPipeline pipeline)
{
    if (pipeline)
        core->deferred.push_back({DeferredKind::Pipeline, PendingSerial(*core), nullptr, (uint64_t)pipeline});
}

VkSampler GetSampler(Core *core, const VkSamplerCreateInfo &info)
{
    SamplerKey key;
    std::memset(&key, 0, sizeof(key)); // padding takes part in the comparison
    key.magFilter = info.magFilter;
    key.minFilter = info.minFilter;
    key.mipmapMode = info.mipmapMode;
    key.u = info.addressModeU;
    key.v = info.addressModeV;
    key.w = info.addressModeW;
    key.mipLodBias = info.mipLodBias;
    key.anisotropyEnable = info.anisotropyEnable;
    key.maxAnisotropy = info.anisotropyEnable ? info.maxAnisotropy : 1.0f;
    key.compareEnable = info.compareEnable;
    key.compareOp = info.compareEnable ? info.compareOp : VK_COMPARE_OP_NEVER;
    key.minLod = info.minLod;
    key.maxLod = info.maxLod;
    key.borderColor = info.borderColor;
    auto found = core->samplers.find(key);
    if (found != core->samplers.end())
        return found->second;
    VkSamplerCreateInfo create = info;
    create.pNext = nullptr;
    VkSampler sampler = VK_NULL_HANDLE;
    if (!Check(*core, core->vk.vkCreateSampler(core->dev, &create, nullptr, &sampler), "vkCreateSampler"))
        return VK_NULL_HANDLE;
    core->samplers[key] = sampler;
    ++core->stats.samplersCreated;
    return sampler;
}
} // namespace gfx_vk
