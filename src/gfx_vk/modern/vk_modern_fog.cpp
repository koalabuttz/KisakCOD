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
// A spot light as the march reads it (std140).
struct SpotBlock
{
    float shadowLookup[4][4]; // world to its tile, projective
    float origin[4];          // xyz world units; w 1 / radius
    float dir[4];             // xyz towards the light along the axis; w the cone's power
    float factors[4];         // the cone's dot scale and bias, the shadow's fade, 1 in the sun's map
    float color[4];           // rgb linear light times the spot share
};
static_assert(sizeof(SpotBlock) == 128, "a spot light is a matrix and four vectors");

// The shaders' uniform block (std140): two matrices, then vec4s, then the
// spot lights.
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
    float spotInfo[4]; // x the spot lights, y the steps through each
    SpotBlock spots[kFogSpots];
};
static_assert(sizeof(Params) == 256 + 16 + 128 * kFogSpots, "the fog's uniform block");

// r the sun's shadowed share, gba the spot lights' light.
constexpr VkFormat kMarchFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBindings = 5;
constexpr uint32_t kUniformBinding = 3;

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
        error = "the device cannot draw into R16G16B16A16_SFLOAT";
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
    // result, 3 the parameters, 4 the spot lights' shadow map (compared).
    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i == kUniformBinding ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = kBindings;
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
bool Pass(Core *core, Fog *fog, VkPipeline pipeline, Image *depth, Image *shadow, Image *spotShadow,
          Image *marched, const Transient &params, Image *target, const VkRect2D &area, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, fog->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    const VkDescriptorImageInfo images[kBindings] = {
        {fog->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {fog->compare, ImageSampleView(shadow), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {fog->nearest, ImageSampleView(marched), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {},
        {fog->compare, ImageSampleView(spotShadow), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
    };
    const VkDescriptorBufferInfo buffer{params.buffer, params.offset, sizeof(Params)};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == kUniformBinding)
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
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), kBindings, writes, 0, nullptr);
    UseImage(core, depth, ImageUse::Sampled);
    UseImage(core, shadow, ImageUse::Sampled);
    if (spotShadow != shadow)
        UseImage(core, spotShadow, ImageUse::Sampled);
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

bool DrawFog(Core *core, Fog *fog, Image *color, Image *depth, Image *shadowMap, Image *spotShadowMap,
             const SceneView &view, const FogInputs &inputs, const FogParams &params, std::string &error)
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
    const auto sampleable = [](Image *map) {
        if (!map)
            return false;
        const ImageDesc &info = ImageInfo(map);
        return info.samples == 1 && (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    };
    if (!sampleable(shadowMap))
        shadowMap = nullptr; // the sun's air marched as all lit, its spot lights left out
    if (!sampleable(spotShadowMap))
        spotShadowMap = nullptr;
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
    const bool sunShadowed = shadowMap && inputs.sunShadowed;
    block.camera[3] = sunShadowed ? 1.0f : 0.0f;
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
    uint32_t spots = 0;
    const float spotShare = std::fmax(params.spotShare, 0.0f);
    for (uint32_t i = 0; spotShare > 0.0f && i < inputs.spotCount && i < kFogSpots; ++i)
    {
        const FogInputs::Spot &spot = inputs.spots[i];
        if (!(spot.inSunMap ? shadowMap : spotShadowMap) || spot.radius <= 0.0f
            || !(spot.cosHalfFovInner > spot.cosHalfFovOuter))
            continue;
        const float x = spot.dir[0], y = spot.dir[1], z = spot.dir[2];
        const float norm = std::sqrt(x * x + y * y + z * z);
        if (norm < 1e-6f)
            continue;
        SpotBlock &to = block.spots[spots++];
        std::memcpy(to.shadowLookup, spot.shadowLookup, sizeof(to.shadowLookup));
        to.origin[0] = spot.origin[0];
        to.origin[1] = spot.origin[1];
        to.origin[2] = spot.origin[2];
        to.origin[3] = 1.0f / spot.radius;
        to.dir[0] = x / norm;
        to.dir[1] = y / norm;
        to.dir[2] = z / norm;
        to.dir[3] = std::fmax(spot.exponent, 0.0f);
        const float dotScale = 1.0f / (spot.cosHalfFovInner - spot.cosHalfFovOuter);
        to.factors[0] = dotScale;
        to.factors[1] = -dotScale * spot.cosHalfFovOuter;
        to.factors[2] = std::fmin(std::fmax(spot.fade, 0.0f), 1.0f);
        to.factors[3] = spot.inSunMap ? 1.0f : 0.0f;
        for (int c = 0; c < 3; ++c)
            to.color[c] = spotShare * Decode(std::fmax(spot.color[c], 0.0f));
    }
    block.spotInfo[0] = static_cast<float>(spots);
    block.spotInfo[1] = static_cast<float>(params.spotSteps ? params.spotSteps : 1u);

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
    // Without a shadow map the march reads nothing from its binding; the
    // scene depth stands in so that the set is whole. The march's own image
    // stands in for binding 2 while it is being drawn: the depth.
    Image *shadow = shadowMap ? shadowMap : depth;
    Image *spotShadow = spotShadowMap ? spotShadowMap : depth;
    if (!Pass(core, fog, fog->marchPipeline, depth, shadow, spotShadow, depth, uniforms, fog->marched, marchArea,
              error))
        return false;
    if (!Pass(core, fog, apply, depth, shadow, spotShadow, fog->marched, uniforms, color, view.area, error))
        return false;
    ++fog->stats.scenes;
    if (sunShadowed)
        ++fog->stats.shadowed;
    if (spots)
    {
        ++fog->stats.spotScenes;
        fog->stats.spots += spots;
    }
    return true;
}

FogStats Stats(const Fog *fog) { return fog ? fog->stats : FogStats{}; }
} // namespace gfx_vk::modern
