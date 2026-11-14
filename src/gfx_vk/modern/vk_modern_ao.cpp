// The Modern path's ambient occlusion: see vk_modern.h and docs/vulkan-modern.md
// ("Ambient occlusion"). The shaders are src/gfx_vk/modern/shaders/gtao.frag
// and ao_blur.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <cmath>
#include <utility>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// The push constants both shaders share (gtao.frag; ao_blur.frag reads the
// members up to the power, and up[3], the floor).
struct Push
{
    float clipToView[4][4];
    float area[4];
    float sky;
    float radius;
    float pixelsPerUnit;
    float falloff;
    float power;
    uint32_t frame;
    float thickness;
    float grazing;
    float up[4];
};
static_assert(sizeof(Push) == 128, "gtao.frag's push constant block fits 128 bytes, the least a device offers");

constexpr VkFormat kRawFormat = VK_FORMAT_R16_SFLOAT;
} // namespace

struct AmbientOcclusion
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule gtao = VK_NULL_HANDLE;
    VkShaderModule blur = VK_NULL_HANDLE;
    VkPipeline gtaoPipeline = VK_NULL_HANDLE;
    // The blur's, one per target format, made at its first use.
    std::vector<std::pair<VkFormat, VkPipeline>> blurPipelines;
    VkSampler nearest = VK_NULL_HANDLE;
    Image *raw = nullptr;
    AoStats stats;
};

