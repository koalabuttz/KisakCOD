// Vulkan Legacy adapter: state, pipelines, passes, draws, clears, copies,
// present and queries. See vk_legacy.h.

#include "vk_legacy_internal.h"

#include <cmath>

namespace gfx_vk::legacy
{
static_assert(sizeof(PipelineKey) == 48, "PipelineKey must have no padding: it is hashed and compared as bytes");

namespace
{
// One-time warnings (Device::warnedOnce bits).
enum Warning : uint32_t
{
    WarnFixedFunction,
    WarnFeedback,
    WarnNoTargets,
    WarnUnboundStream,
    WarnSystemMemTexture,
    WarnRenderTargetGap,
    WarnStretchRect,
};

float AsFloat(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void UnpackColor(uint32_t color, float out[4])
{
    out[0] = float((color >> 16) & 0xff) / 255.0f;
    out[1] = float((color >> 8) & 0xff) / 255.0f;
    out[2] = float(color & 0xff) / 255.0f;
    out[3] = float((color >> 24) & 0xff) / 255.0f;
}

VkCompareOp CompareOp(uint32_t func)
{
    // D3DCMPFUNC - 1 is VkCompareOp.
    return func >= d3d::CMP_NEVER && func <= d3d::CMP_ALWAYS ? VkCompareOp(func - 1) : VK_COMPARE_OP_ALWAYS;
}

VkStencilOp StencilOp(uint32_t op)
{
    switch (op)
    {
    case d3d::STENCILOP_ZERO:
        return VK_STENCIL_OP_ZERO;
    case d3d::STENCILOP_REPLACE:
        return VK_STENCIL_OP_REPLACE;
    case d3d::STENCILOP_INCRSAT:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case d3d::STENCILOP_DECRSAT:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case d3d::STENCILOP_INVERT:
        return VK_STENCIL_OP_INVERT;
    case d3d::STENCILOP_INCR:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case d3d::STENCILOP_DECR:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:
        return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor BlendFactor(uint32_t blend, bool alpha)
{
    switch (blend)
    {
    case d3d::BLEND_ONE:
        return VK_BLEND_FACTOR_ONE;
    case d3d::BLEND_SRCCOLOR:
        return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case d3d::BLEND_INVSRCCOLOR:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case d3d::BLEND_SRCALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case d3d::BLEND_INVSRCALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3d::BLEND_DESTALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case d3d::BLEND_INVDESTALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case d3d::BLEND_DESTCOLOR:
        return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case d3d::BLEND_INVDESTCOLOR:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case d3d::BLEND_SRCALPHASAT:
        return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case d3d::BLEND_BLENDFACTOR:
        return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case d3d::BLEND_INVBLENDFACTOR:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:
        return VK_BLEND_FACTOR_ZERO;
    }
}

// D3DBLEND_BOTHSRCALPHA and BOTHINVSRCALPHA set both factors from the source one.
void BlendPair(uint32_t src, uint32_t dst, bool alpha, bool noDestAlpha, uint8_t &srcOut, uint8_t &dstOut)
{
    VkBlendFactor s;
    VkBlendFactor d;
    if (src == d3d::BLEND_BOTHSRCALPHA)
    {
        s = VK_BLEND_FACTOR_SRC_ALPHA;
        d = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    else if (src == d3d::BLEND_BOTHINVSRCALPHA)
    {
        s = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        d = VK_BLEND_FACTOR_SRC_ALPHA;
    }
    else
    {
        s = BlendFactor(src, alpha);
        d = BlendFactor(dst, alpha);
    }
    if (noDestAlpha)
    {
        // Targets without alpha read destination alpha as 1.
        auto fix = [](VkBlendFactor f) {
            if (f == VK_BLEND_FACTOR_DST_ALPHA)
                return VK_BLEND_FACTOR_ONE;
            if (f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA)
                return VK_BLEND_FACTOR_ZERO;
            return f;
        };
        s = fix(s);
        d = fix(d);
    }
    srcOut = static_cast<uint8_t>(s);
    dstOut = static_cast<uint8_t>(d);
}

VkBlendOp BlendOperation(uint32_t op)
{
    switch (op)
    {
    case d3d::BLENDOP_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;
    case d3d::BLENDOP_REVSUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case d3d::BLENDOP_MIN:
        return VK_BLEND_OP_MIN;
    case d3d::BLENDOP_MAX:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}

VkFormat DeclFormat(uint8_t type)
{
    switch (type)
    {
    case d3d::DECLTYPE_FLOAT1:
        return VK_FORMAT_R32_SFLOAT;
    case d3d::DECLTYPE_FLOAT2:
        return VK_FORMAT_R32G32_SFLOAT;
    case d3d::DECLTYPE_FLOAT3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case d3d::DECLTYPE_FLOAT4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case d3d::DECLTYPE_D3DCOLOR:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case d3d::DECLTYPE_UBYTE4:
        return VK_FORMAT_R8G8B8A8_USCALED;
    case d3d::DECLTYPE_SHORT2:
        return VK_FORMAT_R16G16_SSCALED;
    case d3d::DECLTYPE_SHORT4:
        return VK_FORMAT_R16G16B16A16_SSCALED;
    case d3d::DECLTYPE_UBYTE4N:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case d3d::DECLTYPE_SHORT2N:
        return VK_FORMAT_R16G16_SNORM;
    case d3d::DECLTYPE_SHORT4N:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case d3d::DECLTYPE_USHORT2N:
        return VK_FORMAT_R16G16_UNORM;
    case d3d::DECLTYPE_USHORT4N:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case d3d::DECLTYPE_UDEC3:
        return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case d3d::DECLTYPE_DEC3N:
        return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case d3d::DECLTYPE_FLOAT16_2:
        return VK_FORMAT_R16G16_SFLOAT;
    case d3d::DECLTYPE_FLOAT16_4:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

uint8_t TopologyClass(d3d::PrimitiveType type)
{
    if (type == d3d::PT_POINTLIST)
        return 0;
    return type == d3d::PT_LINELIST || type == d3d::PT_LINESTRIP ? 1 : 2;
}

VkPrimitiveTopology Topology(d3d::PrimitiveType type)
{
    switch (type)
    {
    case d3d::PT_POINTLIST:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case d3d::PT_LINELIST:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case d3d::PT_LINESTRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case d3d::PT_TRIANGLESTRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case d3d::PT_TRIANGLEFAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

uint32_t VertexCount(d3d::PrimitiveType type, uint32_t primitives)
{
    switch (type)
    {
    case d3d::PT_POINTLIST:
        return primitives;
    case d3d::PT_LINELIST:
        return primitives * 2;
    case d3d::PT_LINESTRIP:
        return primitives + 1;
    case d3d::PT_TRIANGLELIST:
        return primitives * 3;
    case d3d::PT_TRIANGLESTRIP:
    case d3d::PT_TRIANGLEFAN:
        return primitives + 2;
    default:
        return 0;
    }
}

uint32_t ColorCount(Surface *const targets[kMaxRenderTargets])
{
    uint32_t count = 0;
    while (count < kMaxRenderTargets && targets[count])
        ++count;
    return count;
}

bool UsesTexture(const Surface *surface, const Texture *texture) { return surface && surface->texture == texture; }

// The texture is one of the targets the next pass draws to.
bool IsTarget(const Device &device, const Texture *texture)
{
    for (const Surface *target : device.renderTargets)
    {
        if (UsesTexture(target, texture))
            return true;
    }
    return UsesTexture(device.depthStencil, texture);
}

bool PassMatches(const Device &device)
{
    return device.passOpen && std::memcmp(device.passTargets, device.renderTargets, sizeof(device.passTargets)) == 0
        && device.passDepth == device.depthStencil;
}

bool BeginTargetsPass(Device &device)
{
    const uint32_t colors = ColorCount(device.renderTargets);
    for (uint32_t i = colors; i < kMaxRenderTargets; ++i)
    {
        if (device.renderTargets[i])
            WarnOnce(device, WarnRenderTargetGap, "render target %u is set but %u is not; it is not drawn to", i,
                     colors);
    }
    if (!colors && !device.depthStencil)
    {
        WarnOnce(device, WarnNoTargets, "a draw or clear with no render target and no depth surface was skipped");
        return false;
    }
    PassDesc pass;
    pass.name = "legacy";
    pass.colorCount = colors;
    uint32_t width = UINT32_MAX;
    uint32_t height = UINT32_MAX;
    auto fit = [&](const Surface *surface) {
        width = std::min(width, LevelSize(surface->texture->desc.width, surface->level));
        height = std::min(height, LevelSize(surface->texture->desc.height, surface->level));
    };
    for (uint32_t i = 0; i < colors; ++i)
    {
        pass.color[i].image = device.renderTargets[i]->texture->image;
        fit(device.renderTargets[i]);
    }
    if (device.depthStencil)
    {
        pass.depth.image = device.depthStencil->texture->image;
        fit(device.depthStencil);
    }
    pass.area = {{0, 0}, {width, height}};
    if (!BeginPass(device.core, pass))
        return Fail(device, "BeginPass: %s", LastError(device.core));
    device.passOpen = true;
    std::memcpy(device.passTargets, device.renderTargets, sizeof(device.passTargets));
    device.passDepth = device.depthStencil;
    device.passWidth = width;
    device.passHeight = height;
    ++device.stats.passes;
    return true;
}

// The command buffer the bound pipeline and dynamic state belong to.
VkCommandBuffer CurrentCommands(Device &device)
{
    VkCommandBuffer cmd = Commands(device.core);
    if (cmd != device.stateCommands)
    {
        device.stateCommands = cmd;
        device.boundPipeline = VK_NULL_HANDLE;
        device.dynamicValid = false;
    }
    return cmd;
}

// ---- Layouts and pipelines ------------------------------------------------------------

Layout *GetLayout(Device &device, const LayoutKey &key)
{
    auto found = device.layouts.find(key);
    if (found != device.layouts.end())
        return found->second.get();

    std::unique_ptr<Layout> layout(new Layout());
    layout->key = key;
    const Dispatch &vk = *device.vk;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        if (key.uniforms & (1u << bit))
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = bit;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags = bit < 16 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(binding);
        }
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    if (!bindings.empty())
    {
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vk.vkCreateDescriptorSetLayout(device.dev, &info, nullptr, &layout->uniformSet) != VK_SUCCESS)
        {
            Fail(device, "vkCreateDescriptorSetLayout failed (uniforms 0x%x)", key.uniforms);
            return nullptr;
        }
    }
    bindings.clear();
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        if (!(key.samplers & (1u << bit)))
            continue;
        const ls::Stage stage = bit < 16 ? ls::Stage::Pixel : ls::Stage::Vertex;
        const uint32_t sampler = bit < 16 ? bit : bit - 16;
        const VkShaderStageFlags flags = bit < 16 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = ls::ImageBinding(stage, sampler);
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = flags;
        bindings.push_back(binding);
        binding.binding = ls::SamplerBinding(stage, sampler);
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings.push_back(binding);
    }
    if (!bindings.empty())
    {
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vk.vkCreateDescriptorSetLayout(device.dev, &info, nullptr, &layout->textureSet) != VK_SUCCESS)
        {
            if (layout->uniformSet)
                vk.vkDestroyDescriptorSetLayout(device.dev, layout->uniformSet, nullptr);
            Fail(device, "vkCreateDescriptorSetLayout failed (samplers 0x%x)", key.samplers);
            return nullptr;
        }
    }
    const VkDescriptorSetLayout sets[2] = {layout->uniformSet ? layout->uniformSet : device.emptySet,
                                           layout->textureSet ? layout->textureSet : device.emptySet};
    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = 2;
    pipelineInfo.pSetLayouts = sets;
    if (vk.vkCreatePipelineLayout(device.dev, &pipelineInfo, nullptr, &layout->pipeline) != VK_SUCCESS)
    {
        if (layout->uniformSet)
            vk.vkDestroyDescriptorSetLayout(device.dev, layout->uniformSet, nullptr);
        if (layout->textureSet)
            vk.vkDestroyDescriptorSetLayout(device.dev, layout->textureSet, nullptr);
        Fail(device, "vkCreatePipelineLayout failed");
        return nullptr;
    }
    Layout *result = layout.get();
    device.layouts.emplace(key, std::move(layout));
    return result;
}

LayoutKey LayoutFor(const Shader &vs, const Shader &ps)
{
    const ls::Reflection &vr = vs.translated.reflection;
    const ls::Reflection &pr = ps.translated.reflection;
    LayoutKey key;
    key.uniforms = (vr.uniforms & 0xffffu) | (pr.uniforms & 0xffffu) << 16;
    key.samplers = (pr.samplers & 0xffffu) | (vr.samplers & ((1u << kVertexSamplers) - 1)) << 16;
    return key;
}

VkShaderModule PixelModule(Device &device, Shader &ps, Shader &vs)
{
    std::vector<uint32_t> spirv;
    std::string error;
    if (ls::StagesLinkDirectly(vs.translated, ps.translated))
    {
        if (ps.module)
            return ps.module;
        if (!ls::BuildSpirv(ps.translated, nullptr, spirv, error))
        {
            Fail(device, "pixel shader %08x: %s", ps.translated.bytecodeHash, error.c_str());
            return VK_NULL_HANDLE;
        }
        ps.module = CreateShaderModule(device.core, spirv.data(), spirv.size() * sizeof(uint32_t));
        return ps.module;
    }
    auto found = ps.linked.find(vs.id);
    if (found != ps.linked.end())
        return found->second;
    if (!ls::BuildSpirv(ps.translated, &vs.translated, spirv, error))
    {
        Fail(device, "pixel shader %08x with vertex shader %08x: %s", ps.translated.bytecodeHash,
             vs.translated.bytecodeHash, error.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModule module = CreateShaderModule(device.core, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (module)
    {
        ps.linked.emplace(vs.id, module);
        ++device.stats.shaderPairs;
    }
    return module;
}

Pipeline *BuildPipeline(Device &device, const PipelineKey &key, Shader &vs, Shader &ps, const VertexDecl &decl)
{
    const TimeInto timer(device.stats.pipelineMicros);
    Layout *layout = GetLayout(device, LayoutFor(vs, ps));
    const VkShaderModule fragment = layout ? PixelModule(device, ps, vs) : VK_NULL_HANDLE;
    if (!fragment)
    {
        if (device.error.empty())
            Fail(device, "cannot build the pixel shader: %s", LastError(device.core));
        return nullptr;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    // Each vertex shader input reads the declaration element with its usage
    // and index; inputs the declaration lacks read zero.
    uint32_t bindingMask = 0;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const ls::IoEntry &input : vs.translated.reflection.inputs)
    {
        if (input.builtIn)
            continue;
        VkVertexInputAttributeDescription attribute{};
        attribute.location = input.location;
        attribute.binding = kZeroBinding;
        attribute.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        for (const VertexElement &element : decl.elements)
        {
            if (element.usage == input.semanticUsage && element.usageIndex == input.semanticIndex)
            {
                attribute.binding = element.stream;
                attribute.format = DeclFormat(element.type);
                attribute.offset = element.offset;
                break;
            }
        }
        bindingMask |= 1u << attribute.binding;
        attributes.push_back(attribute);
    }
    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t binding = 0; binding <= kZeroBinding; ++binding)
    {
        if (bindingMask & (1u << binding))
        {
            // The stride is dynamic. The zero binding steps per instance, so
            // every vertex reads its first element.
            bindings.push_back({binding, 16, binding == kZeroBinding ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                      : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = key.topologyClass == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        : key.topologyClass == 1              ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = static_cast<VkPolygonMode>(key.polygonMode);
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blends[kMaxRenderTargets]{};
    for (uint32_t i = 0; i < key.colorCount; ++i)
    {
        VkPipelineColorBlendAttachmentState &blend = blends[i];
        blend.blendEnable = key.blendEnable;
        blend.srcColorBlendFactor = static_cast<VkBlendFactor>(key.srcColor);
        blend.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dstColor);
        blend.colorBlendOp = static_cast<VkBlendOp>(key.colorOp);
        blend.srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.srcAlpha);
        blend.dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.dstAlpha);
        blend.alphaBlendOp = static_cast<VkBlendOp>(key.alphaOp);
        blend.colorWriteMask = key.writeMask[i];
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = key.colorCount;
    blend.pAttachments = blends;
    static const VkDynamicState kDynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(sizeof(kDynamic) / sizeof(kDynamic[0]));
    dynamic.pDynamicStates = kDynamic;

    VkFormat colorFormats[kMaxRenderTargets];
    for (uint32_t i = 0; i < key.colorCount; ++i)
        colorFormats[i] = static_cast<VkFormat>(key.colorFormats[i]);
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = key.colorCount;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = static_cast<VkFormat>(key.depthFormat);
    rendering.stencilAttachmentFormat = static_cast<VkFormat>(key.depthFormat); // the core's formats have stencil

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
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout->pipeline;
    Pipeline pipeline;
    pipeline.pipeline = CreateGraphicsPipeline(device.core, info);
    if (!pipeline.pipeline)
    {
        Fail(device, "cannot build the pipeline for shaders %08x/%08x: %s", vs.translated.bytecodeHash,
             ps.translated.bytecodeHash, LastError(device.core));
        return nullptr;
    }
    pipeline.layout = layout;
    pipeline.bindings = bindingMask;
    ++device.stats.pipelines;
    if (device.frameOpen)
        ++device.stats.pipelinesInFrame;
    return &device.pipelines.emplace(key, pipeline).first->second;
}

PipelineKey MakeKey(const Device &device, const Shader &vs, const Shader &ps, const VertexDecl &decl,
                    d3d::PrimitiveType type)
{
    const uint32_t *rs = device.renderStates;
    PipelineKey key{};
    key.vs = vs.id;
    key.ps = ps.id;
    key.decl = decl.id;
    key.colorCount = static_cast<uint8_t>(ColorCount(device.passTargets));
    for (uint32_t i = 0; i < key.colorCount; ++i)
        key.colorFormats[i] = static_cast<uint32_t>(device.passTargets[i]->texture->vkFormat);
    key.depthFormat = device.passDepth ? static_cast<uint32_t>(device.depthFormat) : 0;
    key.topologyClass = TopologyClass(type);
    const bool nonSolid = Features(device.core).fillModeNonSolid;
    key.polygonMode = static_cast<uint8_t>(!nonSolid ? VK_POLYGON_MODE_FILL
                                           : rs[d3d::RS_FILLMODE] == d3d::FILL_POINT     ? VK_POLYGON_MODE_POINT
                                           : rs[d3d::RS_FILLMODE] == d3d::FILL_WIREFRAME ? VK_POLYGON_MODE_LINE
                                                                                         : VK_POLYGON_MODE_FILL);
    key.blendEnable = rs[d3d::RS_ALPHABLENDENABLE] ? 1 : 0;
    if (key.blendEnable && key.colorCount)
    {
        const bool noAlpha = device.passTargets[0]->texture->format->noAlpha;
        BlendPair(rs[d3d::RS_SRCBLEND], rs[d3d::RS_DESTBLEND], false, noAlpha, key.srcColor, key.dstColor);
        key.colorOp = static_cast<uint8_t>(BlendOperation(rs[d3d::RS_BLENDOP]));
        if (rs[d3d::RS_SEPARATEALPHABLENDENABLE])
        {
            BlendPair(rs[d3d::RS_SRCBLENDALPHA], rs[d3d::RS_DESTBLENDALPHA], true, noAlpha, key.srcAlpha,
                      key.dstAlpha);
            key.alphaOp = static_cast<uint8_t>(BlendOperation(rs[d3d::RS_BLENDOPALPHA]));
        }
        else
        {
            BlendPair(rs[d3d::RS_SRCBLEND], rs[d3d::RS_DESTBLEND], true, noAlpha, key.srcAlpha, key.dstAlpha);
            key.alphaOp = key.colorOp;
        }
    }
    else
    {
        key.blendEnable = 0;
    }
    static const d3d::RenderState kWriteMasks[kMaxRenderTargets] = {
        d3d::RS_COLORWRITEENABLE, d3d::RS_COLORWRITEENABLE1, d3d::RS_COLORWRITEENABLE2, d3d::RS_COLORWRITEENABLE3};
    const bool independent = Features(device.core).independentBlend;
    for (uint32_t i = 0; i < key.colorCount; ++i)
        key.writeMask[i] = static_cast<uint8_t>(rs[independent ? kWriteMasks[i] : d3d::RS_COLORWRITEENABLE] & 0xf);
    return key;
}

// ---- Per-draw state ---------------------------------------------------------------------

VkSampler SamplerFor(Device &device, uint32_t slot, const Texture *texture)
{
    if (device.samplers[slot])
        return device.samplers[slot];
    const uint32_t *ss = device.samplerStates[slot];
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = ss[d3d::SAMP_MAGFILTER] > d3d::TEXF_POINT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = ss[d3d::SAMP_MINFILTER] > d3d::TEXF_POINT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode =
        ss[d3d::SAMP_MIPFILTER] > d3d::TEXF_POINT ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto address = [](uint32_t mode) {
        switch (mode)
        {
        case d3d::TADDRESS_MIRROR:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case d3d::TADDRESS_CLAMP:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case d3d::TADDRESS_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case d3d::TADDRESS_MIRRORONCE:
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };
    if (texture && texture->desc.kind == TextureKind::Cube)
    {
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    else
    {
        info.addressModeU = address(ss[d3d::SAMP_ADDRESSU]);
        info.addressModeV = address(ss[d3d::SAMP_ADDRESSV]);
        info.addressModeW = address(ss[d3d::SAMP_ADDRESSW]);
    }
    // Vulkan has three border colours without an extension; take the nearest.
    const uint32_t border = ss[d3d::SAMP_BORDERCOLOR];
    info.borderColor = (border >> 24) < 0x80 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
        : (border & 0xffffff) < 0x808080     ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                             : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (texture && texture->format->depth)
    {
        info.compareEnable = VK_TRUE;
        info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    // As DXVK: a single level gets the range 0-1, no mip filter level 0 only.
    if (!texture || texture->desc.levels <= 1)
    {
        info.maxLod = 1.0f;
    }
    else if (ss[d3d::SAMP_MIPFILTER] != d3d::TEXF_NONE)
    {
        const VkPhysicalDeviceLimits &limits = Limits(device.core);
        if (ss[d3d::SAMP_MINFILTER] == d3d::TEXF_ANISOTROPIC && ss[d3d::SAMP_MAXANISOTROPY] > 1
            && Features(device.core).samplerAnisotropy)
        {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy = std::min(float(ss[d3d::SAMP_MAXANISOTROPY]), limits.maxSamplerAnisotropy);
        }
        info.mipLodBias = std::clamp(AsFloat(ss[d3d::SAMP_MIPMAPLODBIAS]), -limits.maxSamplerLodBias,
                                     limits.maxSamplerLodBias);
        info.minLod = float(ss[d3d::SAMP_MAXMIPLEVEL]);
        info.maxLod = std::max(16.0f, info.minLod);
    }
    device.samplers[slot] = GetSampler(device.core, info);
    return device.samplers[slot];
}

Texture *NullTexture(const Device &device, ls::TextureType type)
{
    switch (type)
    {
    case ls::TextureType::Cube:
        return device.nullTexture[static_cast<int>(TextureKind::Cube)];
    case ls::TextureType::Tex3D:
        return device.nullTexture[static_cast<int>(TextureKind::Volume)];
    default:
        return device.nullTexture[static_cast<int>(TextureKind::Tex2D)];
    }
}

uint32_t TextureTypeValue(const Texture *texture, ls::TextureType declared)
{
    // dxbc-spirv's SamplerStateType: 0 2D, 1 3D, 2 cube.
    const TextureKind kind = texture ? texture->desc.kind
        : declared == ls::TextureType::Cube ? TextureKind::Cube
        : declared == ls::TextureType::Tex3D ? TextureKind::Volume
                                             : TextureKind::Tex2D;
    return kind == TextureKind::Volume ? 1u : kind == TextureKind::Cube ? 2u : 0u;
}

struct DrawTextures
{
    uint32_t mask = 0;              // LayoutKey::samplers bits
    Texture *bound[kSamplerSlots] = {}; // null: nothing usable bound
};

// Slot for a LayoutKey::samplers bit: pixel samplers 0-15, vertex 16-19.
uint32_t SlotForBit(uint32_t bit) { return bit; }

ls::TextureType DeclaredType(const Shader &vs, const Shader &ps, uint32_t bit)
{
    return bit < 16 ? ps.translated.reflection.samplerTypes[bit] : vs.translated.reflection.samplerTypes[bit - 16];
}

bool BindTextures(Device &device, const Shader &vs, const Shader &ps, DrawTextures &textures)
{
    textures.mask = LayoutFor(vs, ps).samplers;
    Image *settle[kSamplerSlots];
    uint32_t settleCount = 0;
    for (uint32_t bit = 0; bit < kSamplerSlots; ++bit)
    {
        if (!(textures.mask & (1u << bit)))
            continue;
        const uint32_t slot = SlotForBit(bit);
        Texture *texture = device.textures[slot];
        if (texture && !texture->image)
        {
            WarnOnce(device, WarnSystemMemTexture, "a SYSTEMMEM texture was bound for drawing; it reads as unbound");
            texture = nullptr;
        }
        if (texture && IsTarget(device, texture))
        {
            WarnOnce(device, WarnFeedback, "texture %s is bound while drawn to; it reads as unbound",
                     texture->name.c_str());
            texture = nullptr;
        }
        textures.bound[slot] = texture;
        Texture *sampled = texture ? texture : NullTexture(device, DeclaredType(vs, ps, bit));
        if (!ImageReadyToSample(sampled->image))
            settle[settleCount++] = sampled->image;
    }
    if (settleCount)
    {
        // A pass cannot record barriers: textures written since (rendered to,
        // uploaded) move to the sampled layout between passes.
        EndPassIfOpen(device);
        for (uint32_t i = 0; i < settleCount; ++i)
            UseImage(device.core, settle[i], ImageUse::Sampled);
    }
    return Usable(device);
}

void WriteUniforms(Device &device, const Shader &vs, const Shader &ps, const DrawTextures &textures,
                   const Layout &layout, VkDescriptorSet set)
{
    struct Block
    {
        uint32_t binding;
        uint32_t offset;
        uint32_t size;
        ls::Stage stage;
        ls::Uniform uniform;
    };
    Block blocks[2 * ls::kUniformCount];
    uint32_t count = 0;
    uint32_t total = 0;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        if (!(layout.key.uniforms & (1u << bit)))
            continue;
        const ls::Stage stage = bit < 16 ? ls::Stage::Vertex : ls::Stage::Pixel;
        const ls::Uniform uniform = static_cast<ls::Uniform>(bit & 15);
        const ls::Reflection &reflection = (stage == ls::Stage::Vertex ? vs : ps).translated.reflection;
        uint32_t size = 0;
        switch (uniform)
        {
        case ls::Uniform::FloatConstants:
            size = 16 * std::max(1u, reflection.floatConstants);
            break;
        case ls::Uniform::IntConstants:
            size = 16 * std::max(1u, reflection.intConstants);
            break;
        case ls::Uniform::BoolConstants:
            size = 4 * std::max(16u, reflection.boolConstants);
            break;
        case ls::Uniform::AlphaTest:
            size = sizeof(ls::AlphaTestBlock);
            break;
        case ls::Uniform::Fog:
            size = sizeof(ls::FogBlock);
            break;
        case ls::Uniform::ClipPlanes:
            size = sizeof(ls::ClipPlaneBlock);
            break;
        case ls::Uniform::PointArgs:
            size = sizeof(ls::PointArgsBlock);
            break;
        case ls::Uniform::SamplerState:
            size = sizeof(ls::SamplerStateBlock) * ls::kSamplerCount;
            break;
        case ls::Uniform::TextureStage:
            size = sizeof(ls::TextureStageBlock) * ls::kSamplerCount;
            break;
        default:
            break;
        }
        blocks[count] = {bit, total, size, stage, uniform};
        total += (size + device.uniformAlignment - 1) / device.uniformAlignment * device.uniformAlignment;
        ++count;
    }
    const Transient memory = AllocateTransient(device.core, total, device.uniformAlignment);
    if (!memory.data)
    {
        Fail(device, "out of per-frame memory for uniforms: %s", LastError(device.core));
        return;
    }
    device.stats.constantBytes += total;
    uint8_t *base = static_cast<uint8_t *>(memory.data);
    std::memset(base, 0, total);
    VkDescriptorBufferInfo infos[2 * ls::kUniformCount];
    VkWriteDescriptorSet writes[2 * ls::kUniformCount];
    for (uint32_t i = 0; i < count; ++i)
    {
        const Block &block = blocks[i];
        uint8_t *data = base + block.offset;
        const bool vertex = block.stage == ls::Stage::Vertex;
        switch (block.uniform)
        {
        case ls::Uniform::FloatConstants:
        {
            const uint32_t registers = std::min(block.size / 16, vertex ? kVsFloatRegisters : kPsFloatRegisters);
            std::memcpy(data, vertex ? &device.vsConstants[0][0] : &device.psConstants[0][0], registers * 16);
            break;
        }
        case ls::Uniform::AlphaTest:
        {
            ls::AlphaTestBlock alpha{};
            alpha.compareOp = device.renderStates[d3d::RS_ALPHATESTENABLE]
                ? static_cast<uint32_t>(CompareOp(device.renderStates[d3d::RS_ALPHAFUNC]))
                : static_cast<uint32_t>(ls::AlphaCompare::Always);
            alpha.alphaRef = device.renderStates[d3d::RS_ALPHAREF] & 0xff;
            std::memcpy(data, &alpha, sizeof(alpha));
            break;
        }
        case ls::Uniform::SamplerState:
        {
            for (uint32_t sampler = 0; sampler < ls::kSamplerCount; ++sampler)
            {
                const uint32_t bit = vertex ? (sampler < kVertexSamplers ? 16 + sampler : UINT32_MAX) : sampler;
                if (bit == UINT32_MAX)
                    break;
                const Texture *texture = textures.bound[SlotForBit(bit)];
                ls::SamplerStateBlock state{};
                state.textureType = TextureTypeValue(texture, DeclaredType(vs, ps, bit));
                state.useDepthCompare = texture && texture->format->depth;
                state.isNull = texture == nullptr;
                state.drefClamp = state.useDepthCompare && device.depthFormat != VK_FORMAT_D24_UNORM_S8_UINT;
                state.drefScale = 1.0f / float((1u << 24) - 1);
                std::memcpy(data + sampler * sizeof(state), &state, sizeof(state));
            }
            break;
        }
        case ls::Uniform::PointArgs:
        {
            // D3D9's defaults (D3DRS_POINTSIZE, _MIN, _MAX); no point sprites.
            const ls::PointArgsBlock point{0, 1.0f, 1.0f, 64.0f};
            std::memcpy(data, &point, sizeof(point));
            break;
        }
        default:
            // Int and bool constants, fog, clip planes and texture stages:
            // not used by the game's shaders, read as zero (off).
            break;
        }
        infos[i] = {memory.buffer, memory.offset + block.offset, block.size};
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = block.binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    device.vk->vkUpdateDescriptorSets(device.dev, count, writes, 0, nullptr);
}

void WriteTextures(Device &device, const Shader &vs, const Shader &ps, const DrawTextures &textures,
                   VkDescriptorSet set)
{
    VkDescriptorImageInfo images[2 * kSamplerSlots];
    VkWriteDescriptorSet writes[2 * kSamplerSlots];
    uint32_t count = 0;
    for (uint32_t bit = 0; bit < kSamplerSlots; ++bit)
    {
        if (!(textures.mask & (1u << bit)))
            continue;
        const uint32_t slot = SlotForBit(bit);
        const Texture *bound = textures.bound[slot];
        const Texture *texture = bound ? bound : NullTexture(device, DeclaredType(vs, ps, bit));
        const ls::Stage stage = bit < 16 ? ls::Stage::Pixel : ls::Stage::Vertex;
        const uint32_t sampler = bit < 16 ? bit : bit - 16;
        images[count] = {VK_NULL_HANDLE, ImageSampleView(texture->image), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
        images[count + 1] = {SamplerFor(device, slot, bound), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        writes[count] = {};
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = set;
        writes[count].dstBinding = ls::ImageBinding(stage, sampler);
        writes[count].descriptorCount = 1;
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[count].pImageInfo = &images[count];
        writes[count + 1] = writes[count];
        writes[count + 1].dstBinding = ls::SamplerBinding(stage, sampler);
        writes[count + 1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[count + 1].pImageInfo = &images[count + 1];
        count += 2;
    }
    device.vk->vkUpdateDescriptorSets(device.dev, count, writes, 0, nullptr);
}

VkRect2D ClampRect(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t width, uint32_t height)
{
    left = std::clamp<int32_t>(left, 0, int32_t(width));
    top = std::clamp<int32_t>(top, 0, int32_t(height));
    right = std::clamp<int32_t>(right, left, int32_t(width));
    bottom = std::clamp<int32_t>(bottom, top, int32_t(height));
    return {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
}

// The viewport, limited by the scissor rect when the scissor test is on, and
// by the pass's targets.
VkRect2D DrawArea(const Device &device)
{
    const Viewport &vp = device.viewport;
    int32_t left = int32_t(vp.x), top = int32_t(vp.y);
    int32_t right = int32_t(vp.x + vp.width), bottom = int32_t(vp.y + vp.height);
    if (device.renderStates[d3d::RS_SCISSORTESTENABLE])
    {
        left = std::max(left, device.scissor.left);
        top = std::max(top, device.scissor.top);
        right = std::min(right, device.scissor.right);
        bottom = std::min(bottom, device.scissor.bottom);
    }
    return ClampRect(left, top, right, bottom, device.passWidth, device.passHeight);
}

void ApplyDynamicState(Device &device, VkCommandBuffer cmd, d3d::PrimitiveType type)
{
    const uint32_t *rs = device.renderStates;
    DynamicState state{};
    const Viewport &vp = device.viewport;
    // As DXVK: flipped (D3D9's clip space has y up), moved by half a pixel
    // (D3D9 samples pixel centres at whole coordinates), and a depth range
    // that is never empty.
    const float zBias = vp.minZ >= 0.5f ? 0.0f : 0.001f;
    state.viewport = {float(vp.x) + 0.5f,
                      float(vp.y + vp.height) + 0.5f,
                      float(vp.width),
                      -float(vp.height),
                      std::clamp(vp.minZ, 0.0f, 1.0f),
                      std::clamp(std::max(vp.maxZ, vp.minZ + zBias), 0.0f, 1.0f)};
    state.scissor = DrawArea(device);
    state.depthBias = AsFloat(rs[d3d::RS_DEPTHBIAS]) * device.depthBiasScale;
    state.depthSlope = AsFloat(rs[d3d::RS_SLOPESCALEDEPTHBIAS]);
    state.depthBiasEnable = state.depthBias != 0.0f || state.depthSlope != 0.0f;
    // D3D9's front faces are clockwise on screen; the flipped viewport keeps
    // the winding, so CULL_CW culls front faces and CULL_CCW back faces.
    state.cullMode = rs[d3d::RS_CULLMODE] == d3d::CULL_CW    ? VK_CULL_MODE_FRONT_BIT
        : rs[d3d::RS_CULLMODE] == d3d::CULL_CCW ? VK_CULL_MODE_BACK_BIT
                                                : VK_CULL_MODE_NONE;
    state.topology = Topology(type);
    const bool hasDepth = device.passDepth != nullptr;
    state.depthTest = hasDepth && rs[d3d::RS_ZENABLE];
    state.depthWrite = state.depthTest && rs[d3d::RS_ZWRITEENABLE];
    state.depthCompare = CompareOp(rs[d3d::RS_ZFUNC]);
    state.stencilTest = hasDepth && rs[d3d::RS_STENCILENABLE];
    state.frontFail = StencilOp(rs[d3d::RS_STENCILFAIL]);
    state.frontPass = StencilOp(rs[d3d::RS_STENCILPASS]);
    state.frontDepthFail = StencilOp(rs[d3d::RS_STENCILZFAIL]);
    state.frontCompare = CompareOp(rs[d3d::RS_STENCILFUNC]);
    const bool twoSided = rs[d3d::RS_TWOSIDEDSTENCILMODE] != 0;
    // The fields are plain integers; GCC's -Wextra refuses an enum and an
    // integer in one conditional.
    state.backFail = twoSided ? uint32_t(StencilOp(rs[d3d::RS_CCW_STENCILFAIL])) : state.frontFail;
    state.backPass = twoSided ? uint32_t(StencilOp(rs[d3d::RS_CCW_STENCILPASS])) : state.frontPass;
    state.backDepthFail = twoSided ? uint32_t(StencilOp(rs[d3d::RS_CCW_STENCILZFAIL])) : state.frontDepthFail;
    state.backCompare = twoSided ? uint32_t(CompareOp(rs[d3d::RS_CCW_STENCILFUNC])) : state.frontCompare;
    state.stencilRef = rs[d3d::RS_STENCILREF] & 0xff;
    state.stencilMask = rs[d3d::RS_STENCILMASK] & 0xff;
    state.stencilWriteMask = rs[d3d::RS_STENCILWRITEMASK] & 0xff;
    UnpackColor(rs[d3d::RS_BLENDFACTOR], state.blendConstants);

    if (device.dynamicValid && state == device.dynamic)
        return;
    const Dispatch &vk = *device.vk;
    vk.vkCmdSetViewport(cmd, 0, 1, &state.viewport);
    vk.vkCmdSetScissor(cmd, 0, 1, &state.scissor);
    vk.vkCmdSetDepthBias(cmd, state.depthBias, 0.0f, state.depthSlope);
    vk.vkCmdSetDepthBiasEnable(cmd, state.depthBiasEnable);
    vk.vkCmdSetBlendConstants(cmd, state.blendConstants);
    vk.vkCmdSetCullMode(cmd, state.cullMode);
    vk.vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vk.vkCmdSetPrimitiveTopology(cmd, static_cast<VkPrimitiveTopology>(state.topology));
    vk.vkCmdSetDepthTestEnable(cmd, state.depthTest);
    vk.vkCmdSetDepthWriteEnable(cmd, state.depthWrite);
    vk.vkCmdSetDepthCompareOp(cmd, static_cast<VkCompareOp>(state.depthCompare));
    vk.vkCmdSetStencilTestEnable(cmd, state.stencilTest);
    vk.vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, static_cast<VkStencilOp>(state.frontFail),
                         static_cast<VkStencilOp>(state.frontPass), static_cast<VkStencilOp>(state.frontDepthFail),
                         static_cast<VkCompareOp>(state.frontCompare));
    vk.vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, static_cast<VkStencilOp>(state.backFail),
                         static_cast<VkStencilOp>(state.backPass), static_cast<VkStencilOp>(state.backDepthFail),
                         static_cast<VkCompareOp>(state.backCompare));
    vk.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilRef);
    vk.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilMask);
    vk.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilWriteMask);
    device.dynamic = state;
    device.dynamicValid = true;
}

// Everything a draw needs, up to the draw command. False: skip the draw.
bool PrepareDraw(Device &device, d3d::PrimitiveType type)
{
    if (!EnsureFrame(device))
        return false;
    Shader *vs = device.vertexShader;
    Shader *ps = device.pixelShader;
    VertexDecl *decl = device.decl;
    if (!vs || !ps || !decl)
    {
        WarnOnce(device, WarnFixedFunction,
                 "draws need a vertex shader, a pixel shader and a vertex declaration (no fixed function); skipped");
        return false;
    }
    if (!device.viewport.width || !device.viewport.height)
        return false;
    DrawTextures textures;
    if (!BindTextures(device, *vs, *ps, textures))
        return false;
    if (!PassMatches(device))
    {
        EndPassIfOpen(device);
        if (!BeginTargetsPass(device))
            return false;
    }
    const PipelineKey key = MakeKey(device, *vs, *ps, *decl, type);
    auto found = device.pipelines.find(key);
    Pipeline *pipeline = found != device.pipelines.end() ? &found->second : BuildPipeline(device, key, *vs, *ps, *decl);
    if (!pipeline)
        return false;

    // Vertex streams first: an unbound one skips the draw before anything is recorded.
    VkBuffer buffers[kMaxStreams + 1];
    VkDeviceSize offsets[kMaxStreams + 1];
    VkDeviceSize strides[kMaxStreams + 1];
    for (uint32_t binding = 0; binding <= kZeroBinding; ++binding)
    {
        buffers[binding] = BufferHandle(device.zeroVertices);
        offsets[binding] = 0;
        strides[binding] = 16;
        if (binding == kZeroBinding || !(pipeline->bindings & (1u << binding)))
            continue;
        StreamBinding &stream = device.streams[binding];
        if (stream.transient)
        {
            buffers[binding] = stream.transient;
            offsets[binding] = stream.transientOffset;
        }
        else if (stream.buffer)
        {
            buffers[binding] = DrawBuffer(device, *stream.buffer);
            offsets[binding] = stream.offset;
        }
        else
        {
            WarnOnce(device, WarnUnboundStream, "a draw reads vertex stream %u, which has no buffer; skipped",
                     binding);
            return false;
        }
        strides[binding] = stream.stride;
    }

    VkCommandBuffer cmd = CurrentCommands(device);
    const Dispatch &vk = *device.vk;
    if (device.boundPipeline != pipeline->pipeline)
    {
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
        device.boundPipeline = pipeline->pipeline;
    }
    ApplyDynamicState(device, cmd, type);
    for (uint32_t binding = 0; binding <= kZeroBinding; ++binding)
    {
        if (pipeline->bindings & (1u << binding))
            vk.vkCmdBindVertexBuffers2(cmd, binding, 1, &buffers[binding], &offsets[binding], nullptr,
                                       &strides[binding]);
    }

    const Layout &layout = *pipeline->layout;
    if (layout.uniformSet)
    {
        VkDescriptorSet set = AllocateDescriptorSet(device.core, layout.uniformSet);
        if (!set)
            return Fail(device, "cannot allocate a descriptor set: %s", LastError(device.core));
        WriteUniforms(device, *vs, *ps, textures, layout, set);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, ls::kUniformSet, 1, &set, 0,
                                   nullptr);
        ++device.stats.descriptorSets;
    }
    if (layout.textureSet)
    {
        VkDescriptorSet set = AllocateDescriptorSet(device.core, layout.textureSet);
        if (!set)
            return Fail(device, "cannot allocate a descriptor set: %s", LastError(device.core));
        WriteTextures(device, *vs, *ps, textures, set);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, ls::kTextureSet, 1, &set, 0,
                                   nullptr);
        ++device.stats.descriptorSets;
    }
    return Usable(device);
}
} // namespace

size_t PipelineKeyHash::operator()(const PipelineKey &key) const
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&key);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(key); ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return static_cast<size_t>(hash ^ (hash >> 32));
}

int SamplerSlot(uint32_t sampler)
{
    if (sampler < ls::kSamplerCount)
        return static_cast<int>(sampler);
    if (sampler >= kVertexSamplerBase && sampler < kVertexSamplerBase + kVertexSamplers)
        return static_cast<int>(ls::kSamplerCount + sampler - kVertexSamplerBase);
    return -1;
}

// ---- Frames, passes and object lifetime ---------------------------------------------------

bool EnsureFrame(Device &device)
{
    if (!Usable(device) || device.frameSkipped)
        return false;
    if (device.frameOpen)
        return true;
    switch (BeginFrame(device.core))
    {
    case FrameResult::Ok:
        device.frameOpen = true;
        return true;
    case FrameResult::Skipped:
        device.frameSkipped = true;
        return false;
    default:
        return Fail(device, "BeginFrame: %s", LastError(device.core));
    }
}

void EndPassIfOpen(Device &device)
{
    if (!device.passOpen)
        return;
    if (InPass(device.core))
        EndPass(device.core);
    device.passOpen = false;
    std::memset(device.passTargets, 0, sizeof(device.passTargets));
    device.passDepth = nullptr;
}

void ForgetTexture(Device &device, Texture *texture)
{
    if (!texture)
        return;
    for (uint32_t slot = 0; slot < kSamplerSlots; ++slot)
    {
        if (device.textures[slot] == texture)
        {
            device.textures[slot] = nullptr;
            device.samplers[slot] = VK_NULL_HANDLE;
        }
    }
    bool inPass = UsesTexture(device.passDepth, texture);
    for (const Surface *target : device.passTargets)
        inPass = inPass || UsesTexture(target, texture);
    if (inPass)
        EndPassIfOpen(device);
    for (Surface *&target : device.renderTargets)
    {
        if (UsesTexture(target, texture))
            target = nullptr;
    }
    if (UsesTexture(device.depthStencil, texture))
        device.depthStencil = nullptr;
}

void ForgetPipelines(Device &device, uint32_t id)
{
    for (auto it = device.pipelines.begin(); it != device.pipelines.end();)
    {
        if (it->first.vs == id || it->first.ps == id || it->first.decl == id)
        {
            if (device.boundPipeline == it->second.pipeline)
                device.boundPipeline = VK_NULL_HANDLE;
            DestroyPipeline(device.core, it->second.pipeline);
            it = device.pipelines.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool CreatePipelineObjects(Device &device)
{
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    if (device.vk->vkCreateDescriptorSetLayout(device.dev, &info, nullptr, &device.emptySet) != VK_SUCCESS)
        return Fail(device, "vkCreateDescriptorSetLayout failed (empty set)");
    return true;
}

void DestroyPipelineObjects(Device &device)
{
    for (const auto &entry : device.pipelines)
        DestroyPipeline(device.core, entry.second.pipeline);
    device.pipelines.clear();
    // Layouts are not used by the GPU after recording.
    for (const auto &entry : device.layouts)
    {
        const Layout &layout = *entry.second;
        device.vk->vkDestroyPipelineLayout(device.dev, layout.pipeline, nullptr);
        if (layout.uniformSet)
            device.vk->vkDestroyDescriptorSetLayout(device.dev, layout.uniformSet, nullptr);
        if (layout.textureSet)
            device.vk->vkDestroyDescriptorSetLayout(device.dev, layout.textureSet, nullptr);
    }
    device.layouts.clear();
    if (device.emptySet)
        device.vk->vkDestroyDescriptorSetLayout(device.dev, device.emptySet, nullptr);
    device.emptySet = VK_NULL_HANDLE;
}

void ResetState(Device &device)
{
    for (Surface *&target : device.renderTargets)
        target = nullptr;
    device.depthStencil = nullptr;
    SetRenderTarget(&device, 0, &device.backBufferSurface);
}

// ---- State ----------------------------------------------------------------------------------

void SetRenderState(Device *device, d3d::RenderState state, uint32_t value)
{
    if (state < d3d::RS_COUNT)
        device->renderStates[state] = value;
}

uint32_t GetRenderState(const Device *device, d3d::RenderState state)
{
    return state < d3d::RS_COUNT ? device->renderStates[state] : 0;
}

void SetSamplerState(Device *device, uint32_t sampler, d3d::SamplerState state, uint32_t value)
{
    const int slot = SamplerSlot(sampler);
    if (slot < 0 || state >= d3d::SAMP_COUNT || device->samplerStates[slot][state] == value)
        return;
    device->samplerStates[slot][state] = value;
    device->samplers[slot] = VK_NULL_HANDLE;
}

void SetTexture(Device *device, uint32_t sampler, Texture *texture)
{
    const int slot = SamplerSlot(sampler);
    if (slot < 0 || device->textures[slot] == texture)
        return;
    device->textures[slot] = texture;
    device->samplers[slot] = VK_NULL_HANDLE; // depends on the texture's kind, levels and format
}

void SetVertexShader(Device *device, Shader *shader)
{
    if (shader && shader->stage != ls::Stage::Vertex)
    {
        Log(*device, "SetVertexShader: shader %08x is a pixel shader", shader->translated.bytecodeHash);
        return;
    }
    device->vertexShader = shader;
}

void SetPixelShader(Device *device, Shader *shader)
{
    if (shader && shader->stage != ls::Stage::Pixel)
    {
        Log(*device, "SetPixelShader: shader %08x is a vertex shader", shader->translated.bytecodeHash);
        return;
    }
    device->pixelShader = shader;
}

void SetVertexShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (start >= kVsFloatRegisters)
        return;
    count = std::min(count, kVsFloatRegisters - start);
    std::memcpy(device->vsConstants[start], data, count * 4 * sizeof(float));
}

void SetPixelShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (start >= kPsFloatRegisters)
        return;
    count = std::min(count, kPsFloatRegisters - start);
    std::memcpy(device->psConstants[start], data, count * 4 * sizeof(float));
}

void SetVertexDecl(Device *device, VertexDecl *decl) { device->decl = decl; }

void SetStreamSource(Device *device, uint32_t stream, VertexBuffer *buffer, uint32_t offset, uint32_t stride)
{
    if (stream >= kMaxStreams)
        return;
    device->streams[stream] = StreamBinding{};
    device->streams[stream].buffer = buffer;
    device->streams[stream].offset = offset;
    device->streams[stream].stride = stride;
}

void SetIndices(Device *device, IndexBuffer *buffer) { device->indices = buffer; }

void SetViewport(Device *device, const Viewport &viewport) { device->viewport = viewport; }

void SetScissorRect(Device *device, const Rect &rect) { device->scissor = rect; }

void SetRenderTarget(Device *device, uint32_t index, Surface *surface)
{
    if (index >= kMaxRenderTargets)
        return;
    if (surface
        && (!surface->texture->image || surface->texture->format->depth
            || !(surface->texture->desc.usage & d3d::USAGE_RENDERTARGET)))
    {
        Log(*device, "SetRenderTarget: %s is not a render target", surface->texture->name.c_str());
        return;
    }
    device->renderTargets[index] = surface;
    if (index == 0 && surface)
    {
        const uint32_t width = LevelSize(surface->texture->desc.width, surface->level);
        const uint32_t height = LevelSize(surface->texture->desc.height, surface->level);
        device->viewport = {0, 0, width, height, 0.0f, 1.0f};
        device->scissor = {0, 0, int32_t(width), int32_t(height)};
    }
}

void SetDepthStencilSurface(Device *device, Surface *surface)
{
    if (surface && (!surface->texture->image || !surface->texture->format->depth))
    {
        Log(*device, "SetDepthStencilSurface: %s is not a depth surface", surface->texture->name.c_str());
        return;
    }
    device->depthStencil = surface;
}

// ---- Drawing ----------------------------------------------------------------------------------

void Clear(Device *device_, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
           uint32_t stencil)
{
    Device &device = *device_;
    if (!EnsureFrame(device))
        return;
    if (!PassMatches(device))
    {
        EndPassIfOpen(device);
        if (!BeginTargetsPass(device))
            return;
    }
    VkClearAttachment attachments[kMaxRenderTargets + 1];
    uint32_t count = 0;
    if (flags & d3d::CLEAR_TARGET)
    {
        const uint32_t colors = ColorCount(device.passTargets);
        for (uint32_t i = 0; i < colors; ++i)
        {
            attachments[count] = {};
            attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[count].colorAttachment = i;
            UnpackColor(color, attachments[count].clearValue.color.float32);
            ++count;
        }
    }
    VkImageAspectFlags depthAspects = 0;
    if (device.passDepth && (flags & d3d::CLEAR_ZBUFFER))
        depthAspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (device.passDepth && (flags & d3d::CLEAR_STENCIL))
        depthAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (depthAspects)
    {
        attachments[count] = {};
        attachments[count].aspectMask = depthAspects;
        attachments[count].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xff};
        ++count;
    }
    if (!count)
        return;

    const VkRect2D area = DrawArea(device);
    std::vector<VkClearRect> clearRects;
    if (!rectCount || !rects)
    {
        clearRects.push_back({area, 0, 1});
    }
    else
    {
        for (uint32_t i = 0; i < rectCount; ++i)
        {
            const int32_t right = area.offset.x + int32_t(area.extent.width);
            const int32_t bottom = area.offset.y + int32_t(area.extent.height);
            const VkRect2D rect = ClampRect(std::max(rects[i].left, area.offset.x), std::max(rects[i].top, area.offset.y),
                                            std::min(rects[i].right, right), std::min(rects[i].bottom, bottom),
                                            device.passWidth, device.passHeight);
            if (rect.extent.width && rect.extent.height)
                clearRects.push_back({rect, 0, 1});
        }
    }
    clearRects.erase(std::remove_if(clearRects.begin(), clearRects.end(),
                                    [](const VkClearRect &r) { return !r.rect.extent.width || !r.rect.extent.height; }),
                     clearRects.end());
    if (clearRects.empty())
        return;
    device.vk->vkCmdClearAttachments(CurrentCommands(device), count, attachments,
                                     static_cast<uint32_t>(clearRects.size()), clearRects.data());
    ++device.stats.clears;
}

void DrawIndexedPrimitive(Device *device, d3d::PrimitiveType type, int32_t baseVertex, uint32_t minIndex,
                          uint32_t vertexCount, uint32_t startIndex, uint32_t primitiveCount)
{
    (void)minIndex;
    (void)vertexCount;
    const uint32_t indexCount = VertexCount(type, primitiveCount);
    if (!indexCount || !device->indices)
        return;
    if (!PrepareDraw(*device, type))
        return;
    VkCommandBuffer cmd = CurrentCommands(*device);
    device->vk->vkCmdBindIndexBuffer(cmd, DrawBuffer(*device, *device->indices), 0, device->indices->indexType);
    device->vk->vkCmdDrawIndexed(cmd, indexCount, 1, startIndex, baseVertex, 0);
    ++device->stats.draws;
}

void DrawPrimitive(Device *device, d3d::PrimitiveType type, uint32_t startVertex, uint32_t primitiveCount)
{
    const uint32_t count = VertexCount(type, primitiveCount);
    if (!count || !PrepareDraw(*device, type))
        return;
    device->vk->vkCmdDraw(CurrentCommands(*device), count, 1, startVertex, 0);
    ++device->stats.draws;
}

void DrawPrimitiveUP(Device *device, d3d::PrimitiveType type, uint32_t primitiveCount, const void *vertices,
                     uint32_t stride)
{
    const uint32_t count = VertexCount(type, primitiveCount);
    if (count && stride && vertices && EnsureFrame(*device))
    {
        const VkDeviceSize bytes = VkDeviceSize(count) * stride;
        const Transient memory = AllocateTransient(device->core, bytes, 16);
        if (!memory.data)
        {
            Fail(*device, "out of per-frame memory for DrawPrimitiveUP: %s", LastError(device->core));
            return;
        }
        std::memcpy(memory.data, vertices, static_cast<size_t>(bytes));
        StreamBinding &stream = device->streams[0];
        stream = StreamBinding{};
        stream.transient = memory.buffer;
        stream.transientOffset = memory.offset;
        stream.stride = stride;
        if (PrepareDraw(*device, type))
        {
            device->vk->vkCmdDraw(CurrentCommands(*device), count, 1, 0, 0);
            ++device->stats.draws;
        }
    }
    // As D3D9: stream 0 is unbound afterwards.
    device->streams[0] = StreamBinding{};
}

// ---- Copies -------------------------------------------------------------------------------------

void StretchRect(Device *device_, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear)
{
    Device &device = *device_;
    if (!Usable(device) || !src || !dst)
        return;
    Texture *from = src->texture;
    Texture *to = dst->texture;
    if (!from->image || !to->image || from->image == to->image || from->format->depth != to->format->depth)
    {
        WarnOnce(device, WarnStretchRect, "StretchRect from %s to %s is not supported", from->name.c_str(),
                 to->name.c_str());
        return;
    }
    EndPassIfOpen(device);
    auto box = [](const Surface &surface, const Rect *rect, VkOffset3D out[2]) {
        const uint32_t width = LevelSize(surface.texture->desc.width, surface.level);
        const uint32_t height = LevelSize(surface.texture->desc.height, surface.level);
        const VkRect2D clamped = rect ? ClampRect(rect->left, rect->top, rect->right, rect->bottom, width, height)
                                      : VkRect2D{{0, 0}, {width, height}};
        out[0] = {clamped.offset.x, clamped.offset.y, 0};
        out[1] = {clamped.offset.x + int32_t(clamped.extent.width), clamped.offset.y + int32_t(clamped.extent.height), 1};
    };
    VkOffset3D srcBox[2];
    VkOffset3D dstBox[2];
    box(*src, srcRect, srcBox);
    box(*dst, dstRect, dstBox);
    if (srcBox[0].x == srcBox[1].x || srcBox[0].y == srcBox[1].y || dstBox[0].x == dstBox[1].x
        || dstBox[0].y == dstBox[1].y)
        return;
    const VkFilter filter = linear && !from->format->depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    BlitImage(device.core, from->image, srcBox, to->image, dstBox, filter, src->level, src->face, dst->level,
              dst->face);
    Usable(device);
}

bool GetRenderTargetData(Device *device_, Surface *renderTarget, Surface *dst)
{
    Device &device = *device_;
    if (!Usable(device) || !renderTarget || !dst)
        return false;
    Texture *from = renderTarget->texture;
    Texture *to = dst->texture;
    uint32_t width, height, dstWidth, dstHeight;
    d3d::Format format, dstFormat;
    SurfaceSize(renderTarget, width, height, format);
    SurfaceSize(dst, dstWidth, dstHeight, dstFormat);
    if (!from->image || to->image || width != dstWidth || height != dstHeight || from->vkFormat != to->vkFormat
        || from->format->blockBytes != 4 || from->format->depth)
    {
        Log(device, "GetRenderTargetData: %s to %s needs a 4-byte render target and a SYSTEMMEM surface of its size",
            from->name.c_str(), to->name.c_str());
        return false;
    }
    EndPassIfOpen(device);
    Readback *readback = ReadImage(device.core, from->image, renderTarget->level, renderTarget->face);
    ReadbackData data{};
    if (!readback || !WaitReadback(device.core, readback, &data))
    {
        DestroyReadback(device.core, readback);
        return Fail(device, "GetRenderTargetData: %s", LastError(device.core));
    }
    Subresource &sub = to->subresources[dst->face * to->desc.levels + dst->level];
    sub.cpu.assign(data.pixels, data.pixels + size_t(width) * height * 4);
    DestroyReadback(device.core, readback);
    return true;
}

void UpdateTexture(Device *device_, Texture *src, Texture *dst)
{
    Device &device = *device_;
    if (!Usable(device) || !src || !dst)
        return;
    if (src->image || !dst->image || src->desc.kind != dst->desc.kind || src->vkFormat != dst->vkFormat
        || src->desc.width != dst->desc.width || src->desc.height != dst->desc.height
        || src->desc.depth != dst->desc.depth)
    {
        Log(device, "UpdateTexture: %s to %s needs a SYSTEMMEM texture of the same kind, size and format",
            src->name.c_str(), dst->name.c_str());
        return;
    }
    const uint32_t levels = std::min(src->desc.levels, dst->desc.levels);
    for (uint32_t face = 0; face < dst->faces; ++face)
    {
        for (uint32_t level = 0; level < levels; ++level)
        {
            const Subresource &from = src->subresources[face * src->desc.levels + level];
            if (from.cpu.empty())
                continue; // never written
            LockedRect locked;
            if (!LockRect(device_, dst, face, level, nullptr, 0, locked))
                return;
            Subresource &to = dst->subresources[face * dst->desc.levels + level];
            std::memcpy(to.cpu.data(), from.cpu.data(), std::min(to.cpu.size(), from.cpu.size()));
            UnlockRect(device_, dst, face, level);
        }
    }
}

// ---- Frames and queries ----------------------------------------------------------------------

PresentResult Present(Device *device_)
{
    Device &device = *device_;
    if (!Usable(device))
        return PresentResult::Failed;
    if (!device.frameOpen && !EnsureFrame(device))
    {
        const bool skipped = device.frameSkipped;
        device.frameSkipped = false;
        return skipped ? PresentResult::Skipped : PresentResult::Failed;
    }
    EndPassIfOpen(device);
    Image *swapchain = SwapchainImage(device.core);
    const ImageDesc &shown = ImageInfo(swapchain);
    const TextureDesc &back = device.backBuffer->desc;
    const VkOffset3D src[2] = {{0, 0, 0}, {int32_t(back.width), int32_t(back.height), 1}};
    const VkOffset3D dst[2] = {{0, 0, 0}, {int32_t(shown.width), int32_t(shown.height), 1}};
    const bool scaled = shown.width != back.width || shown.height != back.height;
    BlitImage(device.core, device.backBuffer->image, src, swapchain, dst, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    const FrameResult result = gfx_vk::Present(device.core);
    device.frameOpen = false;
    device.stateCommands = VK_NULL_HANDLE;
    device.boundPipeline = VK_NULL_HANDLE;
    device.dynamicValid = false;
    switch (result)
    {
    case FrameResult::Ok:
        return PresentResult::Ok;
    case FrameResult::Skipped:
        return PresentResult::Skipped;
    default:
        Fail(device, "Present: %s", LastError(device.core));
        return PresentResult::Failed;
    }
}

Query *CreateQuery(Device *device, QueryKind kind)
{
    if (!Usable(*device))
        return nullptr;
    Query *query = new Query();
    query->kind = kind;
    return query;
}

void DestroyQuery(Device *device, Query *query)
{
    (void)device;
    delete query;
}

void IssueQuery(Device *device, Query *query, bool begin)
{
    if (query && !begin)
        query->serial = SubmissionSerial(device->core);
}

bool GetQueryData(Device *device_, Query *query, bool flush, uint32_t &result)
{
    Device &device = *device_;
    // Occlusion queries are not counted yet: every sample passed.
    const uint32_t done = query && query->kind == QueryKind::Occlusion ? 0xffffffu : 1u;
    result = 0;
    if (!query || !Usable(device))
        return true; // a lost device reports done, as D3D9's does
    if (!query->serial || SerialFinished(device.core, query->serial))
    {
        result = done;
        return true;
    }
    if (flush && query->serial == SubmissionSerial(device.core))
    {
        EndPassIfOpen(device);
        if (!Flush(device.core, false))
            Fail(device, "Flush: %s", LastError(device.core));
    }
    return false;
}
} // namespace gfx_vk::legacy
