// Compute passes of the Modern path: the helper every compute pass shares
// (vk_modern_internal.h) and the compute check (vk_modern.h,
// KISAK_VK_MODERN_DEBUG=compute; docs/vulkan-modern.md "Compute passes").
// The check's shaders are src/gfx_vk/modern/shaders/compute_check_fill.comp,
// compute_check_integrate.comp and compute_check_draw.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <iterator>
#include <utility>
#include <vector>

namespace gfx_vk::modern
{
VkPipeline CreateComputePipeline(Core *core, VkPipelineLayout layout, VkShaderModule module, std::string &error,
                                 const VkSpecializationInfo *specialization)
{
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.stage.pSpecializationInfo = specialization;
    info.layout = layout;
    const VkPipeline pipeline = gfx_vk::CreateComputePipeline(core, info);
    if (!pipeline)
        error = LastError(core);
    return pipeline;
}

namespace
{
// The fill shader's work group (compute_check_fill.comp) and the integrate
// shader's (compute_check_integrate.comp, one invocation per column).
constexpr uint32_t kFillGroup = 4;
constexpr uint32_t kIntegrateGroup = 8;

// The draw shader's push constants (compute_check_draw.frag, std430).
struct DrawPush
{
    float offset[2];
    float size[2];
    float depth;
};
static_assert(sizeof(DrawPush) == 20, "compute_check_draw.frag's push constant block is 20 bytes");

// What the descriptor set layouts hold, by binding.
VkDescriptorSetLayout CreateSetLayout(Core *core, const VkDescriptorType *types, uint32_t count,
                                      VkShaderStageFlags stages)
{
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < count; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = count;
    info.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (Functions(core).vkCreateDescriptorSetLayout(DeviceHandle(core), &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

VkPipelineLayout CreateLayout(Core *core, VkDescriptorSetLayout set, const VkPushConstantRange *push)
{
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = push ? 1 : 0;
    info.pPushConstantRanges = push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (Functions(core).vkCreatePipelineLayout(DeviceHandle(core), &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                const VkDescriptorImageInfo *image)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = image;
    return write;
}

VkWriteDescriptorSet BufferWrite(VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo *buffer)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = buffer;
    return write;
}
} // namespace

struct ComputeCheck
{
    VkDescriptorSetLayout fillSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout integrateSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout drawSet = VK_NULL_HANDLE;
    VkPipelineLayout fillLayout = VK_NULL_HANDLE;
    VkPipelineLayout integrateLayout = VK_NULL_HANDLE;
    VkPipelineLayout drawLayout = VK_NULL_HANDLE;
    VkShaderModule fill = VK_NULL_HANDLE;
    VkShaderModule integrate = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule draw = VK_NULL_HANDLE;
    VkPipeline fillPipeline = VK_NULL_HANDLE;
    VkPipeline integratePipeline = VK_NULL_HANDLE;
    // The draw's pipeline per target format, made at its first use.
    std::vector<std::pair<VkFormat, VkPipeline>> drawPipelines;
    VkSampler linear = VK_NULL_HANDLE;
    // Pass one's volume and slice weights, pass two's volume (what is drawn).
    Image *filled = nullptr;
    Image *integrated = nullptr;
    Buffer *slices = nullptr;
    bool built = false; // integrated holds a volume
    ComputeCheckStats stats{};
};

ComputeCheck *CreateComputeCheck(Core *core, std::string &error)
{
    ComputeCheck *check = new ComputeCheck();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyComputeCheck(core, check);
        return nullptr;
    };
    const VkDescriptorType fillTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType integrateTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType drawTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    check->fillSet = CreateSetLayout(core, fillTypes, 2, VK_SHADER_STAGE_COMPUTE_BIT);
    check->integrateSet = CreateSetLayout(core, integrateTypes, 3, VK_SHADER_STAGE_COMPUTE_BIT);
    check->drawSet = CreateSetLayout(core, drawTypes, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!check->fillSet || !check->integrateSet || !check->drawSet)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush)};
    check->fillLayout = CreateLayout(core, check->fillSet, nullptr);
    check->integrateLayout = CreateLayout(core, check->integrateSet, nullptr);
    check->drawLayout = CreateLayout(core, check->drawSet, &push);
    if (!check->fillLayout || !check->integrateLayout || !check->drawLayout)
        return fail("vkCreatePipelineLayout failed");
    check->fill = CreateShaderModule(core, shaders::kComputeCheckFillComp, sizeof(shaders::kComputeCheckFillComp));
    check->integrate =
        CreateShaderModule(core, shaders::kComputeCheckIntegrateComp, sizeof(shaders::kComputeCheckIntegrateComp));
    check->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    check->draw = CreateShaderModule(core, shaders::kComputeCheckDrawFrag, sizeof(shaders::kComputeCheckDrawFrag));
    if (!check->fill || !check->integrate || !check->vertex || !check->draw)
        return fail(LastError(core));
    check->fillPipeline = CreateComputePipeline(core, check->fillLayout, check->fill, error);
    if (!check->fillPipeline)
        return fail(error);
    check->integratePipeline = CreateComputePipeline(core, check->integrateLayout, check->integrate, error);
    if (!check->integratePipeline)
        return fail(error);
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check->linear = GetSampler(core, sampler);
    if (!check->linear)
        return fail(LastError(core));

