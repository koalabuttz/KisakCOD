// Vulkan Legacy adapter: state, pipelines, passes, draws, clears, copies,
// present and queries. See vk_legacy.h.

#include "vk_legacy_internal.h"

#include <cmath>

namespace gfx_vk::legacy
{
static_assert(sizeof(PipelineKey) == 56 + 2 * kMaxStreams, "PipelineKey must have no padding: it is hashed and compared as bytes");

namespace
{
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

// The size a pass of the bound targets has: the smallest of them.
void TargetsSize(const Device &device, uint32_t &width, uint32_t &height)
{
    width = UINT32_MAX;
    height = UINT32_MAX;
    const auto fit = [&](const Surface *surface) {
        width = std::min(width, LevelSize(surface->texture->desc.width, surface->level));
        height = std::min(height, LevelSize(surface->texture->desc.height, surface->level));
    };
    for (uint32_t i = 0; i < ColorCount(device.renderTargets); ++i)
        fit(device.renderTargets[i]);
    if (device.depthStencil)
        fit(device.depthStencil);
}

// loads: clears the pass begins with (none: every target is loaded).
bool BeginTargetsPass(Device &device, const Device::PassLoads &loads = {})
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
    // Every target of a pass has the same samples (as in D3D9, where a
    // multisampled render target needs a depth surface of its count).
    const uint32_t samples = colors ? device.renderTargets[0]->texture->samples : device.depthStencil->texture->samples;
    bool mismatch = device.depthStencil && device.depthStencil->texture->samples != samples;
    for (uint32_t i = 0; i < colors; ++i)
        mismatch = mismatch || device.renderTargets[i]->texture->samples != samples;
    if (mismatch)
    {
        WarnOnce(device, WarnSampleMismatch,
                 "a draw or clear whose render targets and depth surface have different samples was skipped");
        return false;
    }
    PassDesc pass;
    pass.name = "legacy";
    pass.colorCount = colors;
    uint32_t width = 0, height = 0;
    TargetsSize(device, width, height);
    for (uint32_t i = 0; i < colors; ++i)
    {
        pass.color[i].image = device.renderTargets[i]->texture->image;
        if (loads.clearColor)
        {
            pass.color[i].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            std::memcpy(pass.color[i].clear, loads.color, sizeof(loads.color));
        }
    }
    if (device.depthStencil)
    {
        pass.depth.image = device.depthStencil->texture->image;
        if (loads.clearDepth)
        {
            pass.depth.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            pass.depth.clearDepth = loads.depth;
            pass.depth.clearStencil = loads.stencil;
        }
    }
    pass.area = {{0, 0}, {width, height}};
    if (!BeginPass(device.core, pass))
        return Fail(device, "BeginPass: %s", LastError(device.core));
    device.passOpen = true;
    device.passEmpty = true;
    device.passLoads = loads;
    std::memcpy(device.passTargets, device.renderTargets, sizeof(device.passTargets));
    device.passDepth = device.depthStencil;
    device.passWidth = width;
    device.passHeight = height;
    device.passSamples = samples;
    device.keyStale = true;
    device.dynamicStale = true;
    ++device.stats.passes;
    return true;
}

// Starts recording if needed. The bound pipeline and dynamic state belong to
// the command buffer being recorded: a new one starts without them.
bool SyncRecording(Device &device)
{
    const uint64_t recording = Recording(device.core);
    if (!recording)
        return false;
    if (recording != device.stateRecording)
    {
        ForgetRecordingState(device);
        device.stateRecording = recording;
    }
    return true;
}

uint32_t BitCount(uint32_t bits)
{
    uint32_t count = 0;
    for (; bits; bits &= bits - 1)
        ++count;
    return count;
}

} // namespace

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
    // Dynamic uniform buffers let a draw reuse a set with new offsets. Some
    // devices allow as few as 8 in a pipeline layout; past that, plain ones.
    const uint32_t uniformCount = BitCount(key.uniforms);
    layout->dynamicUniforms =
        device.dynamicUniforms && uniformCount <= Limits(device.core).maxDescriptorSetUniformBuffersDynamic;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        if (key.uniforms & (1u << bit))
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = bit;
            binding.descriptorType =
                layout->dynamicUniforms ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
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

VkShaderModule FindPixelModule(Device &device, Shader &ps, const Shader &vs, uint32_t depthSamplers,
                               uint32_t alphaCompare, PixelModuleRecipe &recipe)
{
    recipe = PixelModuleRecipe{};
    recipe.direct = ls::StagesLinkDirectly(vs.translated, ps.translated);
    const bool fixed = Features(device.core).fixedDepthCompare;
    if (recipe.direct && !fixed && !alphaCompare)
    {
        recipe.plain = true;
        return ps.module;
    }
    // The vertex shader's id (none when the stages link as they stand), the
    // depth samplers above it where they are fixed, and the alpha test's
    // compare plus 1 above those where it is fixed (PipelineKey::alphaCompare).
    recipe.moduleKey = uint64_t(alphaCompare & 0xf) << 52
        | (fixed ? uint64_t(depthSamplers | 0x10000u) << 32 : 0) | (recipe.direct ? 0xffffffffu : vs.pipelineId);
    // Where depth compare is fixed, so is the reference's clamp
    // (SamplerStateBlock::drefClamp, as FillUniformBlock writes it).
    recipe.fixDepth = fixed;
    recipe.depthCompare =
        depthSamplers | (device.depthFormat != VK_FORMAT_D24_UNORM_S8_UINT ? ls::kFixedDepthClamp : 0u);
    recipe.fixAlpha = alphaCompare != 0;
    recipe.alphaCompare = alphaCompare ? alphaCompare - 1 : 0;
    auto found = ps.linked.find(recipe.moduleKey);
    return found != ps.linked.end() ? found->second : VK_NULL_HANDLE;
}

bool BuildPixelSpirv(const PixelModuleRecipe &recipe, const Shader &ps, const Shader &vs,
                     std::vector<uint32_t> &spirv, std::string &error)
{
    if (recipe.plain)
        return ls::BuildSpirv(ps.translated, nullptr, spirv, error);
    return ls::BuildSpirv(ps.translated, recipe.direct ? nullptr : &vs.translated, spirv, error,
                          recipe.fixDepth ? &recipe.depthCompare : nullptr,
                          recipe.fixAlpha ? &recipe.alphaCompare : nullptr);
}

bool KeepPixelModule(Device &device, Shader &ps, const PixelModuleRecipe &recipe, VkShaderModule module)
{
    if (recipe.plain)
    {
        if (ps.module)
            return false;
        ps.module = module;
        return true;
    }
    if (!ps.linked.emplace(recipe.moduleKey, module).second)
        return false;
    if (!recipe.direct)
        ++device.stats.shaderPairs;
    return true;
}

namespace
{
VkShaderModule PixelModule(Device &device, Shader &ps, Shader &vs, uint32_t depthSamplers, uint32_t alphaCompare)
{
    PixelModuleRecipe recipe;
    if (const VkShaderModule kept = FindPixelModule(device, ps, vs, depthSamplers, alphaCompare, recipe))
        return kept;
    const TimeInto timer(device.stats.pixelModuleMicros);
    ++device.stats.pixelModules;
    std::vector<uint32_t> spirv;
    std::string error;
    if (!BuildPixelSpirv(recipe, ps, vs, spirv, error))
    {
        if (recipe.direct)
            Fail(device, "pixel shader %08x: %s", ps.translated.bytecodeHash, error.c_str());
        else
            Fail(device, "pixel shader %08x with vertex shader %08x: %s", ps.translated.bytecodeHash,
                 vs.translated.bytecodeHash, error.c_str());
        return VK_NULL_HANDLE;
    }
    const VkShaderModule module = CreateShaderModule(device.core, spirv.data(), spirv.size() * sizeof(uint32_t));
    if (module)
        KeepPixelModule(device, ps, recipe, module);
    return module;
}

// The blocks of set 0 a shader pair reads, in binding order, with their sizes.
void ListUniformBlocks(Pipeline &pipeline, const Layout &layout, const Shader &vs, const Shader &ps)
{
    pipeline.blockCount = 0;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        if (!(layout.key.uniforms & (1u << bit)))
            continue;
        const ls::Uniform uniform = static_cast<ls::Uniform>(bit & 15);
        const ls::Reflection &reflection = (bit < 16 ? vs : ps).translated.reflection;
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
        pipeline.blocks[pipeline.blockCount++] = {bit, size};
    }
}

