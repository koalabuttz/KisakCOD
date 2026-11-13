// The Modern path's normals buffer: see vk_modern.h and docs/vulkan-modern.md
// ("Normals buffer"). The shader is src/gfx_vk/modern/shaders/normals.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <cmath>

namespace gfx_vk::modern
{
namespace
{
// The shader's push constants (normals.frag), padded to 16-byte rows.
struct Push
{
    float clipToView[4][4]; // column-vector matrix (the row-vector inverse, as it is)
    float area[4];
    float sky;
    float pad[3];
};
static_assert(sizeof(Push) == 96, "normals.frag's push constant block fits 96 bytes");

// Gauss-Jordan with partial pivoting, in double.
bool Invert(const double m[4][4], double out[4][4])
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
            {
                const double t = a[c][k];
                a[c][k] = a[pivot][k];
                a[pivot][k] = t;
            }
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
} // namespace

struct Normals
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    Image *image = nullptr;
    NormalsStats stats;
};

bool ClipToView(const SceneView &view, float out[4][4])
{
    double projection[4][4], inverse[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            projection[r][c] = view.projection[r][c];
    if (!Invert(projection, inverse))
        return false;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = static_cast<float>(inverse[r][c]);
    return true;
}

float SkyDepth(const SceneView &view)
{
    // A point at infinity in front of the camera (view (0, 0, 1, 0)) has
    // depth projection[2][2] / projection[2][3]; nothing nearer than about
    // 4 million times the near plane comes within 2^-22 of it. A projection
    // without one (no w from z) has only the clear value, 1.
    const float p22 = view.projection[2][2], p23 = view.projection[2][3];
    const float far = p23 > 0.0f && p22 / p23 < 1.0f ? p22 / p23 : 1.0f;
    return far - 1.0f / 4194304.0f;
}

Normals *CreateNormals(Core *core, std::string &error)
{
    if (!FormatSupports(core, kNormalsFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    {
        error = "the device cannot draw into A2B10G10R10_UNORM";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Normals *normals = new Normals();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyNormals(core, normals);
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
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &normals->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &normals->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &normals->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    normals->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    normals->fragment = CreateShaderModule(core, shaders::kNormalsFrag, sizeof(shaders::kNormalsFrag));
    if (!normals->vertex || !normals->fragment)
        return fail(LastError(core));
    std::string pipelineError;
    normals->pipeline = CreateFullscreenPipeline(core, normals->layout, normals->vertex, normals->fragment,
                                                 kNormalsFormat, false, pipelineError);
    if (!normals->pipeline)
        return fail(pipelineError);
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    normals->nearest = GetSampler(core, sampler);
    if (!normals->nearest)
        return fail(LastError(core));
    return normals;
}

void DestroyNormals(Core *core, Normals *normals)
{
    if (!normals)
        return;
    DestroyPipeline(core, normals->pipeline);
    DestroyImage(core, normals->image);
    DestroyShaderModule(core, normals->vertex);
    DestroyShaderModule(core, normals->fragment);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (normals->layout)
        vk.vkDestroyPipelineLayout(device, normals->layout, nullptr);
    if (normals->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, normals->setLayout, nullptr);
    delete normals;
}

Image *BuildNormals(Core *core, Normals *normals, Image *depth, const SceneView &view, std::string &error)
{
    if (!normals || !depth)
    {
        error = "no depth image";
        return nullptr;
    }
    const ImageDesc &depthInfo = ImageInfo(depth);
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return nullptr;
    }
    if (!Inside(view.area, depthInfo))
    {
        error = "the viewport is not inside the scene depth";
        return nullptr;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return nullptr;
    }
    Push push{};
    if (!ClipToView(view, push.clipToView))
    {
        error = "the scene's projection cannot be inverted";
        return nullptr;
    }
    push.area[0] = static_cast<float>(view.area.offset.x);
    push.area[1] = static_cast<float>(view.area.offset.y);
    push.area[2] = static_cast<float>(view.area.extent.width);
    push.area[3] = static_cast<float>(view.area.extent.height);
    push.sky = SkyDepth(view);

    if (normals->image)
    {
        const ImageDesc &have = ImageInfo(normals->image);
        if (have.width != depthInfo.width || have.height != depthInfo.height)
        {
            DestroyImage(core, normals->image);
            normals->image = nullptr;
        }
    }
    if (!normals->image)
    {
        ImageDesc desc;
        desc.format = kNormalsFormat;
        desc.width = depthInfo.width;
        desc.height = depthInfo.height;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        desc.name = "modern normals";
        normals->image = CreateImage(core, desc);
        if (!normals->image)
        {
            error = LastError(core);
            return nullptr;
        }
    }

    const VkDescriptorSet set = AllocateDescriptorSet(core, normals->setLayout);
    if (!set)
    {
        error = LastError(core);
        return nullptr;
    }
    const VkDescriptorImageInfo image{normals->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 1, &write, 0, nullptr);

    UseImage(core, depth, ImageUse::Sampled);
    PassDesc pass;
    pass.name = "modern normals";
    pass.colorCount = 1;
    pass.color[0].image = normals->image;
    // Only the area is drawn: outside it the image holds "no surface".
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pass.color[0].clear[0] = pass.color[0].clear[1] = pass.color[0].clear[2] = 0.5f;
    pass.area = view.area;
    if (!BeginPass(core, pass))
    {
        error = LastError(core);
        return nullptr;
    }
    const VkPipeline pipeline = normals->pipeline;
    const VkPipelineLayout layout = normals->layout;
    const VkRect2D area = view.area;
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
    UseImage(core, normals->image, ImageUse::Sampled);
    ++normals->stats.builds;
    return normals->image;
}

Image *CurrentNormals(const Normals *normals) { return normals && normals->stats.builds ? normals->image : nullptr; }

NormalsStats Stats(const Normals *normals) { return normals ? normals->stats : NormalsStats{}; }
} // namespace gfx_vk::modern
