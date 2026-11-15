// The Modern path's god rays: see vk_modern.h and docs/vulkan-modern.md ("God
// rays"). The shader is src/gfx_vk/modern/shaders/rays.frag, built three times
// (RAYS_PASS 0 the mask, 1 the smear, 2 the apply).

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// rays.frag's push constants (std430).
struct Push
{
    float area[4];
    float sun[4];
    float sunView[4];
    float lens[4];
    float shape[4];
    float glow[4];
    float target[4];
    float misc[4];
};
static_assert(sizeof(Push) == 128, "rays.frag's push constant block is 128 bytes");

constexpr VkFormat kRaysFormat = kSceneFormat;
constexpr uint32_t kBindings = 3;

float Decode(float display)
{
    return display <= 0.04045f ? display / 12.92f : std::pow((display + 0.055f) / 1.055f, 2.4f);
}

float Smoothstep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

struct Rays
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule mask = VK_NULL_HANDLE;
    VkShaderModule blur = VK_NULL_HANDLE;
    VkShaderModule apply = VK_NULL_HANDLE;
    VkPipeline maskPipeline = VK_NULL_HANDLE;
    VkPipeline blurPipeline = VK_NULL_HANDLE;
    // The apply pass's, one per target format, made at its first use.
    std::vector<std::pair<VkFormat, VkPipeline>> applyPipelines;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE; // clamped to the edge
    VkSampler border = VK_NULL_HANDLE; // black outside: no sky known off the screen
    Image *masked = nullptr;
    Image *smeared = nullptr;
    RaysStats stats{};
};

Rays *CreateRays(Core *core, std::string &error)
{
    if (!FormatSupports(core, kRaysFormat,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
    {
        error = "the device cannot draw into and filter R16G16B16A16_SFLOAT";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Rays *rays = new Rays();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyRays(core, rays);
        return nullptr;
    };
    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = kBindings;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &rays->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rays->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &rays->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    rays->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    rays->mask = CreateShaderModule(core, shaders::kRaysMaskFrag, sizeof(shaders::kRaysMaskFrag));
    rays->blur = CreateShaderModule(core, shaders::kRaysBlurFrag, sizeof(shaders::kRaysBlurFrag));
    rays->apply = CreateShaderModule(core, shaders::kRaysApplyFrag, sizeof(shaders::kRaysApplyFrag));
    if (!rays->vertex || !rays->mask || !rays->blur || !rays->apply)
        return fail(LastError(core));
    std::string reason;
    rays->maskPipeline = CreateFullscreenPipeline(core, rays->layout, rays->vertex, rays->mask, kRaysFormat, false,
                                                  reason);
    if (!rays->maskPipeline)
        return fail(reason);
    rays->blurPipeline = CreateFullscreenPipeline(core, rays->layout, rays->vertex, rays->blur, kRaysFormat, false,
                                                  reason);
    if (!rays->blurPipeline)
        return fail(reason);

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    rays->nearest = GetSampler(core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    rays->linear = GetSampler(core, sampler);
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    rays->border = GetSampler(core, sampler);
    if (!rays->nearest || !rays->linear || !rays->border)
        return fail(LastError(core));
    return rays;
}

void DestroyRays(Core *core, Rays *rays)
{
    if (!rays)
        return;
    DestroyPipeline(core, rays->maskPipeline);
    DestroyPipeline(core, rays->blurPipeline);
    for (const auto &made : rays->applyPipelines)
        DestroyPipeline(core, made.second);
    DestroyImage(core, rays->masked);
    DestroyImage(core, rays->smeared);
    DestroyShaderModule(core, rays->vertex);
    DestroyShaderModule(core, rays->mask);
    DestroyShaderModule(core, rays->blur);
    DestroyShaderModule(core, rays->apply);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (rays->layout)
        vk.vkDestroyPipelineLayout(device, rays->layout, nullptr);
    if (rays->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, rays->setLayout, nullptr);
    delete rays;
}

namespace
{
// One full-screen pass of `pipeline` over `area` of `target`, its three
// bindings `images` sampled through `samplers` (binding 1 is the depth).
bool Pass(Core *core, Rays &rays, VkPipeline pipeline, Image *const images[kBindings],
          const VkSampler samplers[kBindings], Image *target, const VkRect2D &area, VkAttachmentLoadOp load,
          const Push &push, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, rays.setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    VkDescriptorImageInfo infos[kBindings];
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        infos[i] = {samplers[i], i == 1 ? ImageSampleView(images[i]) : ImageView(images[i]),
                    VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), kBindings, writes, 0, nullptr);
    for (uint32_t i = 0; i < kBindings; ++i)
        UseImage(core, images[i], ImageUse::Sampled);
    PassDesc desc;
    desc.name = "modern god rays";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = load;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = rays.layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y),
                                  static_cast<float>(area.extent.width), static_cast<float>(area.extent.height),
                                  0.0f, 1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);
    return true;
}
} // namespace