    ImageDesc volume;
    volume.kind = ImageKind::Tex3D;
    volume.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    volume.width = kComputeCheckSize[0];
    volume.height = kComputeCheckSize[1];
    volume.depth = kComputeCheckSize[2];
    volume.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    volume.name = "modern compute check filled";
    check->filled = CreateImage(core, volume);
    volume.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    volume.name = "modern compute check integrated";
    check->integrated = CreateImage(core, volume);
    BufferDesc slices;
    slices.size = sizeof(float) * 4 * kComputeCheckSize[2];
    slices.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    slices.name = "modern compute check slices";
    check->slices = CreateBuffer(core, slices);
    if (!check->filled || !check->integrated || !check->slices)
        return fail(LastError(core));
    return check;
}

void DestroyComputeCheck(Core *core, ComputeCheck *check)
{
    if (!check)
        return;
    DestroyPipeline(core, check->fillPipeline);
    DestroyPipeline(core, check->integratePipeline);
    for (const auto &[format, pipeline] : check->drawPipelines)
        DestroyPipeline(core, pipeline);
    DestroyShaderModule(core, check->fill);
    DestroyShaderModule(core, check->integrate);
    DestroyShaderModule(core, check->vertex);
    DestroyShaderModule(core, check->draw);
    if (check->filled)
        DestroyImage(core, check->filled);
    if (check->integrated)
        DestroyImage(core, check->integrated);
    if (check->slices)
        DestroyBuffer(core, check->slices);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    for (VkPipelineLayout layout : {check->fillLayout, check->integrateLayout, check->drawLayout})
        if (layout)
            vk.vkDestroyPipelineLayout(device, layout, nullptr);
    for (VkDescriptorSetLayout layout : {check->fillSet, check->integrateSet, check->drawSet})
        if (layout)
            vk.vkDestroyDescriptorSetLayout(device, layout, nullptr);
    delete check;
}

bool BuildComputeCheck(Core *core, ComputeCheck *check, std::string &error)
{
    if (!check)
    {
        error = "no compute check";
        return false;
    }
    if (!InFrame(core) || InPass(core) || InComputePass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    const VkDescriptorSet fillSet = AllocateDescriptorSet(core, check->fillSet);
    const VkDescriptorSet integrateSet = AllocateDescriptorSet(core, check->integrateSet);
    if (!fillSet || !integrateSet)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo filled{VK_NULL_HANDLE, ImageView(check->filled), VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo integrated{VK_NULL_HANDLE, ImageView(check->integrated), VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo slices{BufferHandle(check->slices), 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[] = {
        ImageWrite(fillSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &filled),
        BufferWrite(fillSet, 1, &slices),
        ImageWrite(integrateSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &filled),
        ImageWrite(integrateSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &integrated),
        BufferWrite(integrateSet, 2, &slices),
    };
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), static_cast<uint32_t>(std::size(writes)), writes, 0,
                                           nullptr);

    // Pass one writes the whole volume and the weights.
    if (!BeginComputePass(core, "modern compute check fill"))
    {
        error = LastError(core);
        return false;
    }
    UseImage(core, check->filled, ImageUse::StorageWrite);
    UseBuffer(core, check->slices, BufferUse::ShaderWrite);
    const VkPipeline fillPipeline = check->fillPipeline;
    const VkPipelineLayout fillLayout = check->fillLayout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fillPipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fillLayout, 0, 1, &fillSet, 0, nullptr);
    });
    DispatchCompute(core, GroupCount(kComputeCheckSize[0], kFillGroup), GroupCount(kComputeCheckSize[1], kFillGroup),
                    GroupCount(kComputeCheckSize[2], kFillGroup));
    EndComputePass(core);

