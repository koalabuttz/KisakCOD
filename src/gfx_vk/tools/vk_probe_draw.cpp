// KisakCOD-vulkan-probe --draw: see vk_probe_draw.h.

#include "vk_probe_draw.h"

#include "vk_probe_shaders.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vk_probe
{
namespace
{
constexpr uint32_t kTargetSize = 256;
constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kTextureSize = 64;
constexpr uint32_t kTextureLevels = 4;
constexpr float kClearColor[4] = {0.1f, 0.2f, 0.3f, 1.0f};
// Bytes per colour channel a check may differ by (rounding, filtering).
constexpr int kTolerance = 2;

struct Vertex
{
    float position[3];
    float texCoord[2];
    uint8_t color[4];
};
static_assert(sizeof(Vertex) == 24, "the pipeline's vertex input expects 24-byte vertices");

// Level colours below level 0 (which has four quadrants).
constexpr uint8_t kLevelColors[kTextureLevels][4] = {
    {0, 0, 0, 0},           // level 0: quadrants
    {255, 255, 0, 255},     // yellow
    {255, 0, 255, 255},     // magenta
    {0, 255, 255, 255},     // cyan
};
constexpr uint8_t kQuadrants[4][4] = {
    {255, 0, 0, 255},       // top left: red
    {0, 255, 0, 255},       // top right: green
    {0, 0, 255, 255},       // bottom left: blue
    {255, 255, 255, 255},   // bottom right: white
};

void Quad(Vertex *out, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1,
          uint8_t grey)
{
    const float corners[4][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    for (int i = 0; i < 4; ++i)
    {
        out[i] = {{corners[i][0], corners[i][1], z}, {corners[i][2], corners[i][3]}, {grey, grey, grey, 255}};
    }
}

// Two quads drawn in one indexed draw: B first (nearer, grey, one texel of the
// white quadrant), then A (the whole texture behind it). If the depth test did
// not keep B, A's white would show through.
constexpr int kStaticVertices = 8;
constexpr int kStaticIndices = 12;
void StaticGeometry(Vertex vertices[kStaticVertices], uint16_t indices[kStaticIndices])
{
    Quad(vertices + 0, 0.1f, 0.1f, 0.5f, 0.5f, 0.25f, 0.75f, 0.75f, 0.75f, 0.75f, 128);
    Quad(vertices + 4, -0.8f, -0.8f, 0.8f, 0.8f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 255);
    const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
    for (int q = 0; q < 2; ++q)
    {
        for (int i = 0; i < 6; ++i)
            indices[q * 6 + i] = static_cast<uint16_t>(quad[i] + 4 * q);
    }
}

void FillLevel(uint8_t *out, uint32_t rowPitch, uint32_t level)
{
    const uint32_t size = kTextureSize >> level;
    for (uint32_t y = 0; y < size; ++y)
    {
        uint8_t *row = out + static_cast<size_t>(y) * rowPitch;
        for (uint32_t x = 0; x < size; ++x)
        {
            const uint8_t *color = level ? kLevelColors[level] : kQuadrants[(y >= size / 2) * 2 + (x >= size / 2)];
            std::memcpy(row + 4 * x, color, 4);
        }
    }
}

struct Point
{
    const char *name;
    int x, y; // in the 256x256 target
    int rgb[3];
};

// Target pixel x maps to NDC (x + 0.5) / 128 - 1. Quad A covers pixels 25.6 to
// 230.4 with texture coordinates 0 to 1; B covers 140.8 to 192; C (sampled at
// level 2) covers x 64 to 115.2, y 140.8 to 192.
constexpr Point kPoints[] = {
    {"clear", 5, 5, {26, 51, 77}},
    {"red", 60, 60, {255, 0, 0}},
    {"green", 196, 60, {0, 255, 0}},
    {"blue", 60, 196, {0, 0, 255}},
    {"white", 215, 215, {255, 255, 255}},
    {"depth", 166, 166, {128, 128, 128}},
    {"mip_level", 90, 166, {255, 0, 255}},
};

bool Matches(const uint8_t *pixel, bool bgra, const int rgb[3], int got[3])
{
    got[0] = bgra ? pixel[2] : pixel[0];
    got[1] = pixel[1];
    got[2] = bgra ? pixel[0] : pixel[2];
    for (int c = 0; c < 3; ++c)
    {
        if (std::abs(got[c] - rgb[c]) > kTolerance)
            return false;
    }
    return true;
}

void Compare(const gfx_vk::ReadbackData &data, DrawCheck &check)
{
    const bool bgra = data.format == VK_FORMAT_B8G8R8A8_UNORM;
    for (const Point &point : kPoints)
    {
        // Scale the point to the read image (the swapchain is any size).
        const uint32_t x = static_cast<uint32_t>((point.x + 0.5) * data.width / kTargetSize);
        const uint32_t y = static_cast<uint32_t>((point.y + 0.5) * data.height / kTargetSize);
        const uint8_t *pixel = data.pixels + (static_cast<size_t>(y) * data.width + x) * 4;
        int got[3];
        ++check.points;
        if (!Matches(pixel, bgra, point.rgb, got))
        {
            ++check.mismatched;
            char line[128];
            std::snprintf(line, sizeof(line), "%s%s=%d,%d,%d expected=%d,%d,%d", check.detail.empty() ? "" : " ",
                          point.name, got[0], got[1], got[2], point.rgb[0], point.rgb[1], point.rgb[2]);
            check.detail += line;
        }
    }
}
} // namespace

class DrawScene
{
public:
    gfx_vk::Image *texture = nullptr;
    gfx_vk::Image *target = nullptr;
    gfx_vk::Image *depth = nullptr;
    gfx_vk::Buffer *vertices = nullptr;
    gfx_vk::Buffer *indices = nullptr;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    gfx_vk::Readback *targetRead = nullptr;
    gfx_vk::Readback *swapchainRead = nullptr;
};

namespace
{
bool UploadLevel(gfx_vk::Core *core, DrawScene *scene, uint32_t level, std::string &error)
{
    gfx_vk::ImageRegion region;
    region.mipLevel = level;
    region.width = region.height = kTextureSize >> level;
    uint32_t rowPitch = 0;
    uint32_t slicePitch = 0;
    void *data = gfx_vk::StageImageWrite(core, scene->texture, region, &rowPitch, &slicePitch);
    if (!data)
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    FillLevel(static_cast<uint8_t *>(data), rowPitch, level);
    return true;
}

bool CreatePipeline(gfx_vk::Core *core, DrawScene *scene, std::string &error)
{
    const gfx_vk::Dispatch &vk = gfx_vk::Functions(core);
    const VkDevice device = gfx_vk::DeviceHandle(core);

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &scene->setLayout) != VK_SUCCESS)
    {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &scene->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &scene->layout) != VK_SUCCESS)
    {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    const VkShaderModule vertex =
        gfx_vk::CreateShaderModule(core, vk_probe_shaders::kProbeDrawVert, sizeof(vk_probe_shaders::kProbeDrawVert));
    const VkShaderModule fragment =
        gfx_vk::CreateShaderModule(core, vk_probe_shaders::kProbeDrawFrag, sizeof(vk_probe_shaders::kProbeDrawFrag));
    if (!vertex || !fragment)
    {
        error = gfx_vk::LastError(core);
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

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;
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
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{};
    depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthState.depthTestEnable = VK_TRUE;
    depthState.depthWriteEnable = VK_TRUE;
    depthState.depthCompareOp = VK_COMPARE_OP_LESS;
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

    const VkFormat colorFormat = kTargetFormat;
    const VkFormat depthFormat = gfx_vk::DepthStencilFormat(core);
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = depthFormat;
    rendering.stencilAttachmentFormat = depthFormat; // the core binds both aspects

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
    info.layout = scene->layout;
    scene->pipeline = gfx_vk::CreateGraphicsPipeline(core, info);
    gfx_vk::DestroyShaderModule(core, vertex);
    gfx_vk::DestroyShaderModule(core, fragment);
    if (!scene->pipeline)
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    return true;
}

// Every frame: a short-lived buffer written through staging and destroyed at
// once (its destruction waits for the frame), and a rewrite of the smallest
// texture level with the same colour, which the frame then samples.
bool Churn(gfx_vk::Core *core, DrawScene *scene, int frame, std::string &error)
{
    gfx_vk::BufferDesc desc;
    desc.size = 256;
    desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    desc.name = "churn";
    gfx_vk::Buffer *buffer = gfx_vk::CreateBuffer(core, desc);
    if (!buffer)
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    void *data = gfx_vk::StageBufferWrite(core, buffer, 0, desc.size);
    if (!data)
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    std::memset(data, frame & 0xff, static_cast<size_t>(desc.size));
    gfx_vk::DestroyBuffer(core, buffer);
    return frame % 30 != 1 || UploadLevel(core, scene, kTextureLevels - 1, error);
}
} // namespace

DrawScene *CreateDrawScene(gfx_vk::Core *core, std::string &error)
{
    DrawScene *scene = new DrawScene();
    const VkFormat depthFormat = gfx_vk::DepthStencilFormat(core);
    const VkFormat swapchainFormat = gfx_vk::Swapchain(core).format;
    if (!gfx_vk::FormatSupports(core, kTargetFormat,
                                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT
                                    | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        || (swapchainFormat != VK_FORMAT_UNDEFINED
            && !gfx_vk::FormatSupports(core, swapchainFormat, VK_FORMAT_FEATURE_BLIT_DST_BIT)))
    {
        error = "the device cannot draw to R8G8B8A8 and blit it into the swapchain format";
        DestroyDrawScene(core, scene);
        return nullptr;
    }

    gfx_vk::ImageDesc texture;
    texture.format = kTargetFormat;
    texture.width = texture.height = kTextureSize;
    texture.mipLevels = kTextureLevels;
    texture.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    texture.name = "probe texture";
    gfx_vk::ImageDesc target;
    target.format = kTargetFormat;
    target.width = target.height = kTargetSize;
    target.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    target.name = "probe target";
    gfx_vk::ImageDesc depth;
    depth.format = depthFormat;
    depth.width = depth.height = kTargetSize;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth.name = "probe depth";
    scene->texture = gfx_vk::CreateImage(core, texture);
    scene->target = gfx_vk::CreateImage(core, target);
    scene->depth = gfx_vk::CreateImage(core, depth);

    gfx_vk::BufferDesc vertices;
    vertices.size = sizeof(Vertex) * kStaticVertices;
    vertices.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertices.name = "probe vertices";
    gfx_vk::BufferDesc indices;
    indices.size = sizeof(uint16_t) * kStaticIndices;
    indices.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    indices.name = "probe indices";
    scene->vertices = gfx_vk::CreateBuffer(core, vertices);
    scene->indices = gfx_vk::CreateBuffer(core, indices);
    if (!scene->texture || !scene->target || !scene->depth || !scene->vertices || !scene->indices)
    {
        error = gfx_vk::LastError(core);
        DestroyDrawScene(core, scene);
        return nullptr;
    }

    Vertex staticVertices[kStaticVertices];
    uint16_t staticIndices[kStaticIndices];
    StaticGeometry(staticVertices, staticIndices);
    void *vertexData = gfx_vk::StageBufferWrite(core, scene->vertices, 0, sizeof(staticVertices));
    if (vertexData)
        std::memcpy(vertexData, staticVertices, sizeof(staticVertices));
    void *indexData = vertexData ? gfx_vk::StageBufferWrite(core, scene->indices, 0, sizeof(staticIndices)) : nullptr;
    if (indexData)
        std::memcpy(indexData, staticIndices, sizeof(staticIndices));
    bool ok = indexData != nullptr;
    for (uint32_t level = 0; ok && level < kTextureLevels; ++level)
        ok = UploadLevel(core, scene, level, error);
    if (!ok)
    {
        if (error.empty())
            error = gfx_vk::LastError(core);
        DestroyDrawScene(core, scene);
        return nullptr;
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    scene->sampler = gfx_vk::GetSampler(core, sampler);
    if (!scene->sampler || !CreatePipeline(core, scene, error))
    {
        if (error.empty())
            error = gfx_vk::LastError(core);
        DestroyDrawScene(core, scene);
        return nullptr;
    }
    // The uploads go to the GPU now, as a map load's would, not with the first frame.
    if (!gfx_vk::Flush(core, false))
    {
        error = gfx_vk::LastError(core);
        DestroyDrawScene(core, scene);
        return nullptr;
    }
    return scene;
}

void DestroyDrawScene(gfx_vk::Core *core, DrawScene *scene)
{
    if (!scene)
        return;
    const gfx_vk::Dispatch &vk = gfx_vk::Functions(core);
    const VkDevice device = gfx_vk::DeviceHandle(core);
    gfx_vk::DestroyReadback(core, scene->targetRead);
    gfx_vk::DestroyReadback(core, scene->swapchainRead);
    gfx_vk::DestroyPipeline(core, scene->pipeline);
    gfx_vk::DestroyImage(core, scene->texture);
    gfx_vk::DestroyImage(core, scene->target);
    gfx_vk::DestroyImage(core, scene->depth);
    gfx_vk::DestroyBuffer(core, scene->vertices);
    gfx_vk::DestroyBuffer(core, scene->indices);
    // Layouts are not used by the GPU after recording; the core's own objects
    // above wait for the frames that use them.
    gfx_vk::WaitIdle(core);
    if (scene->layout)
        vk.vkDestroyPipelineLayout(device, scene->layout, nullptr);
    if (scene->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, scene->setLayout, nullptr);
    delete scene;
}

bool RecordDrawFrame(gfx_vk::Core *core, DrawScene *scene, int animate, std::string &error)
{
    if (!Churn(core, scene, animate, error))
        return false;

    // The uniform block: identity for the still frame, a slow wobble otherwise
    // (column-major, as GLSL reads a mat4).
    const float angle = animate ? 0.15f * std::sin(animate * 0.05f) : 0.0f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float transform[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const gfx_vk::Transient uniforms = gfx_vk::AllocateTransient(core, sizeof(transform), 256);
    // Quad C, rewritten every frame: nearest, sampled at level 2 (magenta).
    const gfx_vk::Transient dynamic = gfx_vk::AllocateTransient(core, 6 * sizeof(Vertex), 4);
    const VkDescriptorSet set = gfx_vk::AllocateDescriptorSet(core, scene->setLayout);
    if (!uniforms.data || !dynamic.data || !set)
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    std::memcpy(uniforms.data, transform, sizeof(transform));
    Vertex quad[4];
    Quad(quad, -0.5f, 0.1f, -0.1f, 0.5f, 0.1f, 0.0f, 0.0f, 1.0f, 1.0f, 255);
    Vertex *triangles = static_cast<Vertex *>(dynamic.data);
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i)
        triangles[i] = quad[order[i]];

    const gfx_vk::Dispatch &vk = gfx_vk::Functions(core);
    VkDescriptorBufferInfo bufferInfo{uniforms.buffer, uniforms.offset, sizeof(transform)};
    VkDescriptorImageInfo imageInfo{scene->sampler, gfx_vk::ImageView(scene->texture),
                                    VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imageInfo;
    vk.vkUpdateDescriptorSets(gfx_vk::DeviceHandle(core), 2, writes, 0, nullptr);

    gfx_vk::UseImage(core, scene->texture, gfx_vk::ImageUse::Sampled);
    gfx_vk::PassDesc pass;
    pass.name = "scene";
    pass.colorCount = 1;
    pass.color[0].image = scene->target;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    std::memcpy(pass.color[0].clear, kClearColor, sizeof(kClearColor));
    pass.depth.image = scene->depth;
    pass.depth.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (!gfx_vk::BeginPass(core, pass))
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    // One packet: everything it uses is captured by value (api is the same
    // dispatch table as vk, named apart so it does not shadow it).
    const VkPipeline pipeline = scene->pipeline;
    const VkPipelineLayout layout = scene->layout;
    const VkBuffer staticBuffer = gfx_vk::BufferHandle(scene->vertices);
    const VkBuffer indexBuffer = gfx_vk::BufferHandle(scene->indices);
    const VkBuffer dynamicBuffer = dynamic.buffer;
    const VkDeviceSize dynamicOffset = dynamic.offset;
    gfx_vk::Record(core, [=](VkCommandBuffer cmd, const gfx_vk::Dispatch &api) {
        api.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0.0f, 0.0f, float(kTargetSize), float(kTargetSize), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {kTargetSize, kTargetSize}};
        api.vkCmdSetViewport(cmd, 0, 1, &viewport);
        api.vkCmdSetScissor(cmd, 0, 1, &scissor);
        api.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        float lod = -1.0f;
        api.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(lod), &lod);
        const VkDeviceSize zero = 0;
        api.vkCmdBindVertexBuffers(cmd, 0, 1, &staticBuffer, &zero);
        api.vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        api.vkCmdDrawIndexed(cmd, kStaticIndices, 1, 0, 0, 0);
        lod = 2.0f;
        api.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(lod), &lod);
        api.vkCmdBindVertexBuffers(cmd, 0, 1, &dynamicBuffer, &dynamicOffset);
        api.vkCmdDraw(cmd, 6, 1, 0, 0);
    });
    gfx_vk::EndPass(core);

    // Shown by stretching the target over the whole swapchain image.
    gfx_vk::Image *swapchain = gfx_vk::SwapchainImage(core);
    if (!swapchain)
    {
        // Out of date (Present skips the frame), or the core failed.
        error = gfx_vk::LastError(core);
        return error.empty();
    }
    const gfx_vk::ImageDesc &shown = gfx_vk::ImageInfo(swapchain);
    const VkOffset3D src[2] = {{0, 0, 0}, {int32_t(kTargetSize), int32_t(kTargetSize), 1}};
    const VkOffset3D dst[2] = {{0, 0, 0}, {int32_t(shown.width), int32_t(shown.height), 1}};
    gfx_vk::BlitImage(core, scene->target, src, swapchain, dst, VK_FILTER_LINEAR);
    if (gfx_vk::LastError(core)[0])
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    return true;
}

bool StartDrawCheck(gfx_vk::Core *core, DrawScene *scene, bool alsoSwapchain, std::string &error)
{
    scene->targetRead = gfx_vk::ReadImage(core, scene->target, 0, 0);
    if (scene->targetRead && alsoSwapchain && gfx_vk::Swapchain(core).canCapture)
    {
        gfx_vk::Image *shown = gfx_vk::SwapchainImage(core);
        if (!shown)
        {
            error = gfx_vk::LastError(core)[0] ? gfx_vk::LastError(core) : "the swapchain went out of date";
            return false;
        }
        scene->swapchainRead = gfx_vk::ReadImage(core, shown, 0, 0);
    }
    if (!scene->targetRead || (alsoSwapchain && gfx_vk::Swapchain(core).canCapture && !scene->swapchainRead))
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    return true;
}

bool FinishDrawCheck(gfx_vk::Core *core, DrawScene *scene, DrawCheck &target, DrawCheck &swapchain,
                     std::string &error)
{
    gfx_vk::ReadbackData data{};
    if (!gfx_vk::WaitReadback(core, scene->targetRead, &data))
    {
        error = gfx_vk::LastError(core);
        return false;
    }
    Compare(data, target);
    if (scene->swapchainRead)
    {
        if (!gfx_vk::WaitReadback(core, scene->swapchainRead, &data))
        {
            error = gfx_vk::LastError(core);
            return false;
        }
        Compare(data, swapchain);
    }
    gfx_vk::DestroyReadback(core, scene->targetRead);
    gfx_vk::DestroyReadback(core, scene->swapchainRead);
    scene->targetRead = scene->swapchainRead = nullptr;
    return true;
}
} // namespace vk_probe