Pipeline *BuildPipeline(Device &device, const PipelineKey &key, Shader &vs, Shader &ps, const VertexDecl &decl)
{
    const TimeInto timer(device.stats.pipelineMicros);
    Layout *layout = GetLayout(device, LayoutFor(vs, ps));
    const VkShaderModule fragment = layout ? PixelModule(device, ps, vs, key.depthSamplers, key.alphaCompare) : VK_NULL_HANDLE;
    if (!fragment)
    {
        if (device.error.empty())
            Fail(device, "cannot build the pixel shader: %s", LastError(device.core));
        return nullptr;
    }
    PipelineInfo info;
    MakePipelineInfo(info, key, vs, fragment, decl, *layout, Features(device.core).dynamicVertexStride);
    const VkPipeline handle = CreateGraphicsPipeline(device.core, info.info);
    if (!handle)
    {
        Fail(device, "cannot build the pipeline for shaders %08x/%08x: %s", vs.translated.bytecodeHash,
             ps.translated.bytecodeHash, LastError(device.core));
        return nullptr;
    }
    ++device.stats.pipelines;
    if (device.frameOpen)
        ++device.stats.pipelinesInFrame;
    return AddPipeline(device, key, handle, layout, info.bindingMask, vs, ps, decl);
}

} // namespace

void MakePipelineInfo(PipelineInfo &out, const PipelineKey &key, const Shader &vs, VkShaderModule fragment,
                      const VertexDecl &decl, const Layout &layout, bool dynamicStride)
{
    out.stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    out.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    out.stages[0].module = vs.module;
    out.stages[0].pName = "main";
    out.stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    out.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    out.stages[1].module = fragment;
    out.stages[1].pName = "main";

    // Each vertex shader input reads the declaration element with its usage
    // and index; inputs the declaration lacks read zero.
    out.bindingMask = 0;
    out.attributes.clear();
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
        out.bindingMask |= 1u << attribute.binding;
        out.attributes.push_back(attribute);
    }
    out.bindings.clear();
    for (uint32_t binding = 0; binding <= kZeroBinding; ++binding)
    {
        if (out.bindingMask & (1u << binding))
        {
            // The stride is dynamic where the device allows it, else the
            // key's. The zero binding steps per instance, so every vertex
            // reads its first element.
            const uint32_t stride = binding == kZeroBinding || dynamicStride ? 16 : key.strides[binding];
            out.bindings.push_back({binding, stride, binding == kZeroBinding ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                              : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    }
    out.vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    out.vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(out.bindings.size());
    out.vertexInput.pVertexBindingDescriptions = out.bindings.data();
    out.vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(out.attributes.size());
    out.vertexInput.pVertexAttributeDescriptions = out.attributes.data();

    out.assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    out.assembly.topology = key.topologyClass == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
        : key.topologyClass == 1                  ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    out.viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    out.viewport.viewportCount = 1;
    out.viewport.scissorCount = 1;
    out.raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    out.raster.polygonMode = static_cast<VkPolygonMode>(key.polygonMode);
    out.raster.lineWidth = 1.0f;
    out.multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    out.multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(key.samples);
    out.depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    for (uint32_t i = 0; i < key.colorCount; ++i)
    {
        VkPipelineColorBlendAttachmentState &blend = out.blends[i];
        blend.blendEnable = key.blendEnable;
        blend.srcColorBlendFactor = static_cast<VkBlendFactor>(key.srcColor);
        blend.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dstColor);
        blend.colorBlendOp = static_cast<VkBlendOp>(key.colorOp);
        blend.srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.srcAlpha);
        blend.dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.dstAlpha);
        blend.alphaBlendOp = static_cast<VkBlendOp>(key.alphaOp);
        blend.colorWriteMask = key.writeMask[i];
    }
    out.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    out.blend.attachmentCount = key.colorCount;
    out.blend.pAttachments = out.blends;
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
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, // last: left out when strides are baked in
    };
    constexpr uint32_t kDynamicCount = static_cast<uint32_t>(sizeof(kDynamic) / sizeof(kDynamic[0]));
    out.dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    out.dynamic.dynamicStateCount = dynamicStride ? kDynamicCount : kDynamicCount - 1;
    out.dynamic.pDynamicStates = kDynamic;

    for (uint32_t i = 0; i < key.colorCount; ++i)
        out.colorFormats[i] = static_cast<VkFormat>(key.colorFormats[i]);
    out.rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    out.rendering.colorAttachmentCount = key.colorCount;
    out.rendering.pColorAttachmentFormats = out.colorFormats;
    out.rendering.depthAttachmentFormat = static_cast<VkFormat>(key.depthFormat);
    out.rendering.stencilAttachmentFormat = static_cast<VkFormat>(key.depthFormat); // the core's formats have stencil

    out.info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    out.info.pNext = &out.rendering;
    out.info.stageCount = 2;
    out.info.pStages = out.stages;
    out.info.pVertexInputState = &out.vertexInput;
    out.info.pInputAssemblyState = &out.assembly;
    out.info.pViewportState = &out.viewport;
    out.info.pRasterizationState = &out.raster;
    out.info.pMultisampleState = &out.multisample;
    out.info.pDepthStencilState = &out.depthStencil;
    out.info.pColorBlendState = &out.blend;
    out.info.pDynamicState = &out.dynamic;
    out.info.layout = layout.pipeline;
}

Pipeline *AddPipeline(Device &device, const PipelineKey &key, VkPipeline handle, Layout *layout,
                      uint32_t bindings, const Shader &vs, const Shader &ps, const VertexDecl &decl)
{
    Pipeline pipeline;
    pipeline.pipeline = handle;
    pipeline.layout = layout;
    pipeline.bindings = bindings;
    pipeline.vsHash = vs.contentHash;
    pipeline.psHash = ps.contentHash;
    pipeline.declHash = decl.contentHash;
    ListUniformBlocks(pipeline, *layout, vs, ps);
    return &device.pipelines.emplace(key, pipeline).first->second;
}

