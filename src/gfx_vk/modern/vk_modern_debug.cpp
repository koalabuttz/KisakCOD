// The Modern path's debug views: see vk_modern.h and docs/vulkan-modern.md.
// The shader is src/gfx_vk/modern/shaders/debug_velocity.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <utility>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// The shader's push constants (debug_velocity.frag, std430).
struct Push
{
    float halfSize[2];
};
static_assert(sizeof(Push) == 8, "debug_velocity.frag's push constant block is 8 bytes");
} // namespace

struct DebugView
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    // One pipeline per target format, made at its first use.
    std::vector<std::pair<VkFormat, VkPipeline>> pipelines;
};

DebugView *CreateDebugView(Core *core, std::string &error)
{
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    DebugView *debug = new DebugView();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyDebugView(core, debug);
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
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &debug->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &debug->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &debug->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    debug->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    debug->fragment = CreateShaderModule(core, shaders::kDebugVelocityFrag, sizeof(shaders::kDebugVelocityFrag));
    if (!debug->vertex || !debug->fragment)
        return fail(LastError(core));
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    debug->nearest = GetSampler(core, sampler);
    if (!debug->nearest)
        return fail(LastError(core));
    return debug;
}

void DestroyDebugView(Core *core, DebugView *debug)
{
    if (!debug)
        return;
    for (const auto &[format, pipeline] : debug->pipelines)
        DestroyPipeline(core, pipeline);
    DestroyShaderModule(core, debug->vertex);
    DestroyShaderModule(core, debug->fragment);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (debug->layout)
        vk.vkDestroyPipelineLayout(device, debug->layout, nullptr);
    if (debug->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, debug->setLayout, nullptr);
    delete debug;
}

bool DrawVelocityDebug(Core *core, DebugView *debug, Image *velocity, Image *target, const VkRect2D &area,
                       std::string &error)
{
    if (!debug || !velocity || !target || velocity == target)
    {
        error = "no velocity or target image";
        return false;
    }
    const ImageDesc &velocityInfo = ImageInfo(velocity);
    const ImageDesc &targetInfo = ImageInfo(target);
    if (!(targetInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || targetInfo.samples != 1)
    {
        error = "the target cannot be drawn into";
        return false;
    }
    if (!Inside(area, velocityInfo) || !Inside(area, targetInfo))
    {
        error = "the rectangle is not inside both images";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (const auto &[have, made] : debug->pipelines)
        if (have == targetInfo.format)
            pipeline = made;
    if (!pipeline)
    {
        pipeline = CreateFullscreenPipeline(core, debug->layout, debug->vertex, debug->fragment, targetInfo.format,
                                            false, error);
        if (!pipeline)
            return false;
        debug->pipelines.emplace_back(targetInfo.format, pipeline);
    }
    const VkDescriptorSet set = AllocateDescriptorSet(core, debug->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo image{debug->nearest, ImageSampleView(velocity), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 1, &write, 0, nullptr);

    Push push{};
    push.halfSize[0] = 0.5f * static_cast<float>(area.extent.width);
    push.halfSize[1] = 0.5f * static_cast<float>(area.extent.height);
    UseImage(core, velocity, ImageUse::Sampled);
    PassDesc desc;
    desc.name = "modern debug velocity";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = debug->layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{static_cast<float>(area.offset.x),
                                  static_cast<float>(area.offset.y),
                                  static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height),
                                  0.0f,
                                  1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);
    return true;
}
} // namespace gfx_vk::modern
