// The Modern path's volumetric fog: see vk_modern.h and docs/vulkan-modern.md
// ("Volumetric fog"). The shaders are src/gfx_vk/modern/shaders/fog_march.frag
// and fog_apply.frag.

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
// The shaders' uniform block (std140): two matrices, then vec4s.
struct Params
{
    float clipToWorld[4][4];
    float shadowLookup[4][4];
    float camera[4];
    float sun[4];
    float fog[4];
    float farPartition[4];
    float misc[4];
    float area[4];
    float shape[4];
    float color[4];
};
static_assert(sizeof(Params) == 256, "the fog's uniform block is two matrices and eight vectors");

constexpr VkFormat kMarchFormat = VK_FORMAT_R16G16_SFLOAT;

bool Invert(const double m[4][4], double out[4][4])
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c)
            a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col]))
                pivot = r;
        if (std::fabs(a[pivot][col]) < 1e-30)
            return false;
        if (pivot != col)
            for (int c = 0; c < 8; ++c)
                std::swap(a[pivot][c], a[col][c]);
        const double scale = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c)
            a[col][c] *= scale;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col)
                continue;
            const double factor = a[r][col];
            for (int c = 0; c < 8; ++c)
                a[r][c] -= factor * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = a[r][c + 4];
    return true;
}

// Clip space (NDC x, y, depth, 1) to homogeneous world space, row vectors:
// the inverse of view * projection, worked in double since world positions
// run to the hundreds of thousands.
bool ClipToWorld(const SceneView &view, float out[4][4])
{
    double viewProjection[4][4], inverse[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += double(view.view[r][k]) * double(view.projection[k][c]);
            viewProjection[r][c] = sum;
        }
    if (!Invert(viewProjection, inverse))
        return false;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = static_cast<float>(inverse[r][c]);
    return true;
}

float Decode(float display)
{
    return display <= 0.04045f ? display / 12.92f : std::pow((display + 0.055f) / 1.055f, 2.4f);
}
} // namespace

struct Fog
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule march = VK_NULL_HANDLE;
    VkShaderModule apply = VK_NULL_HANDLE;
    VkPipeline marchPipeline = VK_NULL_HANDLE;
    // The apply pass's, one per target format and blend (added, or the
    // debug view written), made at their first use.
    struct ApplyPipeline
    {
        VkFormat format;
        bool additive;
        VkPipeline pipeline;
    };
    std::vector<ApplyPipeline> applyPipelines;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler compare = VK_NULL_HANDLE;
    Image *marched = nullptr;
    FogStats stats;
};