namespace
{
// The alpha test's compare as the render state gives it (ls::AlphaCompare):
// Always while the test is off.
uint32_t AlphaCompareNow(const Device &device)
{
    return device.renderStates[d3d::RS_ALPHATESTENABLE]
        ? static_cast<uint32_t>(CompareOp(device.renderStates[d3d::RS_ALPHAFUNC]))
        : static_cast<uint32_t>(ls::AlphaCompare::Always);
}

PipelineKey MakeKey(const Device &device, const Shader &vs, const Shader &ps, const VertexDecl &decl,
                    d3d::PrimitiveType type, uint32_t depthSamplers)
{
    const uint32_t *rs = device.renderStates;
    PipelineKey key{};
    key.vs = vs.pipelineId;
    key.ps = ps.pipelineId;
    key.decl = decl.pipelineId;
    key.depthSamplers = static_cast<uint16_t>(depthSamplers);
    if (!Features(device.core).dynamicVertexStride)
    {
        for (const VertexElement &element : decl.elements)
            key.strides[element.stream] = static_cast<uint16_t>(device.streams[element.stream].stride);
    }
    key.colorCount = static_cast<uint8_t>(ColorCount(device.passTargets));
    for (uint32_t i = 0; i < key.colorCount; ++i)
        key.colorFormats[i] = static_cast<uint32_t>(device.passTargets[i]->texture->vkFormat);
    key.depthFormat = device.passDepth ? static_cast<uint32_t>(device.depthFormat) : 0;
    key.samples = static_cast<uint8_t>(device.passSamples);
    key.topologyClass = TopologyClass(type);
    if (Features(device.core).fixedAlphaTest
        && (ps.translated.reflection.uniforms & (1u << static_cast<uint32_t>(ls::Uniform::AlphaTest))))
        key.alphaCompare = static_cast<uint8_t>(AlphaCompareNow(device) + 1);
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

// What of a texture its sampler depends on (SamplerFor): a cube, a depth
// format, and one level or more. No texture is like one of a single level.
uint8_t SamplerTraits(const Texture *texture)
{
    if (!texture)
        return 0;
    return static_cast<uint8_t>((texture->desc.kind == TextureKind::Cube ? 1u : 0u)
                                | (texture->format->depth ? 2u : 0u) | (texture->desc.levels > 1 ? 4u : 0u));
}

// The sampler for a slot's states and this texture, from the core's cache.
VkSampler MakeSampler(Device &device, uint32_t slot, const Texture *texture)
{
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
    return GetSampler(device.core, info);
}

// MakeSampler's, kept in the slot until its states change or a texture with
// other traits is bound (Device::verifyShortcuts: made again and compared).
VkSampler SamplerFor(Device &device, uint32_t slot, const Texture *texture)
{
    const uint8_t traits = SamplerTraits(texture);
    if (device.samplers[slot] && device.samplerTraits[slot] == traits)
    {
        if (device.verifyShortcuts && MakeSampler(device, slot, texture) != device.samplers[slot])
            Fail(device, "the sampler kept for slot %u is not the one its states and texture give", slot);
        return device.samplers[slot];
    }
    device.samplers[slot] = MakeSampler(device, slot, texture);
    device.samplerTraits[slot] = traits;
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
    // Four bits for each sampler with a texture: bit 3 set, bit 2 for a depth
    // format, and TextureTypeValue. With the shader they are everything a
    // SamplerState block is built from, so two draws with the same shader and
    // signature get the same block. Pixel samplers 0-15, vertex samplers 0-3.
    uint64_t pixelSignature = 0;
    uint32_t vertexSignature = 0;
};

// The pixel samplers a draw reads that have a depth texture bound, one bit
// each (PipelineKey::depthSamplers).
uint32_t DepthSamplers(const DrawTextures &textures)
{
    uint32_t depth = 0;
    for (uint32_t left = textures.mask & 0xffffu; left; left &= left - 1)
    {
        const uint32_t sampler = static_cast<uint32_t>(std::countr_zero(left));
        if ((textures.pixelSignature >> (4 * sampler)) & 4u)
            depth |= 1u << sampler;
    }
    return depth;
}

// Slot for a LayoutKey::samplers bit: pixel samplers 0-15, vertex 16-19.
uint32_t SlotForBit(uint32_t bit) { return bit; }

// The number of the lowest bit set; bits is not 0.
uint32_t LowestBit(uint32_t bits) { return static_cast<uint32_t>(std::countr_zero(bits)); }

ls::TextureType DeclaredType(const Shader &vs, const Shader &ps, uint32_t bit)
{
    return bit < 16 ? ps.translated.reflection.samplerTypes[bit] : vs.translated.reflection.samplerTypes[bit - 16];
}

bool BindTextures(Device &device, const Shader &vs, const Shader &ps, DrawTextures &textures)
{
    textures.mask = LayoutFor(vs, ps).samplers;
    Image *settle[kSamplerSlots];
    uint32_t settleCount = 0;
    for (uint32_t left = textures.mask; left; left &= left - 1)
    {
        const uint32_t bit = LowestBit(left);
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
        if (texture)
        {
            const uint64_t signature = 8u | (texture->format->depth ? 4u : 0u) | TextureTypeValue(texture, {});
            if (bit < 16)
                textures.pixelSignature |= signature << (4 * bit);
            else
                textures.vertexSignature |= static_cast<uint32_t>(signature) << (4 * (bit - 16));
        }
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

// ---- Uniform blocks (set 0) -----------------------------------------------------------------
//
// A draw's blocks live in per-frame memory. Most draws differ from the one
// before in the vertex shader's constants and nothing else, so each binding
// remembers the block the frame's memory holds for it (Device::uniformSlots)
// and a draw writes only the blocks whose bytes would differ:
//
// - float constants: let go when a register the block holds is set to
//   another value (SetVertexShaderConstantF, SetPixelShaderConstantF);
// - the alpha test and sampler state blocks: compared by what they are built
//   from (UniformKey);
// - the rest are the same for every draw (see FillUniformBlock).
//
// With dynamic uniform buffers a pipeline's set is written once per frame and
// transient buffer and bound with each draw's offsets, so the blocks of a
// draw must be in one buffer; when the frame's memory moves on to another
// one, the first draw after that writes all its blocks again.

// No member initializers: BindUniforms keeps an array of them per draw and
// fills in the ones the pipeline has (KeyForBlock).
struct UniformKey
{
    uint64_t key;
    uint32_t shader;
};

UniformKey KeyForBlock(const Device &device, uint32_t binding, const Shader &vs, const Shader &ps,
                       const DrawTextures &textures)
{
    UniformKey key{0, 0};
    switch (static_cast<ls::Uniform>(binding & 15))
    {
    case ls::Uniform::AlphaTest:
    {
        const uint32_t compare = AlphaCompareNow(device);
        key.key = uint64_t(1) << 63 | uint64_t(compare) << 8 | (device.renderStates[d3d::RS_ALPHAREF] & 0xff);
        break;
    }
    case ls::Uniform::SamplerState:
        // The shader declares the type an unbound sampler reads as.
        key.key = binding < 16 ? textures.vertexSignature : textures.pixelSignature;
        key.shader = (binding < 16 ? vs : ps).id;
        break;
    default:
        break;
    }
    return key;
}

// Writes one block: every byte of it. The fog, clip plane and point blocks
// and the sampler state's projection, gather, unbound and depth scale members
// are constants in shaders translated with the legacy state fixed
// (legacy_shader::SetDynamicLegacyState, FixLegacyState in
// vk_legacy_shader.cpp): change them there too.
void FillUniformBlock(const Device &device, const UniformBlock &block, const UniformKey &key, const Shader &vs,
                      const Shader &ps, const DrawTextures &textures, uint8_t *data)
{
    const bool vertex = block.binding < 16;
    switch (static_cast<ls::Uniform>(block.binding & 15))
    {
    case ls::Uniform::FloatConstants:
    {
        const uint32_t registers = std::min(block.size / 16, vertex ? kVsFloatRegisters : kPsFloatRegisters);
        std::memcpy(data, vertex ? &device.vsConstants[0][0] : &device.psConstants[0][0], registers * 16);
        std::memset(data + registers * 16, 0, block.size - registers * 16);
        break;
    }
    case ls::Uniform::AlphaTest:
    {
        ls::AlphaTestBlock alpha{};
        alpha.compareOp = static_cast<uint32_t>(key.key >> 8) & 0xff;
        alpha.alphaRef = static_cast<uint32_t>(key.key) & 0xff;
        std::memcpy(data, &alpha, sizeof(alpha));
        break;
    }
    case ls::Uniform::SamplerState:
    {
        std::memset(data, 0, block.size);
        for (uint32_t sampler = 0; sampler < (vertex ? kVertexSamplers : ls::kSamplerCount); ++sampler)
        {
            const uint32_t bit = vertex ? 16 + sampler : sampler;
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
        // Int and bool constants, fog, clip planes and texture stages: not
        // used by the game's shaders, read as zero (off).
        std::memset(data, 0, block.size);
        break;
    }
}

// Device::verifyShortcuts: writes the block again, elsewhere, and compares.
bool HeldBlockIsRight(const Device &device, const UniformBlock &block, const UniformKey &key, const Shader &vs,
                      const Shader &ps, const DrawTextures &textures, const UniformSlot &slot)
{
    uint8_t room[16 * kVsFloatRegisters];
    std::vector<uint8_t> more; // a shader that declares more registers than there are
    uint8_t *fresh = room;
    if (block.size > sizeof(room))
    {
        more.resize(block.size);
        fresh = more.data();
    }
    FillUniformBlock(device, block, key, vs, ps, textures, fresh);
    return std::memcmp(fresh, slot.data, block.size) == 0;
}

void RecordUniformBind(Device &device, const UniformBind &bind)
{
    Record(device.core, [bind](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bind.layout, ls::kUniformSet, 1, &bind.set,
                                   bind.offsetCount, bind.offsetCount ? bind.offsets : nullptr);
    });
}

// ---- Lasting descriptor sets ------------------------------------------------------------

constexpr uint32_t kLastingSetLimit = 8192;       // texture and uniform sets together
constexpr uint32_t kLastingTableSlots = 2 * kLastingSetLimit; // a power of two

void RetireLastingSets(Device &device)
{
    if (LastingDescriptorSets(device.core))
        ++device.stats.lastingRetires;
    RetireLastingDescriptorSets(device.core);
    ++device.lastingGeneration;
    device.lastingTextures.entries.clear();
    device.lastingTextures.slots.assign(kLastingTableSlots, 0);
    device.lastingTextures.imagesDestroyed = ImagesDestroyed(device.core);
    // The last set may be one of those given up.
    device.lastTextureSet = Device::TextureSetCache{};
}

// A set from the lasting pools; gives every lasting set up first when there
// are too many.
VkDescriptorSet AllocateLastingSet(Device &device, VkDescriptorSetLayout layout)
{
    if (LastingDescriptorSets(device.core) >= kLastingSetLimit)
        RetireLastingSets(device);
    const VkDescriptorSet set = AllocateLastingDescriptorSet(device.core, layout);
    if (set)
        ++device.stats.lastingSets;
    return set;
}

// Before a draw takes any lasting set: gives them all up once an image has
// been destroyed since they were written.
void CheckLastingSets(Device &device)
{
    Device::LastingTextureSets &table = device.lastingTextures;
    if (table.slots.empty())
    {
        table.slots.assign(kLastingTableSlots, 0);
        table.imagesDestroyed = ImagesDestroyed(device.core);
    }
    else if (table.imagesDestroyed != ImagesDestroyed(device.core))
    {
        RetireLastingSets(device);
    }
}

uint64_t HashTextureSet(const Device::TextureSetCache &contents)
{
    auto mix = [](uint64_t h, uint64_t value) { return (h ^ value) * 0x100000001b3ull; };
    uint64_t h = mix(0xcbf29ce484222325ull, (uint64_t)contents.layout);
    h = mix(h, contents.mask);
    for (uint32_t left = contents.mask; left; left &= left - 1)
    {
        const uint32_t bit = LowestBit(left);
        h = mix(h, (uint64_t)contents.views[bit]);
        h = mix(h, (uint64_t)contents.samplers[bit]);
    }
    return h ^ (h >> 31);
}

// Puts this draw's uniform blocks in per-frame memory and binds set 0.
bool BindUniforms(Device &device, const Shader &vs, const Shader &ps, const DrawTextures &textures,
                  Pipeline &pipeline)
{
    const Layout &layout = *pipeline.layout;
    const bool dynamic = layout.dynamicUniforms;
    const uint32_t count = pipeline.blockCount;
    const uint32_t alignment = device.uniformAlignment;
    auto aligned = [alignment](uint32_t size) { return (size + alignment - 1) / alignment * alignment; };

    // The blocks the frame's memory does not hold as this draw needs them.
    UniformKey keys[kMaxUniformBlocks];
    uint32_t place[kMaxUniformBlocks]; // where each of those goes in the new memory
    uint32_t write = 0;                // bit per block
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const UniformBlock &block = pipeline.blocks[i];
        const UniformSlot &slot = device.uniformSlots[block.binding];
        keys[i] = KeyForBlock(device, block.binding, vs, ps, textures);
        if (slot.size >= block.size && slot.frame == device.frameSerial && slot.key == keys[i].key
            && slot.shader == keys[i].shader && (!dynamic || slot.buffer == device.uniformBuffer))
        {
            if (device.verifyShortcuts && !HeldBlockIsRight(device, block, keys[i], vs, ps, textures, slot))
                return Fail(device, "uniform block %u kept from an earlier draw is not what this draw's state gives",
                            block.binding);
            continue;
        }
        write |= 1u << i;
        place[i] = total;
        total += aligned(block.size);
    }
    if (write)
    {
        Transient memory = AllocateTransient(device.core, total, alignment);
        if (memory.data && dynamic && memory.buffer != device.uniformBuffer)
        {
            // The frame's memory has moved on to another buffer, and a set 0
            // points at one buffer: all of this draw's blocks go to the new
            // one. One allocation is always in one buffer.
            const uint32_t all = (1u << count) - 1;
            if (write != all)
            {
                write = all;
                total = 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    place[i] = total;
                    total += aligned(pipeline.blocks[i].size);
                }
                memory = AllocateTransient(device.core, total, alignment);
            }
            device.uniformBuffer = memory.buffer;
        }
        if (!memory.data)
            return Fail(device, "out of per-frame memory for uniforms: %s", LastError(device.core));
        device.stats.constantBytes += total;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!(write & (1u << i)))
                continue;
            const UniformBlock &block = pipeline.blocks[i];
            FillUniformBlock(device, block, keys[i], vs, ps, textures, static_cast<uint8_t *>(memory.data) + place[i]);
            UniformSlot &slot = device.uniformSlots[block.binding];
            slot.frame = device.frameSerial;
            slot.buffer = memory.buffer;
            slot.offset = memory.offset + place[i];
            slot.data = static_cast<const uint8_t *>(memory.data) + place[i];
            slot.size = block.size;
            slot.key = keys[i].key;
            slot.shader = keys[i].shader;
        }
    }

    // Offsets go in binding order, which is the order of blocks.
    UniformBind bind;
    bind.layout = layout.pipeline;
    if (dynamic)
    {
        bind.offsetCount = count;
        for (uint32_t i = 0; i < count; ++i)
            bind.offsets[i] = static_cast<uint32_t>(device.uniformSlots[pipeline.blocks[i].binding].offset);
        if (device.keepDrawState)
        {
            // A set 0 holds the pipeline's blocks in one transient buffer,
            // which lives as long as the core: kept across frames.
            for (const Pipeline::LastingUniformSet &kept : pipeline.lastingUniforms)
            {
                if (kept.generation == device.lastingGeneration && kept.buffer == device.uniformBuffer)
                {
                    bind.set = kept.set;
                    ++device.stats.descriptorSetsReused;
                    break;
                }
            }
        }
        else if (pipeline.uniformFrame == device.frameSerial && pipeline.uniformBuffer == device.uniformBuffer)
        {
            bind.set = pipeline.uniformSet;
            ++device.stats.descriptorSetsReused;
        }
    }
    if (!bind.set)
    {
        const bool lasting = dynamic && device.keepDrawState;
        bind.set = lasting ? AllocateLastingSet(device, layout.uniformSet)
                           : AllocateDescriptorSet(device.core, layout.uniformSet);
        if (!bind.set)
            return Fail(device, "cannot allocate a descriptor set: %s", LastError(device.core));
        VkDescriptorBufferInfo infos[kMaxUniformBlocks];
        VkWriteDescriptorSet writes[kMaxUniformBlocks];
        for (uint32_t i = 0; i < count; ++i)
        {
            const UniformBlock &block = pipeline.blocks[i];
            const UniformSlot &slot = device.uniformSlots[block.binding];
            // A dynamic buffer's offset is added at bind time.
            infos[i] = {slot.buffer, dynamic ? 0 : slot.offset, block.size};
            writes[i] = {};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = bind.set;
            writes[i].dstBinding = block.binding;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        device.vk->vkUpdateDescriptorSets(device.dev, count, writes, 0, nullptr);
        ++device.stats.descriptorSets;
        if (lasting)
        {
            Pipeline::LastingUniformSet &kept =
                pipeline.lastingUniforms[pipeline.lastingUniformNext++ % kLastingUniformSets];
            kept.generation = device.lastingGeneration;
            kept.buffer = device.uniformBuffer;
            kept.set = bind.set;
        }
        else if (dynamic)
        {
            pipeline.uniformFrame = device.frameSerial;
            pipeline.uniformBuffer = device.uniformBuffer;
            pipeline.uniformSet = bind.set;
        }
    }
    // The command buffer has this set at these offsets already: a draw that
    // changed no block.
    const UniformBind &bound = device.boundUniforms;
    if (bound.set == bind.set && bound.layout == bind.layout && bound.offsetCount == bind.offsetCount
        && std::memcmp(bound.offsets, bind.offsets, bind.offsetCount * sizeof(uint32_t)) == 0)
    {
        ++device.stats.bindsSkipped;
        return true;
    }
    device.boundUniforms = bind;
    RecordUniformBind(device, bind);
    return true;
}

// The views and samplers a draw's texture set holds, by LayoutKey::samplers bit.
void TextureSetContents(Device &device, const Shader &vs, const Shader &ps, const DrawTextures &textures,
                        Device::TextureSetCache &out)
{
    out.mask = textures.mask;
    for (uint32_t left = textures.mask; left; left &= left - 1)
    {
        const uint32_t bit = LowestBit(left);
        const uint32_t slot = SlotForBit(bit);
        const Texture *bound = textures.bound[slot];
        const Texture *texture = bound ? bound : NullTexture(device, DeclaredType(vs, ps, bit));
        out.views[bit] = ImageSampleView(texture->image);
        out.samplers[bit] = SamplerFor(device, slot, bound);
    }
}

// Only the entries of the mask's bits are filled in.
bool SameTextureSet(const Device::TextureSetCache &a, const Device::TextureSetCache &b)
{
    if (a.layout != b.layout || a.mask != b.mask)
        return false;
    for (uint32_t left = a.mask; left; left &= left - 1)
    {
        const uint32_t bit = LowestBit(left);
        if (a.views[bit] != b.views[bit] || a.samplers[bit] != b.samplers[bit])
            return false;
    }
    return true;
}

void WriteTextures(Device &device, const Device::TextureSetCache &contents, VkDescriptorSet set)
{
    VkDescriptorImageInfo images[2 * kSamplerSlots];
    VkWriteDescriptorSet writes[2 * kSamplerSlots];
    uint32_t count = 0;
    for (uint32_t left = contents.mask; left; left &= left - 1)
    {
        const uint32_t bit = LowestBit(left);
        const ls::Stage stage = bit < 16 ? ls::Stage::Pixel : ls::Stage::Vertex;
        const uint32_t sampler = bit < 16 ? bit : bit - 16;
        images[count] = {VK_NULL_HANDLE, contents.views[bit], VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
        images[count + 1] = {contents.samplers[bit], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
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

// The lasting texture set with these contents, written now if there is none.
// Leaves it in lastTextureSet. Null on failure.
VkDescriptorSet LastingTextureSet(Device &device, Device::TextureSetCache &contents)
{
    Device::LastingTextureSets &table = device.lastingTextures;
    const uint32_t mask = kLastingTableSlots - 1;
    uint32_t at = static_cast<uint32_t>(HashTextureSet(contents)) & mask;
    for (; table.slots[at]; at = (at + 1) & mask)
    {
        const Device::TextureSetCache &entry = table.entries[table.slots[at] - 1];
        if (SameTextureSet(contents, entry))
        {
            ++device.stats.descriptorSetsReused;
            device.lastTextureSet = entry;
            return entry.set;
        }
    }
    const uint64_t generation = device.lastingGeneration;
    const VkDescriptorSet set = AllocateLastingSet(device, contents.layout);
    if (!set)
        return VK_NULL_HANDLE;
    if (device.lastingGeneration != generation)
    {
        // Every lasting set was given up for this one: the table is empty.
        at = static_cast<uint32_t>(HashTextureSet(contents)) & mask;
    }
    WriteTextures(device, contents, set);
    ++device.stats.descriptorSets;
    contents.set = set;
    table.entries.push_back(contents);
    table.slots[at] = static_cast<uint32_t>(table.entries.size());
    device.lastTextureSet = contents;
    return set;
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
VkRect2D DrawArea(const Device &device, uint32_t width, uint32_t height)
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
    return ClampRect(left, top, right, bottom, width, height);
}

VkRect2D DrawArea(const Device &device) { return DrawArea(device, device.passWidth, device.passHeight); }

DynamicState MakeDynamicState(const Device &device, d3d::PrimitiveType type)
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
    return state;
}

// False when a shortcut is wrong (Device::verifyShortcuts).
bool ApplyDynamicState(Device &device, d3d::PrimitiveType type)
{
    // Nothing it is built from has changed since the command buffer got it.
    if (device.dynamicValid && !device.dynamicStale && device.dynamic.topology == uint32_t(Topology(type)))
    {
        if (device.verifyShortcuts && !(MakeDynamicState(device, type) == device.dynamic))
            return Fail(device, "the dynamic state kept from an earlier draw is not what the D3D9 state gives");
        return true;
    }
    const DynamicState state = MakeDynamicState(device, type);
    device.dynamicStale = false;
    if (device.dynamicValid && state == device.dynamic)
        return true;
    Record(device.core, [state](VkCommandBuffer cmd, const Dispatch &vk) {
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
    });
    device.dynamic = state;
    device.dynamicValid = true;
    return true;
}

// Device::keepDrawState off: the draw finds nothing an earlier one left for
// it, so it makes its pipeline key and dynamic state, writes every uniform
// block and records every bind. The pipeline and the dynamic state its
// command buffer has stay known: the adapter compared those and left an equal
// one alone before it kept anything else, so a draw writes and records what
// it did then.
void DropKeptDrawState(Device &device)
{
    device.keyStale = true;
    device.dynamicStale = true;
    for (UniformSlot &slot : device.uniformSlots)
        slot.size = 0;
    for (VertexBind &bind : device.boundVertices)
        bind.buffer = VK_NULL_HANDLE;
    device.boundIndices = VK_NULL_HANDLE;
    device.boundUniforms.set = VK_NULL_HANDLE;
    device.boundTextureSet = VK_NULL_HANDLE;
}

// Everything a draw needs, up to the draw command. False: skip the draw.
bool PrepareDraw(Device &device, d3d::PrimitiveType type)
{
    if (!EnsureFrame(device))
        return false;
    if (!device.keepDrawState)
        DropKeptDrawState(device);
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
    // A clear after this draw is no longer one at the pass's start.
    device.passEmpty = false;
    // The last draw's pipeline, when nothing its key is made of has changed.
    // Where the device fixes vertex strides in the pipeline the key also holds
    // stream strides, so it is made for every draw there. Where it fixes depth
    // compare in the pixel shader the key holds the samplers it reads that
    // have a depth texture bound (the rule SamplerStateBlock follows), which
    // the textures bound for this draw say.
    const OptionalFeatures &features = Features(device.core);
    const bool dynamicStride = features.dynamicVertexStride;
    const uint32_t depthSamplers = features.fixedDepthCompare ? DepthSamplers(textures) : 0;
    Pipeline *pipeline = nullptr;
    if (!device.keyStale && device.lastPipeline && dynamicStride
        && device.lastPipelineKey.depthSamplers == depthSamplers
        && device.lastPipelineKey.topologyClass == TopologyClass(type))
    {
        pipeline = device.lastPipeline;
        if (device.verifyShortcuts
            && !(MakeKey(device, *vs, *ps, *decl, type, depthSamplers) == device.lastPipelineKey))
            return Fail(device, "the pipeline kept from an earlier draw is not the one the D3D9 state gives");
    }
    else
    {
        const PipelineKey key = MakeKey(device, *vs, *ps, *decl, type, depthSamplers);
        // Consecutive draws often keep their pipeline: compare before hashing.
        pipeline = device.lastPipeline;
        if (!pipeline || !(key == device.lastPipelineKey))
        {
            auto found = device.pipelines.find(key);
            pipeline = found != device.pipelines.end() ? &found->second : nullptr;
            if (!pipeline && device.ahead)
                pipeline = AheadFind(device, key);
            if (!pipeline)
                pipeline = BuildPipeline(device, key, *vs, *ps, *decl);
            if (!pipeline)
                return false;
            if (!pipeline->drawn)
                AheadDrawn(device, key, *pipeline);
            device.lastPipeline = pipeline;
            device.lastPipelineKey = key;
        }
        device.keyStale = false;
    }

    // Vertex streams first: an unbound one skips the draw before anything is recorded.
    for (uint32_t left = pipeline->bindings & ~(1u << kZeroBinding); left; left &= left - 1)
    {
        const uint32_t binding = LowestBit(left);
        const StreamBinding &stream = device.streams[binding];
        if (!stream.transient && !stream.buffer)
        {
            WarnOnce(device, WarnUnboundStream, "a draw reads vertex stream %u, which has no buffer; skipped",
                     binding);
            return false;
        }
    }

    // From here on only what the command buffer does not have already is
    // recorded: the engine draws sorted by material, so most draws keep the
    // pipeline, the buffers and the textures of the one before.
    if (!SyncRecording(device))
        return false;
    if (device.boundPipeline != pipeline->pipeline)
    {
        const VkPipeline bound = pipeline->pipeline;
        Record(device.core, [bound](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        });
        device.boundPipeline = bound;
    }
    if (!ApplyDynamicState(device, type))
        return false;
    // Without dynamic strides the pipeline holds them (PipelineKey::strides).
    for (uint32_t left = pipeline->bindings; left; left &= left - 1)
    {
        const uint32_t binding = LowestBit(left);
        VertexBind bind;
        if (binding == kZeroBinding)
        {
            bind = {BufferHandle(device.zeroVertices), 0, dynamicStride ? 16u : 0u};
        }
        else
        {
            StreamBinding &stream = device.streams[binding];
            if (stream.transient)
                bind = {stream.transient, stream.transientOffset, 0};
            else
                bind = {DrawBuffer(device, *stream.buffer), stream.offset, 0};
            bind.stride = dynamicStride ? stream.stride : 0;
        }
        VertexBind &bound = device.boundVertices[binding];
        if (bound.buffer == bind.buffer && bound.offset == bind.offset && bound.stride == bind.stride)
        {
            ++device.stats.bindsSkipped;
            continue;
        }
        bound = bind;
        Record(device.core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdBindVertexBuffers2(cmd, binding, 1, &bind.buffer, &bind.offset, nullptr,
                                       dynamicStride ? &bind.stride : nullptr);
        });
    }

    const Layout &layout = *pipeline->layout;
    if (device.boundLayout != layout.pipeline)
    {
        // Sets bound with another pipeline layout may not carry over.
        device.boundLayout = layout.pipeline;
        device.boundUniforms.set = VK_NULL_HANDLE;
        device.boundTextureSet = VK_NULL_HANDLE;
    }
    if (device.keepDrawState)
        CheckLastingSets(device);
    if (layout.uniformSet && !BindUniforms(device, *vs, *ps, textures, *pipeline))
        return false;
    if (layout.textureSet)
    {
        Device::TextureSetCache contents;
        contents.layout = layout.textureSet;
        TextureSetContents(device, *vs, *ps, textures, contents);
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (device.lastTextureSet.set && SameTextureSet(contents, device.lastTextureSet))
        {
            set = device.lastTextureSet.set;
            ++device.stats.descriptorSetsReused;
        }
        else if (device.keepDrawState)
        {
            set = LastingTextureSet(device, contents);
            if (!set)
                return Fail(device, "cannot allocate a descriptor set: %s", LastError(device.core));
        }
        else
        {
            set = AllocateDescriptorSet(device.core, layout.textureSet);
            if (!set)
                return Fail(device, "cannot allocate a descriptor set: %s", LastError(device.core));
            WriteTextures(device, contents, set);
            contents.set = set;
            device.lastTextureSet = contents;
            ++device.stats.descriptorSets;
        }
        if (device.boundTextureSet == set)
        {
            ++device.stats.bindsSkipped;
        }
        else
        {
            device.boundTextureSet = set;
            const VkPipelineLayout pipelineLayout = layout.pipeline;
            Record(device.core, [pipelineLayout, set](VkCommandBuffer cmd, const Dispatch &vk) {
                vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, ls::kTextureSet, 1,
                                           &set, 0, nullptr);
            });
        }
    }
    return Usable(device);
}

// Called for a draw about to be recorded, in its pass. While occlusion
// queries are between BEGIN and END, draws are inside one of the core's
// counts, which every such query shares. The count ends with the pass (a
// target change, a part handed to the GPU, Present) or when a query begins
// or ends, and the next draw starts another.
void CountDraw(Device &device)
{
    if (device.countingQueries.empty() || OpenOcclusionCount(device.core) != kNoOcclusionCount)
        return;
    const uint32_t count = BeginOcclusionCount(device.core);
    if (count == kNoOcclusionCount)
        return;
    for (Query *query : device.countingQueries)
    {
        // The first query has the count from BeginOcclusionCount.
        if (query != device.countingQueries.front())
            ShareOcclusionCount(device.core, count);
        query->counts.push_back(count);
    }
}

void ReleaseCounts(Device &device, Query &query)
{
    for (uint32_t count : query.counts)
        ReleaseOcclusionCount(device.core, count);
    query.counts.clear();
}

// Called after every draw recorded. Submits what the frame has recorded so
// far once that is worth a submission and the GPU is about to run out of
// work, so the GPU draws the frame while the rest of it is recorded.
//
// Without this a frame reaches the GPU when it is presented, and the GPU
// cannot start it before the render thread has finished it. The engine
// holds each frame back until the GPU has finished the one before
// (RB_AdaptiveGpuSyncFinal and R_SyncGpu, r_gpuSync 1), so the GPU then
// idles from the end of every frame until the next has been recorded in
// full: 1.9 ms of every 8.2 ms frame on an Intel U300 (laptop run 8), where
// DXVK, which submits during the frame, kept the GPU busy throughout.
//
// The rule is DXVK's (GpuFlushTracker), counted in draws instead of command
// chunks: submit after GpuFeed::draws draws while the GPU has fewer than two
// submissions left to finish, and wait for that many again for each further
// one, up to four times. A GPU that is behind is left alone, and a frame is
// never cut into more than its draws divided by that count.
//
// The idle rule (GpuFeed::idleDraws, off unless set) is for the GPU that has
// nothing left at all, which under r_gpuSync is every frame's start: the
// engine lets the render thread begin a frame about when the GPU finishes
// the one before, and every draw recorded before the first part goes out is
// time the GPU stands still. A part then goes out after idleDraws draws, the
// frame's next such part after twice as many, and so on up to the plain
// count, so the rule adds a few parts to a frame and no more.
//
// The draws that follow start a new command buffer and a new pass on the
// same targets, which loads what the last one stored: the result is the same.
void FeedGpu(Device &device)
{
    const GpuFeed &feed = device.feed;
    if (!feed.on)
        return;
    const bool idleRule = feed.idleDraws && feed.idleDraws < feed.draws;
    if (idleRule && device.feedIdleFrame != device.frameSerial)
    {
        device.feedIdleFrame = device.frameSerial;
        device.feedIdleNeed = feed.idleDraws;
    }
    // Under the idle rule the GPU is looked at that often; a look costs a
    // system call at most every 10 microseconds (RefreshProgress).
    const bool idleLooks = idleRule && device.feedIdleNeed < feed.draws;
    if (device.feedRecording != device.stateRecording)
    {
        // The frame's first command buffer, or the first draw since a flush.
        device.feedRecording = device.stateRecording;
        device.feedDraws = 0;
        device.feedCheckAt = idleLooks ? device.feedIdleNeed : feed.draws;
    }
    ++device.feedDraws;
    if (!feed.always)
    {
        if (device.feedDraws < device.feedCheckAt)
            return;
        constexpr uint32_t kMaxBacklog = 4;
        const uint32_t pending = PendingSubmissions(device.core);
        if (idleLooks && !pending && device.feedDraws >= device.feedIdleNeed)
        {
            // The GPU has run dry: this part goes now, the next waits longer.
            device.feedIdleNeed = std::min(feed.draws, device.feedIdleNeed * 2);
            ++device.stats.feedIdleFlushes;
        }
        else if (device.feedDraws < feed.draws * std::clamp(pending, 1u, kMaxBacklog))
        {
            // The GPU has work: look again a quarter of a part later, or as
            // soon as the idle rule could apply.
            uint32_t step = std::max(1u, feed.draws / 4);
            if (idleLooks)
                step = std::min(step, std::max(1u, feed.idleDraws));
            device.feedCheckAt = device.feedDraws + step;
            return;
        }
    }
    EndPassIfOpen(device);
    if (!Flush(device.core, false))
    {
        Fail(device, "Flush: %s", LastError(device.core));
        return;
    }
    ++device.stats.feedFlushes;
}
} // namespace

size_t PipelineKeyHash::operator()(const PipelineKey &key) const
{
    // Eight bytes at a time: a byte-wise hash of the 80-byte key was a
    // measurable part of every draw (laptop run 6).
    static_assert(sizeof(PipelineKey) % 8 == 0, "the key is hashed in 8-byte words");
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&key);
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < sizeof(key); i += 8)
    {
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 32;
    }
    return static_cast<size_t>(hash);
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
        // Cached sets come from an earlier frame's pools.
        ++device.frameSerial;
        device.lastTextureSet = Device::TextureSetCache{};
        return true;
    case FrameResult::Skipped:
        device.frameSkipped = true;
        return false;
    default:
        return Fail(device, "BeginFrame: %s", LastError(device.core));
    }
}

