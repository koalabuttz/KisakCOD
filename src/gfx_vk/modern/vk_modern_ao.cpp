// The Modern path's ambient occlusion: see vk_modern.h and docs/vulkan-modern.md
// ("Ambient occlusion"). The shaders are src/gfx_vk/modern/shaders/gtao.frag
// and ao_blur.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// gtao.frag's push constants.
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

// ao_blur.frag's push constants.
struct BlurPush
{
    float clipToView[4][4];
    float area[4];
    float sky;
    float edge;
    float power;
    float lowest;
    float halfJitter;
};
static_assert(sizeof(BlurPush) <= sizeof(Push), "the blur's push constants fit the layout's range");

// contact.frag's push constants.
struct ContactPush
{
    float clipToView[4][4];
    float area[4];
    float project[4];
    float sun[4];
    float sky;
    float length;
    float thickness;
    float strength;
};
static_assert(sizeof(ContactPush) <= sizeof(Push), "the contact shadows' push constants fit the layout's range");

constexpr VkFormat kRawFormat = VK_FORMAT_R16_SFLOAT;
} // namespace

struct AmbientOcclusion
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule gtao = VK_NULL_HANDLE;
    VkShaderModule blur = VK_NULL_HANDLE;
    VkShaderModule contact = VK_NULL_HANDLE;
    VkPipeline gtaoPipeline = VK_NULL_HANDLE;
    // The blur's and the contact shadows' (green only), one per target
    // format, made at their first use.
    std::vector<std::pair<VkFormat, VkPipeline>> blurPipelines;
    std::vector<std::pair<VkFormat, VkPipeline>> contactPipelines;
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
    ao->contact = CreateShaderModule(core, shaders::kContactFrag, sizeof(shaders::kContactFrag));
    if (!ao->vertex || !ao->gtao || !ao->blur || !ao->contact)
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
    for (const auto &[format, pipeline] : ao->contactPipelines)
        DestroyPipeline(core, pipeline);
    DestroyImage(core, ao->raw);
    DestroyShaderModule(core, ao->vertex);
    DestroyShaderModule(core, ao->gtao);
    DestroyShaderModule(core, ao->blur);
    DestroyShaderModule(core, ao->contact);
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
// `first` and `second` at bindings 0 and 1; with `clear` the area is cleared
// to white first.
bool Pass(Core *core, AmbientOcclusion *ao, VkPipeline pipeline, const char *name, Image *first, Image *second,
          Image *target, const VkRect2D &area, const void *push, uint32_t pushSize, std::string &error,
          bool clear = false)
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
    desc.color[0].load = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    for (float &value : desc.color[0].clear)
        value = 1.0f;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = ao->layout;
    Push words{};
    std::memcpy(&words, push, pushSize);
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{static_cast<float>(area.offset.x),
                                  static_cast<float>(area.offset.y),
                                  static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height),
                                  0.0f,
                                  1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, &words);
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
    // At half resolution: 1 + which pixel of each 2x2 block is worked out
    // (gtao.frag, ao_blur.frag), moving over the four from frame to frame.
    const float halfJitter = params.half ? static_cast<float>(1 + (params.frame & 3u)) : 0.0f;
    push.up[3] = halfJitter;
    // The raw result's size and area: the scene's, or a pixel for each 2x2
    // block of it.
    const uint32_t rawWidth = params.half ? (depthInfo.width + 1) / 2 : depthInfo.width;
    const uint32_t rawHeight = params.half ? (depthInfo.height + 1) / 2 : depthInfo.height;
    VkRect2D rawArea = view.area;
    if (params.half)
    {
        const int32_t x0 = view.area.offset.x / 2, y0 = view.area.offset.y / 2;
        const int32_t x1 = (view.area.offset.x + static_cast<int32_t>(view.area.extent.width) - 1) / 2;
        const int32_t y1 = (view.area.offset.y + static_cast<int32_t>(view.area.extent.height) - 1) / 2;
        rawArea.offset = {x0, y0};
        rawArea.extent = {static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1)};
    }

    if (ao->raw)
    {
        const ImageDesc &have = ImageInfo(ao->raw);
        if (have.width != rawWidth || have.height != rawHeight)
        {
            DestroyImage(core, ao->raw);
            ao->raw = nullptr;
        }
    }
    if (!ao->raw)
    {
        ImageDesc desc;
        desc.format = kRawFormat;
        desc.width = rawWidth;
        desc.height = rawHeight;
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
    BlurPush blurPush{};
    std::memcpy(blurPush.clipToView, push.clipToView, sizeof(blurPush.clipToView));
    std::memcpy(blurPush.area, push.area, sizeof(blurPush.area));
    blurPush.sky = push.sky;
    blurPush.edge = params.edge;
    blurPush.power = params.power;
    blurPush.lowest = params.floor;
    blurPush.halfJitter = halfJitter;
    if (!Pass(core, ao, ao->gtaoPipeline, "modern ao trace", depth, normals, ao->raw, rawArea, &push, sizeof(push), error))
        return false;
    if (!Pass(core, ao, blur, "modern ao", depth, ao->raw, target, view.area, &blurPush, sizeof(blurPush), error))
        return false;
    UseImage(core, target, ImageUse::Sampled);
    ++ao->stats.builds;
    return true;
}

