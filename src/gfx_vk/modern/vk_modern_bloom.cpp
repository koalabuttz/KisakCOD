// The Modern path's bloom: light above a threshold in the linear-light scene,
// spread over a chain of images that halve in size. See vk_modern.h and
// docs/vulkan-modern.md; the shaders are src/gfx_vk/modern/shaders/bloom_down.frag
// and bloom_up.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gfx_vk::modern
{
namespace
{
// bloom_down.frag's push constants (std430).
struct DownPush
{
    float from[4];
    float invSourceSize[2];
    float destSize[2];
    float threshold;
    float knee;
    float desaturation;
    float prefilter;
};
static_assert(sizeof(DownPush) == 48, "bloom_down.frag's push constant block is 48 bytes");

// bloom_up.frag's push constants (std430).
struct UpPush
{
    float invSourceSize[2];
    float destSize[2];
    float radius;
    float pad[3];
};
static_assert(sizeof(UpPush) == 32, "bloom_up.frag's push constant block is 32 bytes");

// The chain's format: the scene's. R11G11B10 would halve the memory traffic;
// the plan allows it once a test shows it harmless.
constexpr VkFormat kBloomFormat = kSceneFormat;
} // namespace

struct Bloom
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule down = VK_NULL_HANDLE;
    VkShaderModule up = VK_NULL_HANDLE;
    VkPipeline downPipeline = VK_NULL_HANDLE;
    VkPipeline upPipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    // The chain for the last area's size; levels[0] is half of it.
    Image *levels[kMaxBloomLevels] = {};
    uint32_t levelCount = 0;
    uint32_t width = 0, height = 0;
    BloomStats stats{};
};

namespace
{
void DestroyLevels(Core *core, Bloom &bloom)
{
    for (Image *&level : bloom.levels)
    {
        if (level)
            DestroyImage(core, level);
        level = nullptr;
    }
    bloom.levelCount = 0;
    bloom.width = bloom.height = 0;
}

uint32_t Half(uint32_t size) { return std::max(1u, (size + 1) / 2); }

// The chain matches the area's size and has at least `levels` images.
bool EnsureLevels(Core *core, Bloom &bloom, uint32_t width, uint32_t height, uint32_t levels, std::string &error)
{
    if (bloom.width == width && bloom.height == height && bloom.levelCount >= levels)
        return true;
    DestroyLevels(core, bloom);
    static const char *const kNames[kMaxBloomLevels] = {"modern bloom 0", "modern bloom 1", "modern bloom 2",
                                                         "modern bloom 3", "modern bloom 4", "modern bloom 5",
                                                         "modern bloom 6"};
    ImageDesc desc;
    desc.format = kBloomFormat;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    uint32_t w = width, h = height;
    // Made to the most levels the size allows, so a change of radius does not
    // make the chain again.
    const uint32_t most = BloomLevelsFor(1e9f, width, height);
    for (uint32_t i = 0; i < std::max(levels, most); ++i)
    {
        w = Half(w);
        h = Half(h);
        desc.width = w;
        desc.height = h;
        desc.name = kNames[i];
        bloom.levels[i] = CreateImage(core, desc);
        if (!bloom.levels[i])
        {
            error = LastError(core);
            DestroyLevels(core, bloom);
            return false;
        }
        bloom.levelCount = i + 1;
    }
    bloom.width = width;
    bloom.height = height;
    return true;
}

bool Pass(Core *core, Bloom &bloom, VkPipeline pipeline, Image *source, Image *target, VkAttachmentLoadOp load,
          const void *push, uint32_t pushSize, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, bloom.setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo image{bloom.sampler, ImageView(source), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 1, &write, 0, nullptr);

    UseImage(core, source, ImageUse::Sampled);
    const ImageDesc &targetInfo = ImageInfo(target);
    const VkRect2D area = {{0, 0}, {targetInfo.width, targetInfo.height}};
    PassDesc desc;
    desc.name = "modern bloom";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = load;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    uint8_t constants[64];
    std::memcpy(constants, push, pushSize);
    const VkPipelineLayout layout = bloom.layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height), 0.0f, 1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, constants);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);
    ++bloom.stats.passes;
    return true;
}
} // namespace

uint32_t BloomLevelsFor(float sigma, uint32_t width, uint32_t height)
{
    // Level n (from 0) is 2^(n+1) times smaller than the scene, and its tent
    // spreads light about 2^(n+1) scene pixels: two levels past the one
    // that matches sigma give the wide, soft tail bloom is for.
    const float wanted = std::log2(std::max(sigma, 1.0f)) + 3.0f;
    uint32_t levels = static_cast<uint32_t>(std::clamp(wanted, 3.0f, static_cast<float>(kMaxBloomLevels)));
    uint32_t fit = 0;
    for (uint32_t w = width, h = height; fit < kMaxBloomLevels; ++fit)
    {
        w = Half(w);
        h = Half(h);
        if (w < 4 || h < 4)
            break;
    }
    return std::max(1u, std::min(levels, fit));
}