Fog *CreateFog(Core *core, std::string &error)
{
    if (!FormatSupports(core, kMarchFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    {
        error = "the device cannot draw into R16G16_SFLOAT";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Fog *fog = new Fog();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyFog(core, fog);
        return nullptr;
    };
    // 0 scene depth, 1 the sun's shadow map (compared), 2 the march's
    // result, 3 the parameters.
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 4;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &fog->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &fog->setLayout;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &fog->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    fog->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    fog->march = CreateShaderModule(core, shaders::kFogMarchFrag, sizeof(shaders::kFogMarchFrag));
    fog->apply = CreateShaderModule(core, shaders::kFogApplyFrag, sizeof(shaders::kFogApplyFrag));
    if (!fog->vertex || !fog->march || !fog->apply)
        return fail(LastError(core));
    std::string pipelineError;
    fog->marchPipeline =
        CreateFullscreenPipeline(core, fog->layout, fog->vertex, fog->march, kMarchFormat, false, pipelineError);
    if (!fog->marchPipeline)
        return fail(pipelineError);
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    fog->nearest = GetSampler(core, sampler);
    // The shadow map compares as the lit shaders' sampler does: lit where the
    // reference is at most the stored depth. Outside the map counts as lit.
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    fog->compare = GetSampler(core, sampler);
    if (!fog->nearest || !fog->compare)
        return fail(LastError(core));
    return fog;
}

void DestroyFog(Core *core, Fog *fog)
{
    if (!fog)
        return;
    DestroyPipeline(core, fog->marchPipeline);
    for (const Fog::ApplyPipeline &made : fog->applyPipelines)
        DestroyPipeline(core, made.pipeline);
    DestroyImage(core, fog->marched);
    DestroyShaderModule(core, fog->vertex);
    DestroyShaderModule(core, fog->march);
    DestroyShaderModule(core, fog->apply);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (fog->layout)
        vk.vkDestroyPipelineLayout(device, fog->layout, nullptr);
    if (fog->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, fog->setLayout, nullptr);
    delete fog;
}

namespace
{
// One full-screen pass of `pipeline` over `area` into `target`.
bool Pass(Core *core, Fog *fog, VkPipeline pipeline, Image *depth, Image *shadow, Image *marched,
          const Transient &params, Image *target, const VkRect2D &area, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, fog->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo images[3] = {
        {fog->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {fog->compare, ImageSampleView(shadow), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {fog->nearest, ImageSampleView(marched), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
    };
    const VkDescriptorBufferInfo buffer{params.buffer, params.offset, sizeof(Params)};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == 3)
        {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[i].pBufferInfo = &buffer;
        }
        else
        {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
    }
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 4, writes, 0, nullptr);
    UseImage(core, depth, ImageUse::Sampled);
    UseImage(core, shadow, ImageUse::Sampled);
    UseImage(core, marched, ImageUse::Sampled);
    PassDesc desc;
    desc.name = "modern fog";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    const VkPipelineLayout layout = fog->layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{static_cast<float>(area.offset.x),
                                  static_cast<float>(area.offset.y),
                                  static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height),
                                  0.0f,
                                  1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);
    return true;
}
} // namespace

bool DrawFog(Core *core, Fog *fog, Image *color, Image *depth, Image *shadowMap, const SceneView &view,
             const FogInputs &inputs, const FogParams &params, std::string &error)
{
    if (!fog || !color || !depth)
    {
        error = "no colour or depth image";
        return false;
    }
    const ImageDesc &depthInfo = ImageInfo(depth);
    const ImageDesc &colorInfo = ImageInfo(color);
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return false;
    }
    if (!(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || colorInfo.samples != 1)
    {
        error = "the scene cannot be drawn into";
        return false;
    }
    if (shadowMap)
    {
        const ImageDesc &shadowInfo = ImageInfo(shadowMap);
        if (shadowInfo.samples != 1 || !(shadowInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            shadowMap = nullptr; // marched as all lit
    }
    if (!Inside(view.area, depthInfo) || !Inside(view.area, colorInfo))
    {
        error = "the viewport is not inside the colour and depth images";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }

    Params block{};
    if (!ClipToWorld(view, block.clipToWorld))
    {
        error = "the scene's view and projection cannot be inverted";
        return false;
    }
    std::memcpy(block.shadowLookup, inputs.shadowLookup, sizeof(block.shadowLookup));
    block.camera[0] = inputs.origin[0];
    block.camera[1] = inputs.origin[1];
    block.camera[2] = inputs.origin[2];
    block.camera[3] = shadowMap ? 1.0f : 0.0f;
    {
        const float x = inputs.sunDirection[0], y = inputs.sunDirection[1], z = inputs.sunDirection[2];
        const float norm = std::sqrt(x * x + y * y + z * z);
        if (norm > 1e-6f)
        {
            block.sun[0] = x / norm;
            block.sun[1] = y / norm;
            block.sun[2] = z / norm;
        }
    }
    block.sun[3] = std::fmin(std::fmax(params.scatter, 0.0f), 0.95f);
    block.fog[0] = inputs.density;
    block.fog[1] = inputs.start;
    block.fog[2] = params.march;
    block.fog[3] = static_cast<float>(params.steps ? params.steps : 1u);
    std::memcpy(block.farPartition, inputs.switchPartition, sizeof(block.farPartition));
    block.misc[0] = inputs.shadowmapScale[0];
    block.misc[1] = inputs.shadowmapScale[1];
    block.misc[2] = SkyDepth(view);
    block.misc[3] = static_cast<float>(params.frame & 0xffffu);
    block.area[0] = static_cast<float>(view.area.offset.x);
    block.area[1] = static_cast<float>(view.area.offset.y);
    block.area[2] = static_cast<float>(view.area.extent.width);
    block.area[3] = static_cast<float>(view.area.extent.height);
    block.shape[2] = params.half ? 1.0f : 0.0f;
    block.shape[3] = std::fmin(std::fmax(params.sunShare, 0.0f), 1.0f);
    for (int i = 0; i < 3; ++i)
        block.color[i] = Decode(std::fmin(std::fmax(inputs.color[i], 0.0f), 1.0f));
    block.color[3] = params.debug ? 1.0f : 0.0f;

    // The march's image and area: the scene's, or a pixel for each 2x2
    // block of it.
    const uint32_t marchWidth = params.half ? (depthInfo.width + 1) / 2 : depthInfo.width;
    const uint32_t marchHeight = params.half ? (depthInfo.height + 1) / 2 : depthInfo.height;
    VkRect2D marchArea = view.area;
    if (params.half)
    {
        const int32_t x0 = view.area.offset.x / 2, y0 = view.area.offset.y / 2;
        const int32_t x1 = (view.area.offset.x + static_cast<int32_t>(view.area.extent.width) - 1) / 2;
        const int32_t y1 = (view.area.offset.y + static_cast<int32_t>(view.area.extent.height) - 1) / 2;
        marchArea.offset = {x0, y0};
        marchArea.extent = {static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1)};
    }
    if (fog->marched)
    {
        const ImageDesc &have = ImageInfo(fog->marched);
        if (have.width != marchWidth || have.height != marchHeight)
        {
            DestroyImage(core, fog->marched);
            fog->marched = nullptr;
        }
    }
    if (!fog->marched)
    {
        ImageDesc desc;
        desc.format = kMarchFormat;
        desc.width = marchWidth;
        desc.height = marchHeight;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = "modern fog march";
        fog->marched = CreateImage(core, desc);
        if (!fog->marched)
        {
            error = LastError(core);
            return false;
        }
    }
    const bool additive = !params.debug;
    VkPipeline apply = VK_NULL_HANDLE;
    for (const Fog::ApplyPipeline &made : fog->applyPipelines)
        if (made.format == colorInfo.format && made.additive == additive)
            apply = made.pipeline;
    if (!apply)
    {
        apply = CreateFullscreenPipeline(core, fog->layout, fog->vertex, fog->apply, colorInfo.format, additive, error);
        if (!apply)
            return false;
        fog->applyPipelines.push_back({colorInfo.format, additive, apply});
    }

    const VkDeviceSize alignment = Limits(core).minUniformBufferOffsetAlignment;
    const Transient uniforms = AllocateTransient(core, sizeof(Params), alignment ? alignment : 16);
    if (!uniforms.data)
    {
        error = LastError(core);
        return false;
    }
    std::memcpy(uniforms.data, &block, sizeof(block));
    // Without a shadow map the march reads nothing from binding 1; the scene
    // depth stands in so that the set is whole. The march's own image stands
    // in for binding 2 while it is being drawn: the depth.
    Image *shadow = shadowMap ? shadowMap : depth;
    if (!Pass(core, fog, fog->marchPipeline, depth, shadow, depth, uniforms, fog->marched, marchArea, error))
        return false;
    if (!Pass(core, fog, apply, depth, shadow, fog->marched, uniforms, color, view.area, error))
        return false;
    ++fog->stats.scenes;
    if (shadowMap)
        ++fog->stats.shadowed;
    return true;
}

FogStats Stats(const Fog *fog) { return fog ? fog->stats : FogStats{}; }
} // namespace gfx_vk::modern