void ForgetRecordingState(Device &device)
{
    device.stateRecording = 0;
    device.boundPipeline = VK_NULL_HANDLE;
    device.dynamicValid = false;
    for (VertexBind &bind : device.boundVertices)
        bind.buffer = VK_NULL_HANDLE;
    device.boundIndices = VK_NULL_HANDLE;
    device.boundUniforms.set = VK_NULL_HANDLE;
    device.boundLayout = VK_NULL_HANDLE;
    device.boundTextureSet = VK_NULL_HANDLE;
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
    // A view handle can be handed out again once the texture is gone, so a
    // cached set must not be matched by it.
    device.lastTextureSet = Device::TextureSetCache{};
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
    device.lastPipeline = nullptr;
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
    device.lastPipeline = nullptr;
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
    if (state >= d3d::RS_COUNT || device->renderStates[state] == value)
        return;
    device->renderStates[state] = value;
    // The alpha test is a uniform block's (KeyForBlock), and in no dynamic
    // state; its compare is in the pipeline key too where the pixel shader
    // has it fixed (PipelineKey::alphaCompare). Every other state may be in
    // either.
    if (state == d3d::RS_ALPHAREF)
        return;
    if (state == d3d::RS_ALPHATESTENABLE || state == d3d::RS_ALPHAFUNC)
    {
        if (Features(device->core).fixedAlphaTest)
            device->keyStale = true;
        return;
    }
    device->keyStale = true;
    device->dynamicStale = true;
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
    // The slot's sampler stays: SamplerFor builds another when this texture's
    // traits differ from those it was built for.
    device->textures[slot] = texture;
}

