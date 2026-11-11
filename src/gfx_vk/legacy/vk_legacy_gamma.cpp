// D3D9's gamma ramp on the way to the window (SetGammaRamp, Present).
//
// D3D9 applies the ramp of a fullscreen swapchain in the display hardware;
// the engine sets it from r_gamma whenever the device says it can
// (D3DCAPS2_FULLSCREENGAMMA). Here Present draws the shown image into the
// swapchain image with one full-screen pass that looks every channel up in
// the ramp, where it would otherwise blit. The window's desktop gamma is never
// touched, so nothing has to be put back when the game quits or crashes.

#include "vk_legacy_internal.h"
#include "vk_legacy_shaders.h"

namespace gfx_vk::legacy
{
namespace
{
constexpr VkFormat kRampFormat = VK_FORMAT_R16G16B16A16_UNORM;

struct Push
{
    float invShownSize[2]; // 1 / the swapchain image's size
    float scaled;          // 1: sample with the linear filter, 0: texel for texel
    float pad;
};

bool IsIdentity(const GammaRamp &ramp)
{
    for (uint32_t i = 0; i < 256; ++i)
    {
        const uint16_t keep = static_cast<uint16_t>(i * 257);
        if (ramp.red[i] != keep || ramp.green[i] != keep || ramp.blue[i] != keep)
            return false;
    }
    return true;
}

void DropPipeline(Device &device)
{
    Device::Gamma &gamma = device.gamma;
    if (gamma.pipeline)
        gfx_vk::DestroyPipeline(device.core, gamma.pipeline);
    gamma.pipeline = VK_NULL_HANDLE;
    gamma.format = VK_FORMAT_UNDEFINED;
}

bool CreateLayouts(Device &device)
{
    Device::Gamma &gamma = device.gamma;
    if (gamma.layout)
        return true;
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (device.vk->vkCreateDescriptorSetLayout(device.dev, &setInfo, nullptr, &gamma.setLayout) != VK_SUCCESS)
        return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &gamma.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (device.vk->vkCreatePipelineLayout(device.dev, &layoutInfo, nullptr, &gamma.layout) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    gamma.nearest = GetSampler(device.core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    gamma.linear = GetSampler(device.core, sampler);
    return gamma.nearest && gamma.linear;
}

bool CreatePipeline(Device &device, VkFormat format)
{
    Device::Gamma &gamma = device.gamma;
    if (gamma.pipeline && gamma.format == format)
        return true;
    DropPipeline(device);
    const VkShaderModule vertex =
        CreateShaderModule(device.core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    const VkShaderModule fragment = CreateShaderModule(device.core, shaders::kGammaFrag, sizeof(shaders::kGammaFrag));
    if (!vertex || !fragment)
    {
        DestroyShaderModule(device.core, vertex);
        DestroyShaderModule(device.core, fragment);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
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
    info.layout = gamma.layout;
    gamma.pipeline = CreateGraphicsPipeline(device.core, info);
    DestroyShaderModule(device.core, vertex);
    DestroyShaderModule(device.core, fragment);
    if (!gamma.pipeline)
        return false;
    gamma.format = format;
    return true;
}

bool UploadRamp(Device &device)
{
    Device::Gamma &gamma = device.gamma;
    if (!gamma.rampImage)
    {
        ImageDesc desc;
        desc.format = kRampFormat;
        desc.width = 256;
        desc.height = 1;
        desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = "gamma ramp";
        gamma.rampImage = CreateImage(device.core, desc);
        if (!gamma.rampImage)
            return false;
        gamma.uploaded = false;
    }
    if (gamma.uploaded)
        return true;
    ImageRegion region;
    region.width = 256;
    region.height = 1;
    uint32_t rowPitch = 0, slicePitch = 0;
    auto *texels = static_cast<uint16_t *>(StageImageWrite(device.core, gamma.rampImage, region, &rowPitch, &slicePitch));
    if (!texels)
        return false;
    for (uint32_t i = 0; i < 256; ++i)
    {
        texels[i * 4 + 0] = gamma.ramp.red[i];
        texels[i * 4 + 1] = gamma.ramp.green[i];
        texels[i * 4 + 2] = gamma.ramp.blue[i];
        texels[i * 4 + 3] = 0xffff;
    }
    gamma.uploaded = true;
    return true;
}

void GiveUp(Device &device, const char *why)
{
    device.gamma.failed = true;
    WarnOnce(device, WarnGamma, "the gamma ramp is not applied: %s", why);
}
} // namespace

void SetGammaRamp(Device *device_, const GammaRamp *ramp)
{
    Device &device = *device_;
    Device::Gamma &gamma = device.gamma;
    const bool on = ramp && !IsIdentity(*ramp);
    if (on && (!gamma.on || std::memcmp(&gamma.ramp, ramp, sizeof(*ramp)) != 0))
    {
        gamma.ramp = *ramp;
        gamma.uploaded = false;
    }
    if (on != gamma.on)
        Log(device, "gamma ramp %s", on ? "on: Present draws the window's image through it" : "off");
    gamma.on = on;
}

bool PresentThroughGamma(Device &device, Image *shown, Image *swapchain)
{
    Device::Gamma &gamma = device.gamma;
    if (!gamma.on || gamma.failed || !shown || !swapchain)
        return false;
    const ImageDesc &from = ImageInfo(shown);
    const ImageDesc &to = ImageInfo(swapchain);
    if (!FormatSupports(device.core, kRampFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    {
        GiveUp(device, "the device cannot sample R16G16B16A16_UNORM");
        return false;
    }
    if (!CreateLayouts(device) || !CreatePipeline(device, to.format) || !UploadRamp(device))
    {
        GiveUp(device, LastError(device.core)[0] ? LastError(device.core) : "its pass could not be made");
        return false;
    }
    const VkDescriptorSet set = AllocateDescriptorSet(device.core, gamma.setLayout);
    if (!set)
    {
        GiveUp(device, LastError(device.core));
        return false;
    }
    const bool scaled = from.width != to.width || from.height != to.height;
    const VkDescriptorImageInfo images[2] = {
        {scaled ? gamma.linear : gamma.nearest, ImageView(shown), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {gamma.nearest, ImageView(gamma.rampImage), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    device.vk->vkUpdateDescriptorSets(device.dev, 2, writes, 0, nullptr);

    UseImage(device.core, shown, ImageUse::Sampled);
    UseImage(device.core, gamma.rampImage, ImageUse::Sampled);
    PassDesc pass;
    pass.name = "gamma";
    pass.colorCount = 1;
    pass.color[0].image = swapchain;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (!BeginPass(device.core, pass))
    {
        GiveUp(device, LastError(device.core));
        return false;
    }
    Push push{};
    push.invShownSize[0] = 1.0f / static_cast<float>(to.width);
    push.invShownSize[1] = 1.0f / static_cast<float>(to.height);
    push.scaled = scaled ? 1.0f : 0.0f;
    const VkPipeline pipeline = gamma.pipeline;
    const VkPipelineLayout layout = gamma.layout;
    const VkRect2D area = {{0, 0}, {to.width, to.height}};
    Record(device.core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height), 0.0f, 1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(device.core);
    ++device.stats.gammaPresents;
    return true;
}

void DestroyGamma(Device &device)
{
    Device::Gamma &gamma = device.gamma;
    DropPipeline(device);
    DestroyImage(device.core, gamma.rampImage);
    gamma.rampImage = nullptr;
    if (gamma.layout)
        device.vk->vkDestroyPipelineLayout(device.dev, gamma.layout, nullptr);
    if (gamma.setLayout)
        device.vk->vkDestroyDescriptorSetLayout(device.dev, gamma.setLayout, nullptr);
    gamma.layout = VK_NULL_HANDLE;
    gamma.setLayout = VK_NULL_HANDLE;
}
} // namespace gfx_vk::legacy