Bloom *CreateBloom(Core *core, std::string &error)
{
    if (!FormatSupports(core, kBloomFormat,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
    {
        error = "the device cannot draw into, blend in and filter R16G16B16A16_SFLOAT";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Bloom *bloom = new Bloom();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyBloom(core, bloom);
        return nullptr;
    };

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &bloom->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DownPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bloom->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &bloom->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    bloom->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    bloom->down = CreateShaderModule(core, shaders::kBloomDownFrag, sizeof(shaders::kBloomDownFrag));
    bloom->up = CreateShaderModule(core, shaders::kBloomUpFrag, sizeof(shaders::kBloomUpFrag));
    if (!bloom->vertex || !bloom->down || !bloom->up)
        return fail(LastError(core));
    std::string reason;
    bloom->downPipeline =
        CreateFullscreenPipeline(core, bloom->layout, bloom->vertex, bloom->down, kBloomFormat, false, reason);
    if (!bloom->downPipeline)
        return fail(reason);
    bloom->upPipeline =
        CreateFullscreenPipeline(core, bloom->layout, bloom->vertex, bloom->up, kBloomFormat, true, reason);
    if (!bloom->upPipeline)
        return fail(reason);

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    bloom->sampler = GetSampler(core, sampler);
    if (!bloom->sampler)
        return fail(LastError(core));
    return bloom;
}

void DestroyBloom(Core *core, Bloom *bloom)
{
    if (!bloom)
        return;
    DestroyLevels(core, *bloom);
    DestroyPipeline(core, bloom->downPipeline);
    DestroyPipeline(core, bloom->upPipeline);
    DestroyShaderModule(core, bloom->vertex);
    DestroyShaderModule(core, bloom->down);
    DestroyShaderModule(core, bloom->up);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (bloom->layout)
        vk.vkDestroyPipelineLayout(device, bloom->layout, nullptr);
    if (bloom->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, bloom->setLayout, nullptr);
    delete bloom;
}

BloomStats Stats(const Bloom *bloom) { return bloom ? bloom->stats : BloomStats{}; }

Image *BuildBloom(Core *core, Bloom *bloom, Image *scene, const VkRect2D &area, const BloomParams &params,
                  std::string &error)
{
    if (!bloom || !scene)
    {
        error = "no bloom or scene image";
        return nullptr;
    }
    const ImageDesc &sceneInfo = ImageInfo(scene);
    if (sceneInfo.samples != 1 || !(sceneInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene is multisampled or cannot be sampled";
        return nullptr;
    }
    if (!Inside(area, sceneInfo))
    {
        error = "the area is not inside the scene image";
        return nullptr;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return nullptr;
    }
    const uint32_t levels = std::clamp(params.levels, 1u, BloomLevelsFor(1e9f, area.extent.width, area.extent.height));
    if (!EnsureLevels(core, *bloom, area.extent.width, area.extent.height, levels, error))
        return nullptr;

    // Down: the scene's area into level 0 (with the threshold), then each
    // level into the next.
    for (uint32_t i = 0; i < levels; ++i)
    {
        Image *source = i ? bloom->levels[i - 1] : scene;
        Image *target = bloom->levels[i];
        const ImageDesc &sourceInfo = ImageInfo(source);
        const ImageDesc &targetInfo = ImageInfo(target);
        DownPush push{};
        if (i)
        {
            push.from[2] = static_cast<float>(sourceInfo.width);
            push.from[3] = static_cast<float>(sourceInfo.height);
        }
        else
        {
            push.from[0] = static_cast<float>(area.offset.x);
            push.from[1] = static_cast<float>(area.offset.y);
            push.from[2] = static_cast<float>(area.extent.width);
            push.from[3] = static_cast<float>(area.extent.height);
        }
        push.invSourceSize[0] = 1.0f / sourceInfo.width;
        push.invSourceSize[1] = 1.0f / sourceInfo.height;
        push.destSize[0] = static_cast<float>(targetInfo.width);
        push.destSize[1] = static_cast<float>(targetInfo.height);
        push.threshold = std::max(params.threshold, 0.0f);
        push.knee = std::max(params.knee, 0.0f);
        push.desaturation = std::clamp(params.desaturation, 0.0f, 1.0f);
        push.prefilter = i ? 0.0f : 1.0f;
        if (!Pass(core, *bloom, bloom->downPipeline, source, target, VK_ATTACHMENT_LOAD_OP_DONT_CARE, &push,
                  sizeof(push), error))
            return nullptr;
    }
    // Up: each level, blurred, added to the one above it.
    for (uint32_t i = levels - 1; i > 0; --i)
    {
        Image *source = bloom->levels[i];
        Image *target = bloom->levels[i - 1];
        const ImageDesc &sourceInfo = ImageInfo(source);
        const ImageDesc &targetInfo = ImageInfo(target);
        UpPush push{};
        push.invSourceSize[0] = 1.0f / sourceInfo.width;
        push.invSourceSize[1] = 1.0f / sourceInfo.height;
        push.destSize[0] = static_cast<float>(targetInfo.width);
        push.destSize[1] = static_cast<float>(targetInfo.height);
        push.radius = 1.0f;
        if (!Pass(core, *bloom, bloom->upPipeline, source, target, VK_ATTACHMENT_LOAD_OP_LOAD, &push, sizeof(push),
                  error))
            return nullptr;
    }
    ++bloom->stats.builds;
    return bloom->levels[0];
}
} // namespace gfx_vk::modern