void SetVertexShader(Device *device, Shader *shader)
{
    if (shader && shader->stage != ls::Stage::Vertex)
    {
        Log(*device, "SetVertexShader: shader %08x is a pixel shader", shader->translated.bytecodeHash);
        return;
    }
    device->vertexShader = shader;
    device->keyStale = true;
}

void SetPixelShader(Device *device, Shader *shader)
{
    if (shader && shader->stage != ls::Stage::Pixel)
    {
        Log(*device, "SetPixelShader: shader %08x is a vertex shader", shader->translated.bytecodeHash);
        return;
    }
    device->pixelShader = shader;
    device->keyStale = true;
}

namespace
{
// Registers set to the values they have change nothing. Otherwise the block
// of them in the frame's memory is stale, if it reaches these registers: a
// later draw writes a new one (BindUniforms).
void SetFloatConstants(Device &device, ls::Stage stage, float *registers, uint32_t start, const float *data,
                       uint32_t count)
{
    const size_t bytes = size_t(count) * 4 * sizeof(float);
    if (std::memcmp(registers, data, bytes) == 0)
        return;
    std::memcpy(registers, data, bytes);
    UniformSlot &slot = device.uniformSlots[ls::UniformBinding(stage, ls::Uniform::FloatConstants)];
    if (start * 16 < slot.size)
        slot.size = 0;
}
} // namespace

void SetVertexShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (start >= kVsFloatRegisters)
        return;
    count = std::min(count, kVsFloatRegisters - start);
    SetFloatConstants(*device, ls::Stage::Vertex, device->vsConstants[start], start, data, count);
}

void SetPixelShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (start >= kPsFloatRegisters)
        return;
    count = std::min(count, kPsFloatRegisters - start);
    SetFloatConstants(*device, ls::Stage::Pixel, device->psConstants[start], start, data, count);
}

void SetVertexDecl(Device *device, VertexDecl *decl)
{
    device->decl = decl;
    device->keyStale = true;
}

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

void SetViewport(Device *device, const Viewport &viewport)
{
    device->viewport = viewport;
    device->dynamicStale = true;
}

void SetScissorRect(Device *device, const Rect &rect)
{
    device->scissor = rect;
    device->dynamicStale = true;
}

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
        device->dynamicStale = true;
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

// A clear of the whole of the bound targets becomes the load operation of a
// pass that draws to them: a new pass is begun with it, or the open pass,
// still empty, is begun again with it and any clear it began with. After a
// draw, the open pass ends and the next one begins with the clear, rather
// than clearing inside the pass: MoltenVK drew nothing for a depth clear
// there (the pass_clear check on CI's Apple GPU). False when the clear is not
// such a one.
bool ClearAtPassStart(Device &device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color,
                      float z, uint32_t stencil)
{
    if (!Features(device.core).passClears || (rectCount && rects))
        return false;
    const bool reopen = PassMatches(device) && device.passEmpty;
    const uint32_t colors = ColorCount(device.renderTargets);
    const bool clearColor = (flags & d3d::CLEAR_TARGET) && colors;
    bool clearDepth = false;
    if (device.depthStencil && (flags & (d3d::CLEAR_ZBUFFER | d3d::CLEAR_STENCIL)))
    {
        // The depth image always has stencil, so a load operation clears
        // both: only when both are cleared, or the D3D9 format has none.
        const bool stencilFormat = device.depthStencil->texture->format->d3d == d3d::FMT_D24S8;
        if (!(flags & d3d::CLEAR_ZBUFFER) || (stencilFormat && !(flags & d3d::CLEAR_STENCIL)))
            return false;
        clearDepth = true;
    }
    if (!clearColor && !clearDepth)
        return false;
    uint32_t width = 0, height = 0;
    TargetsSize(device, width, height);
    if (width == UINT32_MAX || height == UINT32_MAX)
        return false;
    const VkRect2D area = DrawArea(device, width, height);
    if (area.offset.x || area.offset.y || area.extent.width != width || area.extent.height != height)
        return false;
    // A load operation clears the whole image, and the core lets go of its
    // old contents: every cleared target must be one level and one face of
    // exactly the pass's size.
    const auto whole = [&](const Surface *surface) {
        const Texture &texture = *surface->texture;
        return texture.faces == 1 && texture.desc.levels == 1 && texture.desc.width == width
            && texture.desc.height == height;
    };
    for (uint32_t i = 0; clearColor && i < colors; ++i)
    {
        if (!whole(device.renderTargets[i]))
            return false;
    }
    if (clearDepth && !whole(device.depthStencil))
        return false;

    Device::PassLoads loads = reopen ? device.passLoads : Device::PassLoads{};
    if (clearColor)
    {
        loads.clearColor = true;
        UnpackColor(color, loads.color);
    }
    if (clearDepth)
    {
        loads.clearDepth = true;
        loads.depth = std::clamp(z, 0.0f, 1.0f);
        loads.stencil = stencil & 0xff;
    }
    EndPassIfOpen(device);
    if (!BeginTargetsPass(device, loads))
        return true; // skipped with a warning, as the clear would have been
    ++device.stats.clears;
    ++device.stats.passClears;
    return true;
}