AmbientOcclusion *CreateAmbientOcclusion(Core *core, std::string &error)
{
    if (!FormatSupports(core, kRawFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    {
        error = "the device cannot draw into R16_SFLOAT";
        return nullptr;
    }
    if (Limits(core).maxPushConstantsSize < sizeof(Push))
    {
        error = "the device's push constants are smaller than 112 bytes";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    AmbientOcclusion *ao = new AmbientOcclusion();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyAmbientOcclusion(core, ao);
        return nullptr;
    };
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
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &ao->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &ao->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &ao->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    ao->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    ao->gtao = CreateShaderModule(core, shaders::kGtaoFrag, sizeof(shaders::kGtaoFrag));
    ao->blur = CreateShaderModule(core, shaders::kAoBlurFrag, sizeof(shaders::kAoBlurFrag));
    if (!ao->vertex || !ao->gtao || !ao->blur)
        return fail(LastError(core));
    std::string pipelineError;
    ao->gtaoPipeline = CreateFullscreenPipeline(core, ao->layout, ao->vertex, ao->gtao, kRawFormat, false,
                                                pipelineError);
    if (!ao->gtaoPipeline)
        return fail(pipelineError);
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ao->nearest = GetSampler(core, sampler);
    if (!ao->nearest)
        return fail(LastError(core));
    return ao;
}

void DestroyAmbientOcclusion(Core *core, AmbientOcclusion *ao)
{
    if (!ao)
        return;
    DestroyPipeline(core, ao->gtaoPipeline);
    for (const auto &[format, pipeline] : ao->blurPipelines)
        DestroyPipeline(core, pipeline);
    DestroyImage(core, ao->raw);
    DestroyShaderModule(core, ao->vertex);
    DestroyShaderModule(core, ao->gtao);
    DestroyShaderModule(core, ao->blur);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (ao->layout)
        vk.vkDestroyPipelineLayout(device, ao->layout, nullptr);
    if (ao->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, ao->setLayout, nullptr);
    delete ao;
}

namespace
{
// One full-screen pass of `pipeline` over view.area into `target`, reading
// `first` and `second` at bindings 0 and 1.
bool Pass(Core *core, AmbientOcclusion *ao, VkPipeline pipeline, const char *name, Image *first, Image *second,
          Image *target, const VkRect2D &area, const Push &push, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, ao->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo images[2] = {
        {ao->nearest, ImageSampleView(first), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {ao->nearest, ImageSampleView(second), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
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
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 2, writes, 0, nullptr);
    UseImage(core, first, ImageUse::Sampled);
    UseImage(core, second, ImageUse::Sampled);
    PassDesc desc;
    desc.name = name;
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = ao->layout;
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
} // namespace

bool BuildAmbientOcclusion(Core *core, AmbientOcclusion *ao, Image *depth, Image *normals, const SceneView &view,
                           const AoParams &params, Image *target, std::string &error)
{
    if (!ao || !depth || !normals || !target)
    {
        error = "no depth, normals or target image";
        return false;
    }
    const ImageDesc &depthInfo = ImageInfo(depth);
    const ImageDesc &normalsInfo = ImageInfo(normals);
    const ImageDesc &targetInfo = ImageInfo(target);
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return false;
    }
    if (!(targetInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || targetInfo.samples != 1)
    {
        error = "the target cannot be drawn into";
        return false;
    }
    if (!Inside(view.area, depthInfo) || !Inside(view.area, normalsInfo) || !Inside(view.area, targetInfo))
    {
        error = "the viewport is not inside the depth, normals and target images";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    Push push{};
    if (!ClipToView(view, push.clipToView))
    {
        error = "the scene's projection cannot be inverted";
        return false;
    }
    push.area[0] = static_cast<float>(view.area.offset.x);
    push.area[1] = static_cast<float>(view.area.offset.y);
    push.area[2] = static_cast<float>(view.area.extent.width);
    push.area[3] = static_cast<float>(view.area.extent.height);
    push.sky = SkyDepth(view);
    push.radius = params.radius;
    // x_ndc = projection[0][0] * x / z, and NDC 1 is half the area's width.
    push.pixelsPerUnit = view.projection[0][0] * 0.5f * static_cast<float>(view.area.extent.width);
    push.falloff = params.falloff;
    push.power = params.power;
    push.frame = params.frame;
    push.thickness = params.thickness;
    push.grazing = params.grazing;
    push.up[3] = params.floor;
    // World up (the engine's +z) in view space: p_view = p_world * view.
    {
        const float x = view.view[2][0], y = view.view[2][1], z = view.view[2][2];
        const float norm = std::sqrt(x * x + y * y + z * z);
        if (norm > 1e-6f)
        {
            push.up[0] = x / norm;
            push.up[1] = y / norm;
            push.up[2] = z / norm;
        }
    }

    if (ao->raw)
    {
        const ImageDesc &have = ImageInfo(ao->raw);
        if (have.width != depthInfo.width || have.height != depthInfo.height)
        {
            DestroyImage(core, ao->raw);
            ao->raw = nullptr;
        }
    }
    if (!ao->raw)
    {
        ImageDesc desc;
        desc.format = kRawFormat;
        desc.width = depthInfo.width;
        desc.height = depthInfo.height;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = "modern ao raw";
        ao->raw = CreateImage(core, desc);
        if (!ao->raw)
        {
            error = LastError(core);
            return false;
        }
    }
    VkPipeline blur = VK_NULL_HANDLE;
    for (const auto &[have, made] : ao->blurPipelines)
        if (have == targetInfo.format)
            blur = made;
    if (!blur)
    {
        blur = CreateFullscreenPipeline(core, ao->layout, ao->vertex, ao->blur, targetInfo.format, false, error);
        if (!blur)
            return false;
        ao->blurPipelines.emplace_back(targetInfo.format, blur);
    }
    if (!Pass(core, ao, ao->gtaoPipeline, "modern ao", depth, normals, ao->raw, view.area, push, error))
        return false;
    if (!Pass(core, ao, blur, "modern ao", depth, ao->raw, target, view.area, push, error))
        return false;
    UseImage(core, target, ImageUse::Sampled);
    ++ao->stats.builds;
    return true;
}

AoStats Stats(const AmbientOcclusion *ao) { return ao ? ao->stats : AoStats{}; }
} // namespace gfx_vk::modern
