// The Modern path's velocity pass: see vk_modern.h and docs/vulkan-modern.md.
// The shaders are src/gfx_vk/modern/shaders/velocity.*.

#include "vk_modern.h"

#include "vk_modern_shaders.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
// The vertices the engine draws moving surfaces from (GfxPackedVertex) are
// 32 bytes; the stride is baked into the pipeline, since dynamic strides are
// not everywhere (OptionalFeatures::dynamicVertexStride).
constexpr uint32_t kStride = 32;

// The shaders' push constants (velocity.vert and .frag, std430).
struct Push
{
    float clip[16];
    float prevClip[16];
    float jitters[4];
};
static_assert(sizeof(Push) == 144, "velocity.*'s push constant block is 144 bytes");
} // namespace

struct Velocity
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    Image *image = nullptr;
    VelocityStats stats{};
};

namespace
{
bool CreatePipeline(Core *core, Velocity &velocity, std::string &error)
{
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &velocity.setLayout) != VK_SUCCESS)
    {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &velocity.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &velocity.layout) != VK_SUCCESS)
    {
        error = "vkCreatePipelineLayout failed";
        return false;
    }

    const VkShaderModule vertex = CreateShaderModule(core, shaders::kVelocityVert, sizeof(shaders::kVelocityVert));
    const VkShaderModule fragment = CreateShaderModule(core, shaders::kVelocityFrag, sizeof(shaders::kVelocityFrag));
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
    // This frame's vertices, and last frame's (the same vertices again for
    // all but skinned surfaces).
    const VkVertexInputBindingDescription vertexBindings[2] = {{0, kStride, VK_VERTEX_INPUT_RATE_VERTEX},
                                                               {1, kStride, VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription positions[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                            {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = vertexBindings;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = positions;
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
    // Faces the scene culled are behind what it shows, so the depth test
    // in the fragment shader drops them anyway.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{};
    depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
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
    rendering.pColorAttachmentFormats = &kVelocityFormat;

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
    info.layout = velocity.layout;
    velocity.pipeline = CreateGraphicsPipeline(core, info);
    DestroyShaderModule(core, vertex);
    DestroyShaderModule(core, fragment);
    if (!velocity.pipeline)
    {
        error = LastError(core);
        return false;
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    velocity.nearest = GetSampler(core, sampler);
    if (!velocity.nearest)
    {
        error = LastError(core);
        return false;
    }
    return true;
}

bool EnsureImage(Core *core, Velocity &velocity, uint32_t width, uint32_t height, std::string &error)
{
    if (velocity.image)
    {
        const ImageDesc &have = ImageInfo(velocity.image);
        if (have.width == width && have.height == height)
            return true;
        DestroyImage(core, velocity.image);
        velocity.image = nullptr;
    }
    ImageDesc desc;
    desc.format = kVelocityFormat;
    desc.width = width;
    desc.height = height;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.name = "modern velocity";
    velocity.image = CreateImage(core, desc);
    if (!velocity.image)
    {
        error = LastError(core);
        return false;
    }
    return true;
}
} // namespace

Velocity *CreateVelocity(Core *core, std::string &error)
{
    if (!FormatSupports(core, kVelocityFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    {
        error = "the device cannot draw into R16G16_SFLOAT";
        return nullptr;
    }
    if (Limits(core).maxPushConstantsSize < sizeof(Push))
    {
        error = "the device's push constants are smaller than 144 bytes";
        return nullptr;
    }
    Velocity *velocity = new Velocity();
    if (!CreatePipeline(core, *velocity, error))
    {
        DestroyVelocity(core, velocity);
        return nullptr;
    }
    return velocity;
}

void DestroyVelocity(Core *core, Velocity *velocity)
{
    if (!velocity)
        return;
    DestroyPipeline(core, velocity->pipeline);
    DestroyImage(core, velocity->image);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (velocity->layout)
        vk.vkDestroyPipelineLayout(device, velocity->layout, nullptr);
    if (velocity->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, velocity->setLayout, nullptr);
    delete velocity;
}

VelocityStats Stats(const Velocity *velocity) { return velocity ? velocity->stats : VelocityStats{}; }

Image *DrawVelocity(Core *core, Velocity *velocity, Image *depth, const SceneView &view, const SceneView &previous,
                    const MovedSurface *surfaces, uint32_t count, std::string &error)
{
    error.clear();
    if (!velocity || !depth)
    {
        error = "no depth image";
        return nullptr;
    }
    uint32_t usable = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (surfaces[i].stride == kStride && surfaces[i].indexCount && surfaces[i].vertices && surfaces[i].indices)
            ++usable;
    if (!usable)
        return nullptr;
    const ImageDesc &depthInfo = ImageInfo(depth);
    if (depthInfo.samples != 1 || !(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene depth is multisampled or cannot be sampled";
        return nullptr;
    }
    const VkRect2D area = view.area;
    if (!area.extent.width || !area.extent.height || area.offset.x < 0 || area.offset.y < 0
        || area.offset.x + area.extent.width > depthInfo.width
        || area.offset.y + area.extent.height > depthInfo.height)
    {
        error = "the viewport is not inside the scene's images";
        return nullptr;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return nullptr;
    }
    if (!EnsureImage(core, *velocity, depthInfo.width, depthInfo.height, error))
        return nullptr;

    const VkDescriptorSet set = AllocateDescriptorSet(core, velocity->setLayout);
    if (!set)
    {
        error = LastError(core);
        return nullptr;
    }
    const VkDescriptorImageInfo image{velocity->nearest, ImageSampleView(depth), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
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
    pass.name = "modern velocity";
    pass.colorCount = 1;
    pass.color[0].image = velocity->image;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pass.color[0].clear[0] = kNoOwnMotion;
    pass.color[0].clear[1] = kNoOwnMotion;
    pass.area = area;
    if (!BeginPass(core, pass))
    {
        error = LastError(core);
        return nullptr;
    }
    const VkPipeline pipeline = velocity->pipeline;
    const VkPipelineLayout layout = velocity->layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
    });
    const float jitters[4] = {view.projection[2][0], view.projection[2][1], previous.projection[2][0],
                              previous.projection[2][1]};
    uint32_t drawn = 0, skinned = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const MovedSurface &s = surfaces[i];
        if (s.stride != kStride || !s.indexCount || !s.vertices || !s.indices)
            continue;
        // A skinned surface reads last frame's vertices from the kept copy,
        // which starts at the first vertex the draw uses: both bindings start
        // there, and the draw's vertex offset takes the indices to it.
        VkBuffer buffers[2] = {s.vertices, s.vertices};
        VkDeviceSize offsets[2] = {s.vertexOffset, s.vertexOffset};
        int32_t drawBase = s.vertexBase;
        if (s.previous)
        {
            const int64_t first = static_cast<int64_t>(s.vertexBase) + s.minIndex;
            if (first < 0 || !s.vertexCount)
                continue;
            buffers[1] = s.previous;
            offsets[0] = s.vertexOffset + static_cast<VkDeviceSize>(first) * kStride;
            offsets[1] = s.previousOffset;
            drawBase = -static_cast<int32_t>(s.minIndex);
            ++skinned;
        }
        Push push{};
        std::memcpy(push.clip, s.clip, sizeof(push.clip));
        std::memcpy(push.prevClip, s.prevClip, sizeof(push.prevClip));
        std::memcpy(push.jitters, jitters, sizeof(push.jitters));
        // As the Legacy adapter maps D3D9's viewport: flipped (D3D9's clip
        // space has y up) and moved by half a pixel (D3D9 samples pixel
        // centres at whole coordinates).
        const VkViewport viewport{s.viewport[0] + 0.5f,
                                  s.viewport[1] + s.viewport[3] + 0.5f,
                                  s.viewport[2],
                                  -s.viewport[3],
                                  s.minDepth,
                                  s.maxDepth};
        const VkBuffer indices = s.indices;
        const VkIndexType indexType = s.indexType;
        const uint32_t indexCount = s.indexCount;
        const uint32_t firstIndex = s.firstIndex;
        Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(push), &push);
            vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
            vk.vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
            vk.vkCmdBindIndexBuffer(cmd, indices, 0, indexType);
            vk.vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, drawBase, 0);
        });
        ++drawn;
    }
    EndPass(core);
    UseImage(core, velocity->image, ImageUse::Sampled);
    ++velocity->stats.passes;
    velocity->stats.surfaces += drawn;
    velocity->stats.skinned += skinned;
    return velocity->image;
}