void Clear(Device *device_, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
           uint32_t stencil)
{
    Device &device = *device_;
    if (!EnsureFrame(device))
        return;
    if (ClearAtPassStart(device, rectCount, rects, flags, color, z, stencil))
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
    // Fixed-size packets: the attachments, and up to kRects rectangles each.
    constexpr uint32_t kRects = 8;
    struct ClearPacket
    {
        VkClearAttachment attachments[kMaxRenderTargets + 1];
        uint32_t attachmentCount;
        VkClearRect rects[kRects];
        uint32_t rectCount;
    };
    ClearPacket packet{};
    std::memcpy(packet.attachments, attachments, sizeof(attachments));
    packet.attachmentCount = count;
    for (size_t first = 0; first < clearRects.size(); first += kRects)
    {
        packet.rectCount = static_cast<uint32_t>(std::min<size_t>(kRects, clearRects.size() - first));
        std::memcpy(packet.rects, clearRects.data() + first, packet.rectCount * sizeof(VkClearRect));
        Record(device.core, [packet](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdClearAttachments(cmd, packet.attachmentCount, packet.attachments, packet.rectCount, packet.rects);
        });
    }
    device.passEmpty = false;
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
    const VkBuffer indices = DrawBuffer(*device, *device->indices);
    const VkIndexType indexType = device->indices->indexType;
    CountDraw(*device);
    if (device->boundIndices == indices && device->boundIndexType == indexType)
    {
        ++device->stats.bindsSkipped;
        Record(device->core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdDrawIndexed(cmd, indexCount, 1, startIndex, baseVertex, 0);
        });
    }
    else
    {
        device->boundIndices = indices;
        device->boundIndexType = indexType;
        Record(device->core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdBindIndexBuffer(cmd, indices, 0, indexType);
            vk.vkCmdDrawIndexed(cmd, indexCount, 1, startIndex, baseVertex, 0);
        });
    }
    ++device->stats.draws;
    FeedGpu(*device);
}

void DrawPrimitive(Device *device, d3d::PrimitiveType type, uint32_t startVertex, uint32_t primitiveCount)
{
    const uint32_t count = VertexCount(type, primitiveCount);
    if (!count || !PrepareDraw(*device, type))
        return;
    CountDraw(*device);
    Record(device->core, [=](VkCommandBuffer cmd, const Dispatch &vk) { vk.vkCmdDraw(cmd, count, 1, startVertex, 0); });
    ++device->stats.draws;
    FeedGpu(*device);
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
            CountDraw(*device);
            Record(device->core, [count](VkCommandBuffer cmd, const Dispatch &vk) { vk.vkCmdDraw(cmd, count, 1, 0, 0); });
            ++device->stats.draws;
            FeedGpu(*device);
        }
    }
    // As D3D9: stream 0 is unbound afterwards.
    device->streams[0] = StreamBinding{};
}

// ---- Copies -------------------------------------------------------------------------------------

namespace
{
// A multisampled colour texture resolved whole into resolveScratch, a
// one-sample render target of its size and format; null on failure. Not
// inside a pass.
Texture *ResolveToScratch(Device &device, Texture *from)
{
    Texture *&scratch = device.resolveScratch;
    if (scratch
        && (scratch->desc.width != from->desc.width || scratch->desc.height != from->desc.height
            || scratch->desc.format != from->desc.format))
    {
        ForgetTexture(device, scratch);
        DestroyTextureNow(device, scratch); // the core keeps the image until the GPU is done with it
        scratch = nullptr;
    }
    if (!scratch)
    {
        TextureDesc desc;
        desc.width = from->desc.width;
        desc.height = from->desc.height;
        desc.levels = 1;
        desc.usage = d3d::USAGE_RENDERTARGET;
        desc.format = from->desc.format;
        desc.pool = d3d::POOL_DEFAULT;
        desc.name = "legacy resolve scratch";
        scratch = CreateTextureInternal(device, desc, 0);
        if (!scratch)
            return nullptr;
    }
    ResolveImage(device.core, from->image, {0, 0}, scratch->image, {0, 0}, {from->desc.width, from->desc.height});
    return Usable(device) ? scratch : nullptr;
}
} // namespace

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
    if (to->samples > 1 || (from->samples > 1 && from->format->depth))
    {
        // D3D9 resolves a multisampled colour surface and nothing else.
        WarnOnce(device, WarnStretchToSamples, "StretchRect from %s to %s: only a multisampled colour surface "
                 "into a one-sample one is supported", from->name.c_str(), to->name.c_str());
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
    if (from->samples > 1)
    {
        const VkExtent2D extent = {uint32_t(srcBox[1].x - srcBox[0].x), uint32_t(srcBox[1].y - srcBox[0].y)};
        if (extent.width == uint32_t(dstBox[1].x - dstBox[0].x) && extent.height == uint32_t(dstBox[1].y - dstBox[0].y)
            && from->vkFormat == to->vkFormat)
        {
            // Same size and format: the resolve is the copy.
            ResolveImage(device.core, from->image, {srcBox[0].x, srcBox[0].y}, to->image, {dstBox[0].x, dstBox[0].y},
                         extent, dst->level, dst->face);
            Usable(device);
            return;
        }
        // Scaled or converted: resolved whole first, then blitted as any
        // one-sample surface is.
        from = ResolveToScratch(device, from);
        if (!from)
            return;
    }
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
    // D3D9 refuses a multisampled source; here it gives the resolved
    // picture, which is what a screenshot of it wants.
    if (from->samples > 1 && !(from = ResolveToScratch(device, from)))
        return false;
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
    const TextureDesc &desc = dst->desc;
    const FormatInfo &format = *dst->format;
    std::vector<Box> boxes;
    for (uint32_t face = 0; face < dst->faces; ++face)
    {
        Texture::Dirty *dirty = face < src->dirty.size() ? &src->dirty[face] : nullptr;
        const bool all = !dirty || dirty->all;
        if (!all && dirty->boxes.empty())
            continue; // nothing written since the last update
        for (uint32_t level = 0; level < levels; ++level)
        {
            const Subresource &from = src->subresources[face * src->desc.levels + level];
            if (from.cpu.empty())
                continue; // never written
            if (all)
            {
                // The whole level is replaced, so nothing is read back.
                LockedRect locked;
                if (!LockRect(device_, dst, face, level, nullptr, d3d::LOCK_DISCARD, locked))
                    return;
                Subresource &to = dst->subresources[face * dst->desc.levels + level];
                std::memcpy(to.cpu.data(), from.cpu.data(), std::min(to.cpu.size(), from.cpu.size()));
                UnlockRect(device_, dst, face, level);
                continue;
            }
            // The dirty boxes in this level's texels, grown to whole blocks of
            // a compressed format; two may meet once scaled down.
            const uint32_t width = LevelSize(desc.width, level), height = LevelSize(desc.height, level);
            const uint32_t depth = desc.kind == TextureKind::Volume ? LevelSize(desc.depth, level) : 1;
            const uint32_t round = (1u << level) - 1;
            boxes.clear();
            for (const Box &top : dirty->boxes)
            {
                Box box;
                box.left = std::min(top.left >> level, width - 1);
                box.top = std::min(top.top >> level, height - 1);
                box.front = std::min(top.front >> level, depth - 1);
                box.right = std::min(std::max((top.right + round) >> level, box.left + 1), width);
                box.bottom = std::min(std::max((top.bottom + round) >> level, box.top + 1), height);
                box.back = std::min(std::max((top.back + round) >> level, box.front + 1), depth);
                if (format.compressed)
                {
                    box.left &= ~3u;
                    box.top &= ~3u;
                    box.right = std::min((box.right + 3) & ~3u, width);
                    box.bottom = std::min((box.bottom + 3) & ~3u, height);
                }
                AddBoxApart(boxes, box);
            }
            EndPassIfOpen(device);
            if (!UploadRegions(device, *dst, face, level, boxes.data(), static_cast<uint32_t>(boxes.size()),
                               from.cpu.data()))
                return;
            Subresource &to = dst->subresources[face * dst->desc.levels + level];
            to.uploaded = true;
            if (!to.cpu.empty() && to.cpu.size() == from.cpu.size())
            {
                // A level kept on the CPU (locked again since) gets the same bytes.
                uint32_t rowBytes = 0, rows = 0, slices = 0;
                LevelLayout(*dst, level, rowBytes, rows, slices);
                const size_t slice = static_cast<size_t>(rowBytes) * rows;
                for (const Box &box : boxes)
                {
                    const size_t x = static_cast<size_t>(format.compressed ? box.left / 4 : box.left) * format.blockBytes;
                    const size_t bytes =
                        static_cast<size_t>(format.compressed ? (box.right - box.left + 3) / 4 : box.right - box.left)
                        * format.blockBytes;
                    const uint32_t y0 = format.compressed ? box.top / 4 : box.top;
                    const uint32_t y1 = format.compressed ? (box.bottom + 3) / 4 : box.bottom;
                    for (uint32_t z = box.front; z < box.back; ++z)
                        for (uint32_t y = y0; y < y1; ++y)
                        {
                            const size_t at = z * slice + static_cast<size_t>(y) * rowBytes + x;
                            std::memcpy(to.cpu.data() + at, from.cpu.data() + at, bytes);
                        }
                }
            }
        }
        if (dirty)
        {
            dirty->all = false;
            dirty->boxes.clear();
        }
    }
}

