// The Modern path's screen-space reflections: see vk_modern.h and
// docs/vulkan-modern.md ("Screen-space reflections"). The shader is
// src/gfx_vk/modern/shaders/reflections.frag.

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
// The shader's push constants (reflections.frag), padded to 16-byte rows.
struct Push
{
    float clipToView[4][4];    // column-vector matrices (the row-vector ones, as they are)
    float viewToClip[4][4];
    float viewToHistory[4][4];
    float area[4];
    float historyArea[4];
    float march[4];            // sky depth, longest ray, thickness, steps
    float extra[4];            // nearest view z, frame
};
static_assert(sizeof(Push) == 256, "reflections.frag's push constant block is 256 bytes");

using Matrix = double[4][4];

void Multiply(const Matrix a, const Matrix b, Matrix out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += a[r][k] * b[k][c];
            out[r][c] = sum;
        }
}

// Gauss-Jordan with partial pivoting.
bool Invert(const Matrix m, Matrix out)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            a[r][c] = m[r][c];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-30)
            return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k)
                std::swap(a[c][k], a[pivot][k]);
        const double scale = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] *= scale;
        for (int r = 0; r < 4; ++r)
        {
            if (r == c)
                continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k)
                a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = a[r][c + 4];
    return true;
}

// This scene's view space to last scene's clip space without its jitter,
// row vectors: inverse(view) * previous view * previous projection. Worked
// in double, since the views' translations reach tens of thousands of units.
bool ViewToHistory(const SceneView &current, const SceneView &previous, float out[4][4])
{
    Matrix view, inverse, previousView, previousProjection, world, result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            view[r][c] = current.view[r][c];
            previousView[r][c] = previous.view[r][c];
            previousProjection[r][c] = previous.projection[r][c];
        }
    previousProjection[2][0] = 0.0;
    previousProjection[2][1] = 0.0;
    if (!Invert(view, inverse))
        return false;
    Multiply(inverse, previousView, world);
    Multiply(world, previousProjection, result);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = static_cast<float>(result[r][c]);
    return true;
}

uint32_t LevelsFor(uint32_t wanted, uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    uint32_t side = std::min(width, height);
    while (levels < std::min(wanted, kMaxReflectionLevels) && side >= 2)
    {
        side /= 2;
        ++levels;
    }
    return levels;
}
} // namespace

struct Reflections
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    // The pass draws into `traced` (an attachment has one level); the levels
    // are made from it in `halves` (one image each, a blit cannot read and
    // write one image) and copied into `chain`, the image the readers get.
    Image *traced = nullptr;
    Image *halves[kMaxReflectionLevels] = {};
    Image *chain = nullptr;
    uint32_t frame = 0;
    ReflectionStats stats{};
};

namespace
{
void DestroyImages(Core *core, Reflections &reflections)
{
    DestroyImage(core, reflections.traced);
    reflections.traced = nullptr;
    for (Image *&half : reflections.halves)
    {
        DestroyImage(core, half);
        half = nullptr;
    }
    DestroyImage(core, reflections.chain);
    reflections.chain = nullptr;
}

// The images for a depth of this size and this many levels.
bool EnsureImages(Core *core, Reflections &reflections, uint32_t width, uint32_t height, uint32_t levels)
{
    if (reflections.chain)
    {
        const ImageDesc &have = ImageInfo(reflections.chain);
        if (have.width == width && have.height == height && have.mipLevels == levels)
            return true;
        DestroyImages(core, reflections);
    }
    ImageDesc desc;
    desc.format = kReflectionsFormat;
    desc.width = width;
    desc.height = height;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.name = "modern reflections traced";
    reflections.traced = CreateImage(core, desc);
    if (!reflections.traced)
        return false;
    for (uint32_t level = 1; level < levels; ++level)
    {
        desc.width = std::max(width >> level, 1u);
        desc.height = std::max(height >> level, 1u);
        // Sampled only because the core gives every image a view, which
        // needs a usage a view can have.
        desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = "modern reflections level";
        reflections.halves[level] = CreateImage(core, desc);
        if (!reflections.halves[level])
            return false;
    }
    desc.width = width;
    desc.height = height;
    desc.mipLevels = levels;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.name = "modern reflections";
    reflections.chain = CreateImage(core, desc);
    return reflections.chain != nullptr;
}
} // namespace