bool DrawRays(Core *core, Rays *rays, Image *color, Image *depth, const SceneView &view, const RaysInputs &inputs,
              const RaysParams &params, bool &drawn, std::string &error)
{
    drawn = false;
    if (!rays || !color || !depth)
    {
        error = "no colour or depth image";
        return false;
    }
    const ImageDesc &colorInfo = ImageInfo(color);
    const ImageDesc &depthInfo = ImageInfo(depth);
    if (colorInfo.samples != 1 || !(colorInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        || !(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        error = "the scene is multisampled, or cannot be sampled or drawn into";
        return false;
    }
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return false;
    }
    if (!Inside(view.area, colorInfo) || !Inside(view.area, depthInfo))
    {
        error = "the viewport is not inside the colour and depth images";
        return false;
    }
    if (!InFrame(core) || InPass(core) || InComputePass(core))
    {
        error = "not between passes of a frame";
        return false;
    }

    // Towards the sun in view space (row vectors, w 0: v = d * view).
    float sunView[3] = {};
    {
        const float x = inputs.sunDirection[0], y = inputs.sunDirection[1], z = inputs.sunDirection[2];
        for (int c = 0; c < 3; ++c)
            sunView[c] = x * view.view[0][c] + y * view.view[1][c] + z * view.view[2][c];
        const float norm = std::sqrt(sunView[0] * sunView[0] + sunView[1] * sunView[1] + sunView[2] * sunView[2]);
        if (norm < 1e-6f)
        {
            ++rays->stats.behind;
            return true;
        }
        for (float &c : sunView)
            c /= norm;
    }
    // Shafts only while the sun is in front: full within about 70 degrees of
    // the view's middle, none from 90.
    const float fade = Smoothstep(0.0f, 0.35f, sunView[2]);
    const float strength = std::max(params.strength, 0.0f);
    const float p00 = view.projection[0][0], p11 = view.projection[1][1];
    const float p20 = view.projection[2][0], p21 = view.projection[2][1], p23 = view.projection[2][3];
    if (fade <= 0.0f || strength <= 0.0f || std::fabs(p00) < 1e-6f || std::fabs(p11) < 1e-6f)
    {
        ++rays->stats.behind;
        return true;
    }
    // A projection without w from z (orthographic) is taken as one with.
    const float w = p23 > 0.0f ? p23 : 1.0f;

    Push push{};
    push.area[0] = static_cast<float>(view.area.offset.x);
    push.area[1] = static_cast<float>(view.area.offset.y);
    push.area[2] = static_cast<float>(view.area.extent.width);
    push.area[3] = static_cast<float>(view.area.extent.height);
    // The sun's place: ndc = (v.xy * p + v.z * p2) / (v.z * p23).
    const float ndcX = (sunView[0] * p00 + sunView[2] * p20) / (sunView[2] * w);
    const float ndcY = (sunView[1] * p11 + sunView[2] * p21) / (sunView[2] * w);
    push.sun[0] = ndcX * 0.5f + 0.5f;
    push.sun[1] = 0.5f - ndcY * 0.5f;
    push.sun[2] = fade;
    push.sun[3] = static_cast<float>(params.frame & 0xffffu);
    push.sunView[0] = sunView[0];
    push.sunView[1] = sunView[1];
    push.sunView[2] = sunView[2];
    push.sunView[3] = SkyDepth(view);
    push.lens[0] = p00;
    push.lens[1] = p11;
    push.lens[2] = p20;
    push.lens[3] = p21;
    push.shape[0] = std::max(params.spread, 0.0f);
    push.shape[1] = static_cast<float>(std::clamp(params.samples, 1u, 256u));
    push.shape[2] = std::clamp(params.length, 0.0f, 1.0f);
    push.shape[3] = std::clamp(params.decay, 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c)
        push.glow[c] = std::max(params.glow, 0.0f) * Decode(std::max(inputs.sunColor[c], 0.0f));
    push.glow[3] = strength;
    push.misc[0] = w;
    push.misc[1] = std::max(params.most, 0.0f);
    push.misc[2] = std::max(params.threshold, 0.0f);
    push.misc[3] = std::clamp(params.sky, 0.0f, 1.0f);

    // The half-resolution images, a pixel for each 2x2 block of the area.
    const uint32_t halfWidth = (view.area.extent.width + 1) / 2;
    const uint32_t halfHeight = (view.area.extent.height + 1) / 2;
    const auto ready = [&](Image *&image, const char *name) {
        if (image)
        {
            const ImageDesc &have = ImageInfo(image);
            if (have.width == halfWidth && have.height == halfHeight)
                return true;
            DestroyImage(core, image);
            image = nullptr;
        }
        ImageDesc desc;
        desc.format = kRaysFormat;
        desc.width = halfWidth;
        desc.height = halfHeight;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = name;
        image = CreateImage(core, desc);
        if (!image)
            error = LastError(core);
        return image != nullptr;
    };
    if (!ready(rays->masked, "modern god rays mask") || !ready(rays->smeared, "modern god rays"))
        return false;
    push.target[0] = static_cast<float>(halfWidth);
    push.target[1] = static_cast<float>(halfHeight);
    push.target[2] = 1.0f / static_cast<float>(halfWidth);
    push.target[3] = 1.0f / static_cast<float>(halfHeight);

    VkPipeline apply = VK_NULL_HANDLE;
    for (const auto &made : rays->applyPipelines)
        if (made.first == colorInfo.format)
            apply = made.second;
    if (!apply)
    {
        apply = CreateFullscreenPipeline(core, rays->layout, rays->vertex, rays->apply, colorInfo.format, true, error);
        if (!apply)
            return false;
        rays->applyPipelines.emplace_back(colorInfo.format, apply);
    }

    const VkRect2D half = {{0, 0}, {halfWidth, halfHeight}};
    // The images a pass does not read stand in so that its set is whole; the
    // scene's colour is never bound while it is drawn into.
    {
        Image *const images[kBindings] = {color, depth, rays->smeared};
        const VkSampler samplers[kBindings] = {rays->linear, rays->nearest, rays->linear};
        if (!Pass(core, *rays, rays->maskPipeline, images, samplers, rays->masked, half, VK_ATTACHMENT_LOAD_OP_DONT_CARE, push, error))
            return false;
    }
    {
        Image *const images[kBindings] = {color, depth, rays->masked};
        const VkSampler samplers[kBindings] = {rays->linear, rays->nearest, rays->border};
        if (!Pass(core, *rays, rays->blurPipeline, images, samplers, rays->smeared, half, VK_ATTACHMENT_LOAD_OP_DONT_CARE, push, error))
            return false;
    }
    {
        Image *const images[kBindings] = {rays->masked, depth, rays->smeared};
        const VkSampler samplers[kBindings] = {rays->linear, rays->nearest, rays->linear};
        if (!Pass(core, *rays, apply, images, samplers, color, view.area, VK_ATTACHMENT_LOAD_OP_LOAD, push, error))
            return false;
    }
    ++rays->stats.scenes;
    drawn = true;
    return true;
}

RaysStats Stats(const Rays *rays) { return rays ? rays->stats : RaysStats{}; }
} // namespace gfx_vk::modern