// ---- Frames and queries ----------------------------------------------------------------------

void DetachWindow(Device *device_)
{
    Device &device = *device_;
    EndPassIfOpen(device);
    gfx_vk::DetachWindow(device.core); // ends an open frame without showing it
    device.frameOpen = false;
    device.frameSkipped = false;
    ForgetRecordingState(device);
}


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
    // A multisampled back buffer is resolved into the front buffer, which is
    // what is shown; the back buffer stays the back buffer.
    const bool multisampled = device.backBuffer->samples > 1;
    const TextureDesc &back = device.backBuffer->desc;
    if (multisampled)
        ResolveImage(device.core, device.backBuffer->image, {0, 0}, device.frontBuffer->image, {0, 0},
                     {back.width, back.height});
    // Acquired here, at the end of the frame (null when out of date: the
    // core's Present then skips the frame).
    if (Image *swapchain = SwapchainImage(device.core))
    {
        const ImageDesc &shown = ImageInfo(swapchain);
        const VkOffset3D src[2] = {{0, 0, 0}, {int32_t(back.width), int32_t(back.height), 1}};
        const VkOffset3D dst[2] = {{0, 0, 0}, {int32_t(shown.width), int32_t(shown.height), 1}};
        const bool scaled = shown.width != back.width || shown.height != back.height;
        BlitImage(device.core, (multisampled ? device.frontBuffer : device.backBuffer)->image, src, swapchain, dst,
                  scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    // Flip: the image just shown becomes the front buffer, and the next frame
    // draws into the other one. Render target 0 and every other use of the
    // back buffer go through backBufferSurface, which keeps its address.
    if (!multisampled)
    {
        std::swap(device.backBuffer, device.frontBuffer);
        device.backBufferSurface.texture = device.backBuffer;
        device.frontBufferSurface.texture = device.frontBuffer;
    }
    const FrameResult result = gfx_vk::Present(device.core);
    device.frameOpen = false;
    ForgetRecordingState(device);
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
    if (!query)
        return;
    auto &counting = device->countingQueries;
    counting.erase(std::remove(counting.begin(), counting.end(), query), counting.end());
    ReleaseCounts(*device, *query);
    delete query;
}

void IssueQuery(Device *device, Query *query, bool begin)
{
    if (!query)
        return;
    if (query->kind == QueryKind::Occlusion && Features(device->core).occlusionCounts)
    {
        // The count that is open belongs to the queries open until now.
        EndOcclusionCount(device->core);
        auto &counting = device->countingQueries;
        counting.erase(std::remove(counting.begin(), counting.end(), query), counting.end());
        const bool begun = query->counting;
        query->counting = begin;
        if (begin || !begun)
        {
            // A query issued again starts from nothing, whether it was read
            // or not; so does an END that no BEGIN came before.
            ReleaseCounts(*device, *query);
            query->counted = false;
            query->samples = 0;
        }
        if (begin)
            counting.push_back(query);
    }
    // An event marks everything recorded before it. Right after Present
    // nothing is being recorded, so it is the frame just submitted, not the
    // next one: the engine's swap fence (RB_SwapBuffers) then finishes with
    // its frame, and the render thread no longer waits for the next frame's
    // GPU work as well before it ends that frame.
    if (!begin)
    {
        query->serial = RecordedSerial(device->core);
        // Between frames with nothing recorded: the end of the frame just
        // presented. (Not an event issued inside a frame after a flush.)
        query->frameFence = query->kind == QueryKind::Event && !device->frameOpen
            && query->serial != SubmissionSerial(device->core);
        query->flushedAhead = false;
    }
}

namespace
{
// An occlusion query whose end the GPU has passed: adds its counts up. False
// if one of them cannot be read yet.
bool AddCounts(Device &device, Query &query)
{
    if (query.counted)
        return true;
    uint64_t total = 0;
    for (uint32_t count : query.counts)
    {
        uint64_t samples = 0;
        if (!ReadOcclusionCount(device.core, count, samples))
            return false;
        total += samples;
    }
    ReleaseCounts(device, query);
    query.samples = static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
    query.counted = true;
    if (query.serial) // not one that was never issued
    {
        ++device.stats.occlusionResults;
        device.stats.occlusionResultsZero += total ? 0 : 1;
        device.stats.occlusionSamples += total;
        device.stats.occlusionLast = query.samples;
    }
    return true;
}
} // namespace

bool GetQueryData(Device *device_, Query *query, bool flush, uint32_t &result)
{
    Device &device = *device_;
    result = 0;
    if (!query || !Usable(device))
        return true; // a lost device reports done, as D3D9's does
    const bool occlusion = query->kind == QueryKind::Occlusion;
    const bool counts = occlusion && Features(device.core).occlusionCounts;
    // Between BEGIN and END there is no result.
    if (counts && query->counting)
        return false;
    if (!query->serial || SerialFinished(device.core, query->serial))
    {
        if (!occlusion)
            result = 1;
        else if (!counts)
            result = 0xffffffu; // a device that cannot count: every sample passed
        else if (AddCounts(device, *query))
            result = query->samples;
        else
            return false;
        return true;
    }
    if (!flush)
        return false;
    const uint64_t recording = SubmissionSerial(device.core);
    // Its own end is not submitted yet.
    const bool own = query->serial == recording;
    // A frame fence the caller waits on, with the next frame's work recorded
    // and not submitted: the GPU gets it now, behind the frame it is still on,
    // so it has no gap to wait through when that frame ends. The engine polls
    // its swap fence this way once a frame is drawn, in steps of up to 1 ms
    // (RB_BackendTimeout), and presents only afterwards.
    const bool ahead = !own && query->frameFence && !query->flushedAhead && Features(device.core).fenceFlush
        && RecordedSerial(device.core) == recording;
    if (own || ahead)
    {
        EndPassIfOpen(device);
        if (!Flush(device.core, false))
            Fail(device, "Flush: %s", LastError(device.core));
        if (ahead)
        {
            query->flushedAhead = true;
            ++device.stats.fenceFlushes;
        }
    }
    return false;
}
} // namespace gfx_vk::legacy