    // Pass two reads both (the barriers wait for pass one's writes) and
    // writes the volume that is drawn.
    if (!BeginComputePass(core, "modern compute check integrate"))
    {
        error = LastError(core);
        return false;
    }
    UseImage(core, check->filled, ImageUse::StorageRead);
    UseImage(core, check->integrated, ImageUse::StorageWrite);
    UseBuffer(core, check->slices, BufferUse::ShaderRead);
    const VkPipeline integratePipeline = check->integratePipeline;
    const VkPipelineLayout integrateLayout = check->integrateLayout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integratePipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, integrateLayout, 0, 1, &integrateSet, 0,
                                   nullptr);
    });
    DispatchCompute(core, GroupCount(kComputeCheckSize[0], kIntegrateGroup),
                    GroupCount(kComputeCheckSize[1], kIntegrateGroup), 1);
    EndComputePass(core);
    check->built = true;
    ++check->stats.builds;
    return true;
}

bool DrawComputeCheck(Core *core, ComputeCheck *check, Image *target, const VkRect2D &area, float depth,
                      std::string &error)
{
    if (!check || !check->built || !target)
    {
        error = check ? "no volume built" : "no compute check";
        return false;
    }
    const ImageDesc &targetInfo = ImageInfo(target);
    if (!(targetInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || targetInfo.samples != 1)
    {
        error = "the target cannot be drawn into";
        return false;
    }
    if (!Inside(area, targetInfo))
    {
        error = "the rectangle is not inside the target";
        return false;
    }
    if (!InFrame(core) || InPass(core) || InComputePass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (const auto &[format, made] : check->drawPipelines)
        if (format == targetInfo.format)
            pipeline = made;
    if (!pipeline)
    {
        pipeline = CreateFullscreenPipeline(core, check->drawLayout, check->vertex, check->draw, targetInfo.format,
                                            false, error);
        if (!pipeline)
            return false;
        check->drawPipelines.emplace_back(targetInfo.format, pipeline);
    }
    const VkDescriptorSet set = AllocateDescriptorSet(core, check->drawSet);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo volume{check->linear, ImageSampleView(check->integrated),
                                       VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &volume);
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 1, &write, 0, nullptr);

    DrawPush push{};
    push.offset[0] = static_cast<float>(area.offset.x);
    push.offset[1] = static_cast<float>(area.offset.y);
    push.size[0] = static_cast<float>(area.extent.width);
    push.size[1] = static_cast<float>(area.extent.height);
    push.depth = depth;
    // Compute shader writes to fragment shader reads.
    UseImage(core, check->integrated, ImageUse::Sampled);
    PassDesc desc;
    desc.name = "modern compute check draw";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = check->drawLayout;
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
    ++check->stats.draws;
    return true;
}

ComputeCheckStats Stats(const ComputeCheck *check) { return check ? check->stats : ComputeCheckStats{}; }
} // namespace gfx_vk::modern