// ---- Skinned surfaces' vertices, kept for the next scene ----

namespace
{
// Each half holds one scene's: 262144 vertices, several times what a busy
// scene's characters and view model have.
constexpr VkDeviceSize kHalfBytes = VkDeviceSize(8) << 20;

struct Kept
{
    VkDeviceSize offset;
    uint32_t vertexCount;
};
} // namespace

struct KeptVertices
{
    Buffer *buffer = nullptr;
    uint32_t half = 1; // the half the last KeepVertices wrote
    std::unordered_map<uint64_t, Kept> last, next;
    std::vector<VkBufferCopy> regions;
    KeptStats stats{};
};

KeptVertices *CreateKeptVertices() { return new KeptVertices(); }

void DestroyKeptVertices(Core *core, KeptVertices *kept)
{
    if (!kept)
        return;
    DestroyBuffer(core, kept->buffer);
    delete kept;
}

bool FindKept(const KeptVertices *kept, uint64_t key, uint32_t vertexCount, VkBuffer &buffer, VkDeviceSize &offset)
{
    if (!kept || !kept->buffer)
        return false;
    const auto found = kept->last.find(key);
    if (found == kept->last.end() || found->second.vertexCount != vertexCount)
        return false;
    buffer = BufferHandle(kept->buffer);
    offset = found->second.offset;
    return true;
}

