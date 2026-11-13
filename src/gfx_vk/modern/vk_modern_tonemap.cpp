// The Modern path's tonemap: the linear-light scene back to display values.
// See vk_modern.h and docs/vulkan-modern.md; the shader is
// src/gfx_vk/modern/shaders/tonemap.frag.

#include "vk_modern.h"

#include "vk_modern_shaders.h"

#include <utility>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// The shader's push constants (tonemap.frag, std430).
struct Push
{
    float from[4];
    float to[4];
    float invSceneSize[2];
};
static_assert(sizeof(Push) == 40, "tonemap.frag's push constant block is 40 bytes");
} // namespace

struct Tonemap
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    // One pipeline per target format, made at its first use.
    std::vector<std::pair<VkFormat, VkPipeline>> pipelines;
    TonemapStats stats{};
};

namespace
{
VkPipeline PipelineFor(Core *core, Tonemap &tonemap, VkFormat format, std::string &error)
{
    for (const auto &[have, pipeline] : tonemap.pipelines)
        if (have == format)
            return pipeline;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = tonemap.vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = tonemap.fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{};
    depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthState;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = tonemap.layout;
    const VkPipeline pipeline = CreateGraphicsPipeline(core, info);
    if (!pipeline)
    {
        error = LastError(core);
        return VK_NULL_HANDLE;
    }
    tonemap.pipelines.emplace_back(format, pipeline);
    return pipeline;
}

bool Inside(const VkRect2D &rect, const ImageDesc &image)
{
    return rect.extent.width && rect.extent.height && rect.offset.x >= 0 && rect.offset.y >= 0
        && rect.offset.x + rect.extent.width <= image.width && rect.offset.y + rect.extent.height <= image.height;
}
} // namespace

Tonemap *CreateTonemap(Core *core, std::string &error)
{
    if (!FormatSupports(core, kSceneFormat,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
    {
        error = "the device cannot draw into, blend in and filter R16G16B16A16_SFLOAT";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Tonemap *tonemap = new Tonemap();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyTonemap(core, tonemap);
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
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &tonemap->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &tonemap->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &tonemap->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    tonemap->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    tonemap->fragment = CreateShaderModule(core, shaders::kTonemapFrag, sizeof(shaders::kTonemapFrag));
    if (!tonemap->vertex || !tonemap->fragment)
        return fail(LastError(core));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    tonemap->nearest = GetSampler(core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    tonemap->linear = GetSampler(core, sampler);
    if (!tonemap->nearest || !tonemap->linear)
        return fail(LastError(core));
    return tonemap;
}

void DestroyTonemap(Core *core, Tonemap *tonemap)
{
    if (!tonemap)
        return;
    for (const auto &[format, pipeline] : tonemap->pipelines)
        DestroyPipeline(core, pipeline);
    DestroyShaderModule(core, tonemap->vertex);
    DestroyShaderModule(core, tonemap->fragment);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (tonemap->layout)
        vk.vkDestroyPipelineLayout(device, tonemap->layout, nullptr);
    if (tonemap->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, tonemap->setLayout, nullptr);
    delete tonemap;
}

TonemapStats Stats(const Tonemap *tonemap) { return tonemap ? tonemap->stats : TonemapStats{}; }

bool WriteDisplay(Core *core, Tonemap *tonemap, Image *scene, const VkRect2D &from, Image *target,
                  const VkRect2D &to, VkFilter filter, const char *pass, std::string &error)
{
    if (!tonemap || !scene || !target)
    {
        error = "no scene or target image";
        return false;
    }
    if (scene == target)
    {
        error = "the scene and the target are one image";
        return false;
    }
    const ImageDesc &sceneInfo = ImageInfo(scene);
    const ImageDesc &targetInfo = ImageInfo(target);
    if (sceneInfo.samples != 1 || targetInfo.samples != 1)
    {
        error = "the scene or the target is multisampled";
        return false;
    }
    if (!(sceneInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(targetInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        error = "the scene cannot be sampled or the target drawn into";
        return false;
    }
    if (!Inside(from, sceneInfo) || !Inside(to, targetInfo))
    {
        error = "a rectangle is not inside its image";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    const VkPipeline pipeline = PipelineFor(core, *tonemap, targetInfo.format, error);
    if (!pipeline)
        return false;

    Push push{};
    push.from[0] = static_cast<float>(from.offset.x);
    push.from[1] = static_cast<float>(from.offset.y);
    push.from[2] = static_cast<float>(from.extent.width);
    push.from[3] = static_cast<float>(from.extent.height);
    push.to[0] = static_cast<float>(to.offset.x);
    push.to[1] = static_cast<float>(to.offset.y);
    push.to[2] = static_cast<float>(to.extent.width);
    push.to[3] = static_cast<float>(to.extent.height);
    push.invSceneSize[0] = 1.0f / sceneInfo.width;
    push.invSceneSize[1] = 1.0f / sceneInfo.height;

    const VkDescriptorSet set = AllocateDescriptorSet(core, tonemap->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo image{filter == VK_FILTER_LINEAR ? tonemap->linear : tonemap->nearest,
                                      ImageView(scene), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 1, &write, 0, nullptr);

    UseImage(core, scene, ImageUse::Sampled);
    PassDesc desc;
    desc.name = pass;
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    desc.area = to;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = tonemap->layout;
    const VkRect2D area = to;
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
    ++tonemap->stats.passes;
    return true;
}
} // namespace gfx_vk::modern
