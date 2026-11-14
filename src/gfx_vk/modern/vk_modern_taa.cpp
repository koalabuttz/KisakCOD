// Temporal anti-aliasing for the Modern path: see vk_modern.h and
// docs/vulkan-modern.md. The shaders are src/gfx_vk/modern/shaders/*.

#include "vk_modern.h"

#include "vk_modern_shaders.h"

#include <cmath>
#include <cstring>

namespace gfx_vk::modern
{
namespace
{
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The history's weight once it is there: about the last ten frames count.
constexpr float kFeedback = 0.9f;

// The shader's push constants (taa.frag, std430).
struct Push
{
    float clipToPrevClip[16];
    float area[4];
    float invImageSize[2];
    float jitter[2];
    float farDepth;
    float feedback;
    float haveVelocity;
};
static_assert(sizeof(Push) == 108, "taa.frag's push constant block is 108 bytes");

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

// view * projection with the jitter taken out.
void UnjitteredViewProjection(const SceneView &scene, Matrix out)
{
    Matrix view, projection;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            view[r][c] = scene.view[r][c];
            projection[r][c] = scene.projection[r][c];
        }
    projection[2][0] = 0.0;
    projection[2][1] = 0.0;
    Multiply(view, projection, out);
}

double Halton(uint32_t index, uint32_t base)
{
    double result = 0.0;
    double fraction = 1.0 / base;
    while (index)
    {
        result += fraction * (index % base);
        index /= base;
        fraction /= base;
    }
    return result;
}
} // namespace

bool Reprojection(const SceneView &current, const SceneView &previous, double out[4][4])
{
    Matrix now, before, inverse;
    UnjitteredViewProjection(current, now);
    UnjitteredViewProjection(previous, before);
    if (!Invert(now, inverse))
        return false;
    Multiply(inverse, before, out);
    return true;
}

void JitterPixels(uint32_t frame, float &x, float &y)
{
    const uint32_t index = frame % 8 + 1;
    x = static_cast<float>(Halton(index, 2) - 0.5);
    y = static_cast<float>(Halton(index, 3) - 0.5);
}

struct Taa
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    // history[latest] holds the last result; the next one is drawn into the other.
    Image *history[2] = {};
    uint32_t latest = 0;
    bool haveHistory = false;
    SceneView lastView{};
    TaaStats stats{};
};