Reflections *CreateReflections(Core *core, std::string &error)
{
    if (!FormatSupports(core, kReflectionsFormat,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                            | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
    {
        error = "the device cannot draw into, filter and blit R16G16B16A16_SFLOAT";
        return nullptr;
    }
    if (Limits(core).maxPushConstantsSize < sizeof(Push))
    {
        error = "the device's push constants are smaller than 256 bytes";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Reflections *reflections = new Reflections();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyReflections(core, reflections);
        return nullptr;
    };
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 3;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &reflections->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &reflections->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &reflections->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    reflections->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    reflections->fragment = CreateShaderModule(core, shaders::kReflectionsFrag, sizeof(shaders::kReflectionsFrag));
    if (!reflections->vertex || !reflections->fragment)
        return fail(LastError(core));
    std::string pipelineError;
    reflections->pipeline = CreateFullscreenPipeline(core, reflections->layout, reflections->vertex,
                                                     reflections->fragment, kReflectionsFormat, false, pipelineError);
    if (!reflections->pipeline)
        return fail(pipelineError);
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    reflections->nearest = GetSampler(core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    reflections->linear = GetSampler(core, sampler);
    if (!reflections->nearest || !reflections->linear)
        return fail(LastError(core));
    return reflections;
}

void DestroyReflections(Core *core, Reflections *reflections)
{
    if (!reflections)
        return;
    DestroyPipeline(core, reflections->pipeline);
    DestroyImages(core, *reflections);
    DestroyShaderModule(core, reflections->vertex);
    DestroyShaderModule(core, reflections->fragment);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (reflections->layout)
        vk.vkDestroyPipelineLayout(device, reflections->layout, nullptr);
    if (reflections->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, reflections->setLayout, nullptr);
    delete reflections;
}

Image *TraceReflections(Core *core, Reflections *reflections, Image *depth, Image *normals, Image *history,
                        const SceneView &view, const SceneView &historyView, const ReflectionParams &params,
                        std::string &error)
{
    if (!reflections || !depth || !normals || !history)
    {
        error = !history ? "no history" : "no depth or normals image";
        return nullptr;
    }
    const ImageDesc &depthInfo = ImageInfo(depth);
    const ImageDesc &normalsInfo = ImageInfo(normals);
    const ImageDesc &historyInfo = ImageInfo(history);
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return nullptr;
    }
    if (normalsInfo.width != depthInfo.width || normalsInfo.height != depthInfo.height
        || !(historyInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the normals are not the depth's size, or the history cannot be sampled";
        return nullptr;
    }
    if (!Inside(view.area, depthInfo) || !Inside(historyView.area, historyInfo))
    {
        error = "the viewport is not inside the scene depth, or the last one inside the history";
        return nullptr;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return nullptr;
    }
    Push push{};
    float clipToView[4][4];
    if (!ClipToView(view, clipToView) || !ViewToHistory(view, historyView, push.viewToHistory))
    {
        error = "the scene's projection or view cannot be inverted";
        return nullptr;
    }
    std::memcpy(push.clipToView, clipToView, sizeof(push.clipToView));
    std::memcpy(push.viewToClip, view.projection, sizeof(push.viewToClip));
    push.area[0] = static_cast<float>(view.area.offset.x);
    push.area[1] = static_cast<float>(view.area.offset.y);
    push.area[2] = static_cast<float>(view.area.extent.width);
    push.area[3] = static_cast<float>(view.area.extent.height);
    push.historyArea[0] = static_cast<float>(historyView.area.offset.x);
    push.historyArea[1] = static_cast<float>(historyView.area.offset.y);
    push.historyArea[2] = static_cast<float>(historyView.area.extent.width);
    push.historyArea[3] = static_cast<float>(historyView.area.extent.height);
    push.march[0] = SkyDepth(view);
    push.march[1] = std::max(params.maxDistance, 1.0f);
    push.march[2] = std::max(params.thickness, 0.0f);
    push.march[3] = static_cast<float>(std::max(params.steps, 1u));
    // The near plane's view z (where depth is 0 on the view's axis), a
    // hundredth further: no ray is marched nearer.
    {
        float point[4] = {};
        for (int c = 0; c < 4; ++c)
            point[c] = clipToView[3][c];
        push.extra[0] = point[3] != 0.0f ? 1.01f * point[2] / point[3] : 0.0f;
    }
    push.extra[1] = static_cast<float>(reflections->frame++ % 64);

    const uint32_t levels = LevelsFor(std::max(params.levels, 1u), depthInfo.width, depthInfo.height);
    if (!EnsureImages(core, *reflections, depthInfo.width, depthInfo.height, levels))
    {
        DestroyImages(core, *reflections);
        error = LastError(core);
        return nullptr;
    }

    const VkDescriptorSet set = AllocateDescriptorSet(core, reflections->setLayout);
    if (!set)
    {
        error = LastError(core);
        return nullptr;
    }
    const VkDescriptorImageInfo images[3] = {
        {reflections->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {reflections->nearest, ImageSampleView(normals), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {reflections->linear, ImageSampleView(history), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 3, writes, 0, nullptr);

    UseImage(core, depth, ImageUse::Sampled);
    UseImage(core, normals, ImageUse::Sampled);
    UseImage(core, history, ImageUse::Sampled);
    // The whole image is drawn: the shader writes no reflection outside the area.
    const VkRect2D whole = {{0, 0}, {depthInfo.width, depthInfo.height}};
    PassDesc pass;
    pass.name = "modern reflections";
    pass.colorCount = 1;
    pass.color[0].image = reflections->traced;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pass.area = whole;
    if (!BeginPass(core, pass))
    {
        error = LastError(core);
        return nullptr;
    }
    const VkPipeline pipeline = reflections->pipeline;
    const VkPipelineLayout layout = reflections->layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(whole.extent.width),
                                  static_cast<float>(whole.extent.height), 0.0f, 1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &whole);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);

    // The levels: each a 2x2 average of the one before (a linear blit to half
    // size), copied into the chain.
    const auto box = [](uint32_t width, uint32_t height, VkOffset3D out[2]) {
        out[0] = {0, 0, 0};
        out[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    };
    VkOffset3D full[2];
    box(depthInfo.width, depthInfo.height, full);
    BlitImage(core, reflections->traced, full, reflections->chain, full, VK_FILTER_NEAREST, 0, 0, 0, 0);
    for (uint32_t level = 1; level < levels; ++level)
    {
        Image *from = level == 1 ? reflections->traced : reflections->halves[level - 1];
        const ImageDesc &fromInfo = ImageInfo(from);
        const ImageDesc &toInfo = ImageInfo(reflections->halves[level]);
        VkOffset3D source[2], target[2];
        box(fromInfo.width, fromInfo.height, source);
        box(toInfo.width, toInfo.height, target);
        BlitImage(core, from, source, reflections->halves[level], target, VK_FILTER_LINEAR);
        BlitImage(core, reflections->halves[level], target, reflections->chain, target, VK_FILTER_NEAREST, 0, 0,
                  level, 0);
    }
    UseImage(core, reflections->chain, ImageUse::Sampled);
    ++reflections->stats.traces;
    reflections->stats.levels = levels;
    return reflections->chain;
}

ReflectionStats Stats(const Reflections *reflections) { return reflections ? reflections->stats : ReflectionStats{}; }
} // namespace gfx_vk::modern