void ForgetKept(KeptVertices *kept)
{
    if (kept)
        kept->last.clear();
}

bool KeepVertices(Core *core, KeptVertices *kept, const KeepRequest *requests, uint32_t count, std::string &error)
{
    error.clear();
    if (!kept)
        return false;
    kept->last.clear();
    if (!count)
        return true;
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return false;
    }
    if (!kept->buffer)
    {
        BufferDesc desc;
        desc.size = 2 * kHalfBytes;
        desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        desc.memory = MemoryKind::Device;
        desc.name = "modern kept vertices";
        kept->buffer = CreateBuffer(core, desc);
        if (!kept->buffer)
        {
            error = LastError(core);
            return false;
        }
    }
    // The other half from the one the velocity pass just read.
    const uint32_t half = kept->half ^ 1u;
    const VkDeviceSize base = half * kHalfBytes;
    VkDeviceSize used = 0;
    kept->next.clear();
    kept->regions.clear();
    uint32_t surfaces = 0;
    // Copies from one buffer go together (cached skinned surfaces all come
    // from the renderer's skinned cache).
    VkBuffer source = VK_NULL_HANDLE;
    const auto flush = [&]() {
        if (kept->regions.empty())
            return true;
        const bool ok = CopyBufferRegions(core, source, kept->buffer, kept->regions.data(),
                                          static_cast<uint32_t>(kept->regions.size()));
        kept->regions.clear();
        return ok;
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        const KeepRequest &r = requests[i];
        const VkDeviceSize bytes = VkDeviceSize(r.vertexCount) * kStride;
        if (!r.vertices || !bytes || used + bytes > kHalfBytes || kept->next.count(r.key))
            continue;
        if (r.vertices != source && !flush())
        {
            error = LastError(core);
            return false;
        }
        source = r.vertices;
        kept->regions.push_back({r.offset, base + used, bytes});
        kept->next[r.key] = Kept{base + used, r.vertexCount};
        used += bytes;
        ++surfaces;
    }
    if (!flush())
    {
        error = LastError(core);
        kept->next.clear();
        return false;
    }
    kept->last.swap(kept->next);
    kept->half = half;
    if (surfaces)
    {
        ++kept->stats.scenes;
        kept->stats.surfaces += surfaces;
        kept->stats.bytes += used;
    }
    return true;
}

KeptStats Stats(const KeptVertices *kept) { return kept ? kept->stats : KeptStats{}; }
} // namespace gfx_vk::modern