namespace
{
bool CreatePipeline(Core *core, Taa &taa, std::string &error)
{
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);

    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 4;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &taa.setLayout) != VK_SUCCESS)
    {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &taa.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &taa.layout) != VK_SUCCESS)
    {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    const VkShaderModule vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    const VkShaderModule fragment = CreateShaderModule(core, shaders::kTaaFrag, sizeof(shaders::kTaaFrag));
    if (!vertex || !fragment)
    {
        error = LastError(core);
        DestroyShaderModule(core, vertex);
        DestroyShaderModule(core, fragment);
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
    rendering.pColorAttachmentFormats = &kHistoryFormat;

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
    info.layout = taa.layout;
    taa.pipeline = CreateGraphicsPipeline(core, info);
    DestroyShaderModule(core, vertex);
    DestroyShaderModule(core, fragment);
    if (!taa.pipeline)
    {
        error = LastError(core);
        return false;
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    taa.nearest = GetSampler(core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    taa.linear = GetSampler(core, sampler);
    if (!taa.nearest || !taa.linear)
    {
        error = LastError(core);
        return false;
    }
    return true;
}

// The history images match the scene's size; a new size starts the history again.
bool EnsureHistory(Core *core, Taa &taa, uint32_t width, uint32_t height, std::string &error)
{
    if (taa.history[0])
    {
        const ImageDesc &have = ImageInfo(taa.history[0]);
        if (have.width == width && have.height == height)
            return true;
        DestroyImage(core, taa.history[0]);
        DestroyImage(core, taa.history[1]);
        taa.history[0] = taa.history[1] = nullptr;
        taa.haveHistory = false;
    }
    ImageDesc desc;
    desc.format = kHistoryFormat;
    desc.width = width;
    desc.height = height;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (uint32_t i = 0; i < 2; ++i)
    {
        desc.name = i ? "taa history 1" : "taa history 0";
        taa.history[i] = CreateImage(core, desc);
        if (!taa.history[i])
        {
            error = LastError(core);
            return false;
        }
    }
    return true;
}
} // namespace

Taa *CreateTaa(Core *core, std::string &error)
{
    if (!FormatSupports(core, kHistoryFormat,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                            | VK_FORMAT_FEATURE_BLIT_SRC_BIT))
    {
        error = "the device cannot draw into, filter and blit from R16G16B16A16_SFLOAT";
        return nullptr;
    }
    Taa *taa = new Taa();
    if (!CreatePipeline(core, *taa, error))
    {
        DestroyTaa(core, taa);
        return nullptr;
    }
    return taa;
}

void DestroyTaa(Core *core, Taa *taa)
{
    if (!taa)
        return;
    DestroyPipeline(core, taa->pipeline);
    DestroyImage(core, taa->history[0]);
    DestroyImage(core, taa->history[1]);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (taa->layout)
        vk.vkDestroyPipelineLayout(device, taa->layout, nullptr);
    if (taa->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, taa->setLayout, nullptr);
    delete taa;
}

TaaStats Stats(const Taa *taa) { return taa ? taa->stats : TaaStats{}; }

bool LastView(const Taa *taa, SceneView &out)
{
    if (!taa || !taa->haveHistory)
        return false;
    out = taa->lastView;
    return true;
}

Image *LastHistory(const Taa *taa) { return taa && taa->haveHistory ? taa->history[taa->latest] : nullptr; }

bool Resolve(Core *core, Taa *taa, Image *color, Image *depth, const SceneView &view, std::string &error,
             Image *velocity)
{
    if (!taa || !color || !depth)
    {
        error = "no colour or depth image";
        return false;
    }
    if (velocity)
    {
        const ImageDesc &velocityInfo = ImageInfo(velocity);
        if (velocityInfo.width != ImageInfo(color).width || velocityInfo.height != ImageInfo(color).height
            || !(velocityInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        {
            error = "the velocity image is not the scene's size or cannot be sampled";
            return false;
        }
    }
    const ImageDesc &colorInfo = ImageInfo(color);
    const ImageDesc &depthInfo = ImageInfo(depth);
    if (colorInfo.samples != 1 || depthInfo.samples != 1)
    {
        error = "the scene is multisampled";
        return false;
    }
    if (!(colorInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        || !(colorInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        error = "the scene's images cannot be sampled or written by a blit";
        return false;
    }
    if (depthInfo.width != colorInfo.width || depthInfo.height != colorInfo.height)
    {
        error = "the depth image is not the colour image's size";
        return false;
    }
    if (!FormatSupports(core, colorInfo.format, VK_FORMAT_FEATURE_BLIT_DST_BIT))
    {
        error = "the scene's colour format is not a blit destination";
        return false;
    }
    const VkRect2D area = view.area;
    if (!area.extent.width || !area.extent.height || area.offset.x < 0 || area.offset.y < 0
        || area.offset.x + area.extent.width > colorInfo.width
        || area.offset.y + area.extent.height > colorInfo.height)
    {
        error = "the viewport is not inside the scene's images";
        return false;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    if (!EnsureHistory(core, *taa, colorInfo.width, colorInfo.height, error))
        return false;

    Push push{};
    double reprojection[4][4];
    const bool history = taa->haveHistory && !view.cut && Reprojection(view, taa->lastView, reprojection);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            push.clipToPrevClip[r * 4 + c] = history ? static_cast<float>(reprojection[r][c]) : (r == c ? 1.0f : 0.0f);
    push.area[0] = static_cast<float>(area.offset.x);
    push.area[1] = static_cast<float>(area.offset.y);
    push.area[2] = static_cast<float>(area.extent.width);
    push.area[3] = static_cast<float>(area.extent.height);
    push.invImageSize[0] = 1.0f / colorInfo.width;
    push.invImageSize[1] = 1.0f / colorInfo.height;
    push.jitter[0] = view.projection[2][0];
    push.jitter[1] = view.projection[2][1];
    push.farDepth = view.projection[2][2] < 1.0f ? view.projection[2][2] : 1.0f;
    push.feedback = history ? kFeedback : 0.0f;
    // Velocity is motion since last frame: with no history there is none to follow.
    push.haveVelocity = history && velocity ? 1.0f : 0.0f;

    Image *previous = taa->history[taa->latest];
    Image *next = taa->history[taa->latest ^ 1];
    const VkDescriptorSet set = AllocateDescriptorSet(core, taa->setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    // Without velocity the depth stands in for it (the shader does not read it).
    const VkDescriptorImageInfo images[4] = {
        {taa->nearest, ImageView(color), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {taa->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {taa->linear, ImageView(previous), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {taa->nearest, velocity ? ImageView(velocity) : ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 4, writes, 0, nullptr);

    UseImage(core, color, ImageUse::Sampled);
    UseImage(core, depth, ImageUse::Sampled);
    UseImage(core, previous, ImageUse::Sampled);
    if (velocity)
        UseImage(core, velocity, ImageUse::Sampled);
    PassDesc pass;
    pass.name = "modern taa";
    pass.colorCount = 1;
    pass.color[0].image = next;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pass.area = area;
    if (!BeginPass(core, pass))
    {
        error = LastError(core);
        return false;
    }
    const VkPipeline pipeline = taa->pipeline;
    const VkPipelineLayout layout = taa->layout;
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

    const VkOffset3D box[2] = {{area.offset.x, area.offset.y, 0},
                               {area.offset.x + static_cast<int32_t>(area.extent.width),
                                area.offset.y + static_cast<int32_t>(area.extent.height), 1}};
    BlitImage(core, next, box, color, box, VK_FILTER_NEAREST);

    taa->latest ^= 1;
    taa->haveHistory = true;
    taa->lastView = view;
    ++taa->stats.resolves;
    if (!history)
        ++taa->stats.restarts;
    return true;
}
} // namespace gfx_vk::modern