bool TraceContactShadows(Core *core, AmbientOcclusion *ao, Image *depth, Image *normals, const SceneView &view,
                         const ContactParams &params, Image *target, bool clear, std::string &error)
{
    if (!ao || !depth || !normals || !target)
    {
        error = "no depth, normals or target image";
        return false;
    }
    const ImageDesc &depthInfo = ImageInfo(depth);
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
    if (!Inside(view.area, depthInfo) || !Inside(view.area, ImageInfo(normals)) || !Inside(view.area, targetInfo))
    {
        error = "the viewport is not inside the depth, normals and target images";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    ContactPush push{};
    if (!ClipToView(view, push.clipToView))
    {
        error = "the scene's projection cannot be inverted";
        return false;
    }
    // A point's NDC is (x * p00 + z * p20, y * p11 + z * p21) / (z * p23)
    // (row vectors; the jitter is in p20 and p21).
    const float w = view.projection[2][3];
    if (std::fabs(w) < 1e-6f || view.projection[0][0] == 0.0f || view.projection[1][1] == 0.0f)
    {
        error = "the scene's projection is not a perspective one";
        return false;
    }
    push.project[0] = view.projection[0][0] / w;
    push.project[1] = view.projection[1][1] / w;
    push.project[2] = view.projection[2][0] / w;
    push.project[3] = view.projection[2][1] / w;
    push.area[0] = static_cast<float>(view.area.offset.x);
    push.area[1] = static_cast<float>(view.area.offset.y);
    push.area[2] = static_cast<float>(view.area.extent.width);
    push.area[3] = static_cast<float>(view.area.extent.height);
    // The sun's direction in view space: d_view = d_world * view (row
    // vectors, the rotation alone).
    float sun[3];
    for (int j = 0; j < 3; ++j)
        sun[j] = params.sun[0] * view.view[0][j] + params.sun[1] * view.view[1][j] + params.sun[2] * view.view[2][j];
    const float norm = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (norm < 1e-6f)
    {
        error = "no sun direction";
        return false;
    }
    for (int j = 0; j < 3; ++j)
        push.sun[j] = sun[j] / norm;
    push.sun[3] = static_cast<float>(params.frame & 63u);
    push.sky = SkyDepth(view);
    push.length = params.length;
    push.thickness = params.thickness;
    push.strength = params.strength;
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (const auto &[have, made] : ao->contactPipelines)
        if (have == targetInfo.format)
            pipeline = made;
    if (!pipeline)
    {
        pipeline = CreateFullscreenPipeline(core, ao->layout, ao->vertex, ao->contact, targetInfo.format, false, error,
                                            VK_COLOR_COMPONENT_G_BIT);
        if (!pipeline)
            return false;
        ao->contactPipelines.emplace_back(targetInfo.format, pipeline);
    }
    if (!Pass(core, ao, pipeline, "modern contact shadows", depth, normals, target, view.area, &push, sizeof(push),
              error, clear))
        return false;
    UseImage(core, target, ImageUse::Sampled);
    ++ao->stats.contacts;
    return true;
}

AoStats Stats(const AmbientOcclusion *ao) { return ao ? ao->stats : AoStats{}; }
} // namespace gfx_vk::modern
