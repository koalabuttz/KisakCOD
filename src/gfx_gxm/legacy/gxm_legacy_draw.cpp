// The GXM Legacy back end's draws: what D3D9 state becomes in sceGxm.
//
// - Scenes. sceGxm draws inside scenes, one colour surface and one
//   depth-stencil surface each. The scene opens at the first draw or clear
//   after the targets change and closes when they change again, before the
//   CPU reads a target, and at Present. Depth-stencil surfaces load and store
//   their contents every scene, so a frame may come back to a target.
// - Clears are quads drawn in the scene (sceGxm has no clear call), with the
//   depth and stencil written by the depth test. StretchRect draws a
//   textured quad into the destination.
// - Programs. A shader's variant (translator options: alpha test, D3DCOLOR
//   inputs, inputs the declaration lacks, element widths) is compiled through
//   the program cache and registered with the shader patcher on first use.
//   Vertex programs are kept per variant, declaration and strides; fragment
//   programs per variant, vertex variant, blend state and target format.
// - Textures are linear, laid out as sceGxm reads a linear texture (rows of
//   whole multiples of 8 texels, mip levels sized from the power of two
//   above the base). Render-target textures are strided. Cube and
//   block-compressed (DXT) textures are swizzled, since sceGxm refuses linear
//   DXT: LockRect hands out a scratch level that UnlockRect swizzles in.
//   Depth textures are not sampled yet: draws bind a stand-in.
// - Memory the GPU may still read (a DISCARD's old buffer, a destroyed
//   resource, released programs) is freed three frames later: the display
//   queue lets the CPU run at most two frames ahead.
//
// See gxm_legacy.cpp for the rest of the back end, docs/vita-client.md.

#if !defined(KISAK_GFX_GXM)
#error "gxm_legacy_draw.cpp is the PS Vita's Legacy back end (KISAK_GFX_GXM)"
#endif

#include "gxm_legacy_internal.h"

#include <gfx_gxm/gxm_program_cache.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace gfx_vk::legacy
{
namespace
{
// ---- Formats ---------------------------------------------------------------------------------

bool TextureFormat(d3d::Format format, SceGxmTextureFormat &out)
{
    switch (format)
    {
    case d3d::FMT_A8R8G8B8: out = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB; return true;
    case d3d::FMT_X8R8G8B8: out = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB; return true;
    case d3d::FMT_A8B8G8R8: out = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; return true;
    case d3d::FMT_X8B8G8R8: out = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR; return true;
    case d3d::FMT_R5G6B5: out = SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB; return true;
    case d3d::FMT_A1R5G5B5: out = SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB; return true;
    case d3d::FMT_X1R5G5B5: out = SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1RGB; return true;
    case d3d::FMT_A4R4G4B4: out = SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ARGB; return true;
    // The swizzles are written from alpha down to red: A8 samples as
    // (0, 0, 0, a), L8 as (l, l, l, 1), A8L8 as (l, l, l, a).
    case d3d::FMT_A8: out = SCE_GXM_TEXTURE_FORMAT_U8_R000; return true;
    case d3d::FMT_L8: out = SCE_GXM_TEXTURE_FORMAT_U8_1RRR; return true;
    case d3d::FMT_A8L8: out = SCE_GXM_TEXTURE_FORMAT_U8U8_GRRR; return true;
    case d3d::FMT_V8U8: out = SCE_GXM_TEXTURE_FORMAT_S8S8_00GR; return true;
    case d3d::FMT_G16R16: out = SCE_GXM_TEXTURE_FORMAT_U16U16_00GR; return true;
    case d3d::FMT_A16B16G16R16: out = SCE_GXM_TEXTURE_FORMAT_U16U16U16U16_ABGR; return true;
    case d3d::FMT_R16F: out = SCE_GXM_TEXTURE_FORMAT_F16_111R; return true;
    case d3d::FMT_G16R16F: out = SCE_GXM_TEXTURE_FORMAT_F16F16_00GR; return true;
    case d3d::FMT_A16B16G16R16F: out = SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR; return true;
    case d3d::FMT_R32F: out = SCE_GXM_TEXTURE_FORMAT_F32_111R; return true;
    case d3d::FMT_G32R32F: out = SCE_GXM_TEXTURE_FORMAT_F32F32_00GR; return true;
    case d3d::FMT_DXT1: out = SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR; return true;
    case d3d::FMT_DXT2:
    case d3d::FMT_DXT3: out = SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR; return true;
    case d3d::FMT_DXT4:
    case d3d::FMT_DXT5: out = SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR; return true;
    default: return false;
    }
}

// A colour target as sceGxm writes it.
struct ColorTarget
{
    SceGxmColorFormat format;
    SceGxmOutputRegisterFormat output;
    SceGxmOutputRegisterSize size;
    bool noAlpha; // reads destination alpha as 1 (X8R8G8B8 and the screen)
};

bool ColorTargetFormat(d3d::Format format, ColorTarget &out)
{
    out.size = SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
    out.output = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
    out.noAlpha = false;
    switch (format)
    {
    case d3d::FMT_X8R8G8B8: out.noAlpha = true; [[fallthrough]];
    case d3d::FMT_A8R8G8B8: out.format = SCE_GXM_COLOR_FORMAT_A8R8G8B8; return true;
    case d3d::FMT_X8B8G8R8: out.noAlpha = true; [[fallthrough]];
    case d3d::FMT_A8B8G8R8: out.format = SCE_GXM_COLOR_FORMAT_A8B8G8R8; return true;
    case d3d::FMT_R5G6B5: out.noAlpha = true; out.format = SCE_GXM_COLOR_FORMAT_U5U6U5_RGB; return true;
    case d3d::FMT_X1R5G5B5: out.noAlpha = true; [[fallthrough]];
    case d3d::FMT_A1R5G5B5: out.format = SCE_GXM_COLOR_FORMAT_U1U5U5U5_ARGB; return true;
    case d3d::FMT_A16B16G16R16F:
        out.format = SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR;
        out.output = SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4;
        out.size = SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT;
        return true;
    case d3d::FMT_G16R16F:
        out.format = SCE_GXM_COLOR_FORMAT_F16F16_GR;
        out.output = SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF2;
        return true;
    case d3d::FMT_R32F:
        out.format = SCE_GXM_COLOR_FORMAT_F32_R;
        out.output = SCE_GXM_OUTPUT_REGISTER_FORMAT_FLOAT;
        return true;
    default: return false;
    }
}

// How many levels sceGxm samples: the chain stops where either side, halved
// as sceGxm halves it, would reach 0.
uint32_t SampledLevels(const Texture &texture)
{
    if (texture.desc.kind != TextureKind::Tex2D || (texture.desc.usage & d3d::USAGE_RENDERTARGET))
        return 1;
    uint32_t side = std::min(texture.desc.width, texture.desc.height), levels = 1;
    while (side > 1)
    {
        side >>= 1;
        ++levels;
    }
    return std::min({texture.desc.levels, levels, 13u});
}

uint32_t NextPowerOfTwo(uint32_t value)
{
    uint32_t p = 1;
    while (p < value)
        p <<= 1;
    return p;
}

// ---- Programs ------------------------------------------------------------------------------

struct VertexProgramEntry
{
    SceGxmVertexProgram *program = nullptr;
    const Variant *variant = nullptr;
    const VertexDecl *decl = nullptr;
    uint8_t streams[kMaxStreams] = {}; // D3D9 stream per sceGxm stream
    uint8_t streamCount = 0;
};

struct FragmentProgramEntry
{
    SceGxmFragmentProgram *program = nullptr;
    const Variant *pixel = nullptr;
    const Variant *vertex = nullptr;
};

// What a draw failed on, each counted and the first of each logged.
enum Skip : uint8_t
{
    SkipNoShader,
    SkipNotTranslated,
    SkipNoProgram,
    SkipNoDecl,
    SkipNoStream,
    SkipVertexProgram,
    SkipFragmentProgram,
    SkipNoScene,
    SkipPrimitive,
    SkipNoIndices,
    SkipEmptyClip,
    SkipCount,
};
const char *const kSkipNames[SkipCount] = {
    "no vertex or pixel shader set",
    "the shader was not translated",
    "the shader's program did not compile or register",
    "no vertex declaration",
    "a stream has no buffer in GPU memory",
    "the vertex program could not be made",
    "the fragment program could not be made",
    "no scene could be opened on the targets",
    "a primitive type sceGxm lacks (line strips)",
    "no index buffer in GPU memory",
    "the viewport and scissor leave nothing to draw",
};

// The fixed state sceGxm keeps in the context, as last set.
struct FixedState
{
    float viewport[6];
    uint32_t clip[4];
    // sceGxm's enum values, which use high bits.
    uint32_t cull, polygon, depthFunc, depthWrite;
    uint32_t stencilFunc[2], stencilFail[2], stencilDepthFail[2], stencilPass[2];
    uint8_t stencilMask, stencilWriteMask, stencilRef, pad;
    int32_t biasFactor, biasUnits;
    bool operator==(const FixedState &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};

constexpr uint32_t kRingSegment = 512u << 10; // per frame, for DrawPrimitiveUP and clears
constexpr uint32_t kRingSegments = 3;
constexpr uint32_t kSequentialIndices = 60000; // sceGxm wants 16-bit index values below 64000
constexpr uint64_t kFramesInFlight = 3;
} // namespace

struct DrawState
{
    // The open scene.
    bool sceneOpen = false;
    Surface *sceneColor = nullptr;
    Surface *sceneDepth = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0;
    bool hasColor = false;
    ColorTarget color{};
    SceGxmColorSurface colorSurface;
    uint64_t scenes = 0;

    std::map<std::string, VertexProgramEntry> vertexPrograms;
    std::map<std::string, FragmentProgramEntry> fragmentPrograms;
    const SceGxmVertexProgram *boundVertex = nullptr;
    const SceGxmFragmentProgram *boundFragment = nullptr;
    FixedState fixed{};
    bool fixedValid = false;

    // Clears.
    Variant clearVertex, clearFragment;
    SceGxmVertexProgram *clearVertexProgram = nullptr;
    const SceGxmProgramParameter *clearDepth = nullptr, *clearColor = nullptr;
    // StretchRect.
    Variant copyVertex, copyFragment;
    SceGxmVertexProgram *copyVertexProgram = nullptr;
    int copyUnit = -1;

    uint16_t *sequential = nullptr; // 0, 1, 2, ... for draws without indices
    uint8_t *ring = nullptr;
    uint32_t ringUsed = 0;

    // Stand-ins for textures not sampled yet.
    uint8_t *standInMemory = nullptr;
    SceGxmTexture black, white, cube;

    uint64_t frame = 0; // Presents so far
    std::vector<std::pair<uint64_t, std::function<void()>>> deferred;

    uint64_t drawn = 0, skipped[SkipCount] = {};
    uint64_t renames = 0, ringOverflows = 0;
    uint32_t drawFailures = 0, uniformFailures = 0;
    uint64_t copies = 0;
    uint32_t variantsMade = 0, variantFailures = 0;

    // Run diagnostics (see DiagnoseFrame): draws of this frame still to log,
    // and this frame's test (DiagMode).
    uint32_t diagnosedDraws = 0;
    int diagMode = 0;
    Variant diagGreen; // a fragment program that writes opaque green
    // A vertex program that takes the game's POSITION as screen pixels.
    Variant diagPass;
    const SceGxmProgramParameter *diagPassPosition = nullptr;
    std::map<uint32_t, SceGxmVertexProgram *> diagPassPrograms;
};

// Run diagnostics (DiagnoseDraw, DiagnoseFrame): which frames.
constexpr uint64_t kDiagnosedFrames[] = {300};
constexpr uint64_t kReadBackFrames[] = {300, 700, 1000, 1300, 1600, 1900};
constexpr uint64_t kTestRectFirst = 600, kTestRectLast = 779;
// Tests on the game's own draws, three seconds each (read back at 1000,
// 1300, 1600 and 1900). Every test writes opaque green with no blending
// (run G: the game's draws make no fragments at all, green or not).
enum DiagMode
{
    DiagNone = 0,
    DiagPassThrough = 1, // frames 900-1079: a pass-through vertex program reads the game's positions as pixels
    DiagExpand = 2,      // frames 1200-1379: the game's program, vertices copied out, no index buffer
    DiagPassExpand = 3,  // frames 1500-1679: both
    DiagGreen = 4,       // frames 1800-1979: the game's program and buffers (run G's test 1 again)
};
int DiagModeFor(uint64_t frame)
{
    if (frame >= 900 && frame < 1080)
        return DiagPassThrough;
    if (frame >= 1200 && frame < 1380)
        return DiagExpand;
    if (frame >= 1500 && frame < 1680)
        return DiagPassExpand;
    if (frame >= 1800 && frame < 1980)
        return DiagGreen;
    return DiagNone;
}

namespace
{
SceGxmContext *Gxm()
{
    return gxm::Context();
}

void Defer(Device &device, std::function<void()> work)
{
    device.draw->deferred.emplace_back(device.draw->frame, std::move(work));
}

void CountSkip(Device &device, Skip why, const char *detail = nullptr)
{
    if (device.draw->skipped[why]++ == 0)
        Log(device, "draw skipped: %s%s%s (first of its kind; counted from now on)", kSkipNames[why],
            detail ? ": " : "", detail ? detail : "");
}

// Per-frame memory the GPU reads this frame only.
void *Transient(Device &device, uint32_t bytes)
{
    DrawState &d = *device.draw;
    bytes = AlignUp(bytes, 16);
    if (d.ringUsed + bytes <= kRingSegment)
    {
        void *at = d.ring + (d.frame % kRingSegments) * kRingSegment + d.ringUsed;
        d.ringUsed += bytes;
        return at;
    }
    // Rare: a frame wants more. Its own block, freed with the frame.
    ++d.ringOverflows;
    void *block = gxm::Allocate(gxm::MemoryKind::Main, bytes, 16);
    if (block)
        Defer(device, [block] { gxm::Free(gxm::MemoryKind::Main, block); });
    return block;
}

bool Register(Device &device, Variant &variant, const char *what)
{
    if (variant.gxp.empty())
    {
        variant.failed = true;
        return false;
    }
    const int result = sceGxmShaderPatcherRegisterProgram(gxm::ShaderPatcher(),
                                                          reinterpret_cast<const SceGxmProgram *>(variant.gxp.data()),
                                                          &variant.id);
    if (result < 0)
    {
        Log(device, "%s: sceGxmShaderPatcherRegisterProgram failed (0x%08x)", what, unsigned(result));
        variant.failed = true;
        return false;
    }
    const SceGxmProgram *program = variant.program();
    variant.constants = sceGxmProgramFindParameterByName(program, "c");
    variant.alphaRef = sceGxmProgramFindParameterByName(program, "kisak_alpha_ref");
    char name[32];
    for (int i = 0; i < 16; ++i)
    {
        variant.textureUnit[i] = -1;
        std::snprintf(name, sizeof(name), "s%d", i);
        if (const SceGxmProgramParameter *sampler = sceGxmProgramFindParameterByName(program, name))
            if (sceGxmProgramParameterGetCategory(sampler) == SCE_GXM_PARAMETER_CATEGORY_SAMPLER)
                variant.textureUnit[i] = int8_t(sceGxmProgramParameterGetResourceIndex(sampler));
        std::snprintf(name, sizeof(name), "kisak_depth_s%d", i);
        variant.depth[i] = sceGxmProgramFindParameterByName(program, name);
        std::snprintf(name, sizeof(name), "in_v%d", i);
        variant.inputs[i] = sceGxmProgramFindParameterByName(program, name);
    }
    variant.uniforms = sceGxmProgramGetDefaultUniformBufferSize(program) != 0;
    return true;
}

uint64_t VariantKey(const gxm::cg::Options &options)
{
    // Widths of 4 are stored as 0. Alpha tests are for pixel shaders, which
    // have no inputs, so they share the low bits.
    uint64_t components = 0;
    for (int i = 0; i < 16; ++i)
        components |= uint64_t(options.inputComponents[i] & 3) << (2 * i);
    return (uint64_t(options.bgraInputs) | uint64_t(options.absentInputs) << 16) ^ (components << 32) ^
           (options.alphaFunc == gxm::cg::AlphaFunc::Always ? 0 : uint64_t(options.alphaFunc)) ^
           (options.farPlaneInside ? 0x5a5a000000000000ull : 0);
}

// The shader's program for these options: made, compiled and registered on
// first use. Null when it cannot be had (logged once per variant).
Variant *GetVariant(Device &device, Shader &shader, const gxm::cg::Options &options)
{
    const uint64_t key = VariantKey(options);
    for (const auto &variant : shader.variants)
        if (variant->key == key)
            return variant->failed ? nullptr : variant.get();
    DrawState &d = *device.draw;
    std::unique_ptr<Variant> variant(new Variant);
    variant->key = key;
    if (key == 0)
        variant->gxp = shader.program;
    else
    {
        std::string cg, error, log;
        if (!gxm::cg::Emit(shader.parsed, options, cg, error))
            log = error;
        else if (!gxm::GetProgram(cg, shader.pixel ? gxm::ShaderStage::Fragment : gxm::ShaderStage::Vertex,
                                  variant->gxp, log))
            variant->gxp.clear();
        if (variant->gxp.empty() && d.variantFailures++ < 20)
            Log(device, "%s shader variant %016llx did not compile: %s", shader.pixel ? "pixel" : "vertex",
                (unsigned long long)key, log.substr(0, log.find('\n')).c_str());
    }
    ++d.variantsMade;
    Register(device, *variant, "shader variant");
    shader.variants.push_back(std::move(variant));
    Variant *made = shader.variants.back().get();
    return made->failed ? nullptr : made;
}

void ReleaseVertexProgram(Device &device, SceGxmVertexProgram *program)
{
    Defer(device, [program] { sceGxmShaderPatcherReleaseVertexProgram(gxm::ShaderPatcher(), program); });
}

void ReleaseFragmentProgram(Device &device, SceGxmFragmentProgram *program)
{
    Defer(device, [program] { sceGxmShaderPatcherReleaseFragmentProgram(gxm::ShaderPatcher(), program); });
}

// ---- Vertex input ----------------------------------------------------------------------------

bool AttributeFormat(uint8_t type, uint8_t &format, uint8_t &components)
{
    switch (type)
    {
    case d3d::DECLTYPE_FLOAT1: format = SCE_GXM_ATTRIBUTE_FORMAT_F32; components = 1; return true;
    case d3d::DECLTYPE_FLOAT2: format = SCE_GXM_ATTRIBUTE_FORMAT_F32; components = 2; return true;
    case d3d::DECLTYPE_FLOAT3: format = SCE_GXM_ATTRIBUTE_FORMAT_F32; components = 3; return true;
    case d3d::DECLTYPE_FLOAT4: format = SCE_GXM_ATTRIBUTE_FORMAT_F32; components = 4; return true;
    case d3d::DECLTYPE_D3DCOLOR:
    case d3d::DECLTYPE_UBYTE4N: format = SCE_GXM_ATTRIBUTE_FORMAT_U8N; components = 4; return true;
    case d3d::DECLTYPE_UBYTE4: format = SCE_GXM_ATTRIBUTE_FORMAT_U8; components = 4; return true;
    case d3d::DECLTYPE_SHORT2: format = SCE_GXM_ATTRIBUTE_FORMAT_S16; components = 2; return true;
    case d3d::DECLTYPE_SHORT4: format = SCE_GXM_ATTRIBUTE_FORMAT_S16; components = 4; return true;
    case d3d::DECLTYPE_SHORT2N: format = SCE_GXM_ATTRIBUTE_FORMAT_S16N; components = 2; return true;
    case d3d::DECLTYPE_SHORT4N: format = SCE_GXM_ATTRIBUTE_FORMAT_S16N; components = 4; return true;
    case d3d::DECLTYPE_USHORT2N: format = SCE_GXM_ATTRIBUTE_FORMAT_U16N; components = 2; return true;
    case d3d::DECLTYPE_USHORT4N: format = SCE_GXM_ATTRIBUTE_FORMAT_U16N; components = 4; return true;
    case d3d::DECLTYPE_FLOAT16_2: format = SCE_GXM_ATTRIBUTE_FORMAT_F16; components = 2; return true;
    case d3d::DECLTYPE_FLOAT16_4: format = SCE_GXM_ATTRIBUTE_FORMAT_F16; components = 4; return true;
    default: return false; // UDEC3 and DEC3N: no sceGxm format
    }
}

// Which element feeds each of the vertex shader's inputs, and the
// translator options that follows from it.
struct InputPlan
{
    const VertexElement *element[16] = {};
    uint8_t format[16] = {}, components[16] = {};
    gxm::cg::Options options;
};

void PlanInputs(Device &device, const Shader &shader, const VertexDecl &decl, InputPlan &plan)
{
    for (const gxm::cg::VertexInput &input : shader.parsed.reflection.inputs)
    {
        if (input.reg >= 16)
            continue;
        const VertexElement *found = nullptr;
        for (const VertexElement &element : decl.elements)
            if (element.usage == input.usage && element.usageIndex == input.usageIndex &&
                element.type != d3d::DECLTYPE_UNUSED && element.stream < kMaxStreams)
            {
                found = &element;
                break;
            }
        uint8_t format = 0, components = 0;
        if (found && !AttributeFormat(found->type, format, components))
        {
            NotYet(device, "vertex elements of type UDEC3 or DEC3N (read as 0, 0, 0, 1)");
            found = nullptr;
        }
        if (!found)
        {
            plan.options.absentInputs |= uint16_t(1u << input.reg);
            continue;
        }
        plan.element[input.reg] = found;
        plan.format[input.reg] = format;
        plan.components[input.reg] = components;
        if (found->type == d3d::DECLTYPE_D3DCOLOR)
            plan.options.bgraInputs |= uint16_t(1u << input.reg);
        else
            plan.options.inputComponents[input.reg] = components == 4 ? 0 : components;
    }
}

template <class T> void Append(std::string &key, const T &value)
{
    key.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

const VertexProgramEntry *GetVertexProgram(Device &device, const Variant &variant, const VertexDecl &decl,
                                           const InputPlan &plan, bool index32, const uint32_t *strides)
{
    DrawState &d = *device.draw;
    // Streams in use, in sceGxm's order.
    uint8_t streams[kMaxStreams];
    uint8_t streamCount = 0;
    int8_t gxmStream[kMaxStreams];
    std::fill(std::begin(gxmStream), std::end(gxmStream), int8_t(-1));
    for (int reg = 0; reg < 16; ++reg)
    {
        const VertexElement *element = plan.element[reg];
        if (!element || !variant.inputs[reg] || gxmStream[element->stream] >= 0)
            continue;
        gxmStream[element->stream] = int8_t(streamCount);
        streams[streamCount++] = uint8_t(element->stream);
    }
    std::string key;
    Append(key, &variant);
    Append(key, &decl);
    Append(key, index32);
    for (uint8_t i = 0; i < streamCount; ++i)
    {
        Append(key, streams[i]);
        Append(key, strides[streams[i]]);
    }
    auto found = d.vertexPrograms.find(key);
    if (found != d.vertexPrograms.end())
        return found->second.program ? &found->second : nullptr;

    VertexProgramEntry entry;
    entry.variant = &variant;
    entry.decl = &decl;
    entry.streamCount = streamCount;
    std::copy(streams, streams + streamCount, entry.streams);
    SceGxmVertexAttribute attributes[16];
    uint32_t attributeCount = 0;
    for (int reg = 0; reg < 16; ++reg)
    {
        const VertexElement *element = plan.element[reg];
        if (!element || !variant.inputs[reg])
            continue;
        SceGxmVertexAttribute &a = attributes[attributeCount++];
        a.streamIndex = uint16_t(gxmStream[element->stream]);
        a.offset = element->offset;
        a.format = plan.format[reg];
        a.componentCount = plan.components[reg];
        a.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(variant.inputs[reg]));
    }
    SceGxmVertexStream gxmStreams[kMaxStreams];
    for (uint8_t i = 0; i < streamCount; ++i)
    {
        gxmStreams[i].stride = uint16_t(strides[streams[i]]);
        gxmStreams[i].indexSource = index32 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    }
    const int result = sceGxmShaderPatcherCreateVertexProgram(gxm::ShaderPatcher(), variant.id, attributes,
                                                              attributeCount, gxmStreams, streamCount,
                                                              &entry.program);
    if (result < 0)
    {
        entry.program = nullptr;
        char detail[96];
        std::snprintf(detail, sizeof(detail), "sceGxmShaderPatcherCreateVertexProgram 0x%08x, %u attributes",
                      unsigned(result), attributeCount);
        CountSkip(device, SkipVertexProgram, detail);
    }
    auto inserted = d.vertexPrograms.emplace(std::move(key), entry).first;
    return entry.program ? &inserted->second : nullptr;
}

// ---- Blend ------------------------------------------------------------------------------------

uint8_t BlendFactor(Device &device, uint32_t blend, bool noAlpha)
{
    switch (blend)
    {
    case d3d::BLEND_ZERO: return SCE_GXM_BLEND_FACTOR_ZERO;
    case d3d::BLEND_ONE: return SCE_GXM_BLEND_FACTOR_ONE;
    case d3d::BLEND_SRCCOLOR: return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case d3d::BLEND_INVSRCCOLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case d3d::BLEND_SRCALPHA: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case d3d::BLEND_INVSRCALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3d::BLEND_DESTALPHA: return noAlpha ? SCE_GXM_BLEND_FACTOR_ONE : SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case d3d::BLEND_INVDESTALPHA: return noAlpha ? SCE_GXM_BLEND_FACTOR_ZERO : SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case d3d::BLEND_DESTCOLOR: return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case d3d::BLEND_INVDESTCOLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case d3d::BLEND_SRCALPHASAT: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
        NotYet(device, "the blend factor colour (D3DBLEND_BLENDFACTOR; drawn as ONE)");
        return SCE_GXM_BLEND_FACTOR_ONE;
    }
}

uint8_t BlendFunc(uint32_t op)
{
    switch (op)
    {
    case d3d::BLENDOP_SUBTRACT: return SCE_GXM_BLEND_FUNC_SUBTRACT;
    case d3d::BLENDOP_REVSUBTRACT: return SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    case d3d::BLENDOP_MIN: return SCE_GXM_BLEND_FUNC_MIN;
    case d3d::BLENDOP_MAX: return SCE_GXM_BLEND_FUNC_MAX;
    default: return SCE_GXM_BLEND_FUNC_ADD;
    }
}

// False: no blend info at all (no blending, every channel written).
bool MakeBlend(Device &device, bool noAlpha, SceGxmBlendInfo &blend)
{
    const uint32_t *rs = device.renderStates;
    const uint32_t mask = rs[d3d::RS_COLORWRITEENABLE];
    // D3D9: R 1, G 2, B 4, A 8. sceGxm: A 1, R 2, G 4, B 8.
    blend.colorMask = uint8_t((mask & 1 ? SCE_GXM_COLOR_MASK_R : 0) | (mask & 2 ? SCE_GXM_COLOR_MASK_G : 0) |
                              (mask & 4 ? SCE_GXM_COLOR_MASK_B : 0) | (mask & 8 ? SCE_GXM_COLOR_MASK_A : 0));
    if (!rs[d3d::RS_ALPHABLENDENABLE])
    {
        blend.colorFunc = blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
        blend.colorSrc = blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        blend.colorDst = blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
        return blend.colorMask != SCE_GXM_COLOR_MASK_ALL;
    }
    auto pair = [&](uint32_t src, uint32_t dst, uint8_t &srcOut, uint8_t &dstOut) {
        if (src == d3d::BLEND_BOTHSRCALPHA || src == d3d::BLEND_BOTHINVSRCALPHA)
        {
            const bool inverse = src == d3d::BLEND_BOTHINVSRCALPHA;
            srcOut = inverse ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
            dstOut = inverse ? SCE_GXM_BLEND_FACTOR_SRC_ALPHA : SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            return;
        }
        srcOut = BlendFactor(device, src, noAlpha);
        dstOut = BlendFactor(device, dst, noAlpha);
    };
    uint8_t s, t;
    pair(rs[d3d::RS_SRCBLEND], rs[d3d::RS_DESTBLEND], s, t);
    blend.colorSrc = s;
    blend.colorDst = t;
    blend.colorFunc = BlendFunc(rs[d3d::RS_BLENDOP]);
    if (rs[d3d::RS_SEPARATEALPHABLENDENABLE])
    {
        pair(rs[d3d::RS_SRCBLENDALPHA], rs[d3d::RS_DESTBLENDALPHA], s, t);
        blend.alphaFunc = BlendFunc(rs[d3d::RS_BLENDOPALPHA]);
    }
    else
        blend.alphaFunc = blend.colorFunc;
    blend.alphaSrc = s;
    blend.alphaDst = t;
    return true;
}

const FragmentProgramEntry *GetFragmentProgram(Device &device, const Variant &pixel, const Variant &vertex,
                                               const SceGxmBlendInfo *blend)
{
    DrawState &d = *device.draw;
    std::string key;
    Append(key, &pixel);
    Append(key, &vertex);
    uint32_t blendBits = 0;
    if (blend)
        std::memcpy(&blendBits, blend, sizeof(blendBits));
    Append(key, blendBits);
    Append(key, bool(blend));
    Append(key, d.color.output);
    auto found = d.fragmentPrograms.find(key);
    if (found != d.fragmentPrograms.end())
        return found->second.program ? &found->second : nullptr;
    FragmentProgramEntry entry;
    entry.pixel = &pixel;
    entry.vertex = &vertex;
    const int result =
        sceGxmShaderPatcherCreateFragmentProgram(gxm::ShaderPatcher(), pixel.id, d.color.output, SCE_GXM_MULTISAMPLE_NONE,
                                                 blend, vertex.program(), &entry.program);
    if (result < 0)
    {
        entry.program = nullptr;
        char detail[64];
        std::snprintf(detail, sizeof(detail), "sceGxmShaderPatcherCreateFragmentProgram 0x%08x", unsigned(result));
        CountSkip(device, SkipFragmentProgram, detail);
    }
    auto inserted = d.fragmentPrograms.emplace(std::move(key), entry).first;
    return entry.program ? &inserted->second : nullptr;
}

// ---- Scenes ------------------------------------------------------------------------------------

bool OpenScene(Device &device)
{
    DrawState &d = *device.draw;
    Surface *color = device.renderTargets[0];
    Surface *depth = device.depthStencil;
    if (d.sceneOpen && d.sceneColor == color && d.sceneDepth == depth)
        return true;
    EndScene(device);

    d.hasColor = false;
    uint32_t width = 0, height = 0;
    SceGxmSyncObject *sync = nullptr;
    const SceGxmColorSurface *colorSurface = nullptr;
    if (color && color->kind == SurfaceKind::BackBuffer)
    {
        colorSurface = gxm::BackBufferSurface();
        sync = gxm::BackBufferSync();
        width = gxm::kDisplayWidth;
        height = gxm::kDisplayHeight;
        ColorTargetFormat(d3d::FMT_X8B8G8R8, d.color);
        d.hasColor = true;
    }
    else if (color && color->kind == SurfaceKind::TextureLevel && color->texture->memory.gpu)
    {
        const Texture &texture = *color->texture;
        const Level &level = texture.levels[color->face * texture.desc.levels + color->level];
        if (!ColorTargetFormat(texture.desc.format, d.color))
        {
            NotYet(device, "a render target format sceGxm cannot draw to (draws skipped)");
            return false;
        }
        const int result = sceGxmColorSurfaceInit(&d.colorSurface, d.color.format, SCE_GXM_COLOR_SURFACE_LINEAR,
                                                  SCE_GXM_COLOR_SURFACE_SCALE_NONE, d.color.size, level.width,
                                                  level.height, level.pitch / texture.format->blockBytes,
                                                  texture.memory.data + level.offset);
        if (result < 0)
        {
            Log(device, "sceGxmColorSurfaceInit on %s failed (0x%08x)", texture.name.c_str(), unsigned(result));
            return false;
        }
        colorSurface = &d.colorSurface;
        width = level.width;
        height = level.height;
        d.hasColor = true;
    }
    else if (color)
    {
        NotYet(device, "drawing to the front buffer or a target outside GPU memory");
        return false;
    }
    const SceGxmDepthStencilSurface *depthSurface = nullptr;
    if (depth && depth->kind == SurfaceKind::TextureLevel && depth->texture->format->depth &&
        depth->texture->memory.gpu)
    {
        depthSurface = &depth->texture->depthSurface;
        if (!width)
        {
            width = depth->texture->desc.width;
            height = depth->texture->desc.height;
        }
    }
    if (!width || !height)
        return false;
    SceGxmRenderTarget *target = gxm::RenderTargetFor(width, height);
    if (!target)
        return false;
    const int result =
        sceGxmBeginScene(Gxm(), 0, target, nullptr, nullptr, sync, colorSurface, depthSurface);
    if (result < 0)
    {
        if (device.draw->scenes == 0 || device.draw->skipped[SkipNoScene] == 0)
            Log(device, "sceGxmBeginScene %ux%u (colour %s, depth %s) failed (0x%08x)", width, height,
                colorSurface ? "yes" : "no", depthSurface ? "yes" : "no", unsigned(result));
        return false;
    }
    d.sceneOpen = true;
    d.sceneColor = color;
    d.sceneDepth = depth;
    d.sceneWidth = width;
    d.sceneHeight = height;
    d.boundVertex = nullptr;
    d.boundFragment = nullptr;
    d.fixedValid = false;
    ++d.scenes;
    ++device.stats.passes;
    return true;
}

void ApplyFixedState(Device &device, const FixedState &state)
{
    DrawState &d = *device.draw;
    if (d.fixedValid && d.fixed == state)
        return;
    SceGxmContext *context = Gxm();
    const float *v = state.viewport;
    // Said outright rather than trusting the context's default.
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(context, v[0], v[1], v[2], v[3], v[4], v[5]);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, state.clip[0], state.clip[1], state.clip[2],
                        state.clip[3]);
    sceGxmSetCullMode(context, SceGxmCullMode(state.cull));
    // Both faces get the same state: D3D9's two-sided stencil is not done yet.
    sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
    sceGxmSetFrontPolygonMode(context, SceGxmPolygonMode(state.polygon));
    sceGxmSetFrontDepthFunc(context, SceGxmDepthFunc(state.depthFunc));
    sceGxmSetFrontDepthWriteEnable(context, SceGxmDepthWriteMode(state.depthWrite));
    sceGxmSetFrontStencilFunc(context, SceGxmStencilFunc(state.stencilFunc[0]), SceGxmStencilOp(state.stencilFail[0]),
                              SceGxmStencilOp(state.stencilDepthFail[0]), SceGxmStencilOp(state.stencilPass[0]),
                              state.stencilMask, state.stencilWriteMask);
    sceGxmSetFrontStencilRef(context, state.stencilRef);
    sceGxmSetFrontDepthBias(context, state.biasFactor, state.biasUnits);
    d.fixed = state;
    d.fixedValid = true;
}

uint32_t DepthFunc(uint32_t cmp)
{
    switch (cmp)
    {
    case d3d::CMP_NEVER: return SCE_GXM_DEPTH_FUNC_NEVER;
    case d3d::CMP_LESS: return SCE_GXM_DEPTH_FUNC_LESS;
    case d3d::CMP_EQUAL: return SCE_GXM_DEPTH_FUNC_EQUAL;
    case d3d::CMP_LESSEQUAL: return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    case d3d::CMP_GREATER: return SCE_GXM_DEPTH_FUNC_GREATER;
    case d3d::CMP_NOTEQUAL: return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
    case d3d::CMP_GREATEREQUAL: return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
    default: return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
}

uint32_t StencilFunc(uint32_t cmp)
{
    switch (cmp)
    {
    case d3d::CMP_NEVER: return SCE_GXM_STENCIL_FUNC_NEVER;
    case d3d::CMP_LESS: return SCE_GXM_STENCIL_FUNC_LESS;
    case d3d::CMP_EQUAL: return SCE_GXM_STENCIL_FUNC_EQUAL;
    case d3d::CMP_LESSEQUAL: return SCE_GXM_STENCIL_FUNC_LESS_EQUAL;
    case d3d::CMP_GREATER: return SCE_GXM_STENCIL_FUNC_GREATER;
    case d3d::CMP_NOTEQUAL: return SCE_GXM_STENCIL_FUNC_NOT_EQUAL;
    case d3d::CMP_GREATEREQUAL: return SCE_GXM_STENCIL_FUNC_GREATER_EQUAL;
    default: return SCE_GXM_STENCIL_FUNC_ALWAYS;
    }
}

uint32_t StencilOp(uint32_t op)
{
    switch (op)
    {
    case d3d::STENCILOP_ZERO: return SCE_GXM_STENCIL_OP_ZERO;
    case d3d::STENCILOP_REPLACE: return SCE_GXM_STENCIL_OP_REPLACE;
    case d3d::STENCILOP_INCRSAT: return SCE_GXM_STENCIL_OP_INCR;
    case d3d::STENCILOP_DECRSAT: return SCE_GXM_STENCIL_OP_DECR;
    case d3d::STENCILOP_INVERT: return SCE_GXM_STENCIL_OP_INVERT;
    case d3d::STENCILOP_INCR: return SCE_GXM_STENCIL_OP_INCR_WRAP;
    case d3d::STENCILOP_DECR: return SCE_GXM_STENCIL_OP_DECR_WRAP;
    default: return SCE_GXM_STENCIL_OP_KEEP;
    }
}

float AsFloat(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The area a draw may touch: the viewport, cut by the scissor rect when the
// test is on, inside the scene. False when nothing is left.
bool DrawArea(Device &device, uint32_t clip[4])
{
    const DrawState &d = *device.draw;
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
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, int32_t(d.sceneWidth));
    bottom = std::min(bottom, int32_t(d.sceneHeight));
    if (right <= left || bottom <= top)
        return false;
    clip[0] = uint32_t(left);
    clip[1] = uint32_t(top);
    clip[2] = uint32_t(right - 1);
    clip[3] = uint32_t(bottom - 1);
    return true;
}

void MakeFixedState(Device &device, d3d::PrimitiveType type, FixedState &state)
{
    const uint32_t *rs = device.renderStates;
    const DrawState &d = *device.draw;
    state = FixedState{};
    // D3D9 samples pixel centres at whole coordinates, sceGxm half a pixel
    // in; its window y grows downwards like D3D9's.
    const Viewport &vp = device.viewport;
    state.viewport[0] = float(vp.x) + float(vp.width) * 0.5f + 0.5f;
    state.viewport[1] = float(vp.width) * 0.5f;
    state.viewport[2] = float(vp.y) + float(vp.height) * 0.5f + 0.5f;
    state.viewport[3] = -float(vp.height) * 0.5f;
    state.viewport[4] = std::clamp(vp.minZ, 0.0f, 1.0f);
    state.viewport[5] = std::clamp(vp.maxZ, 0.0f, 1.0f) - state.viewport[4];
    // D3D9's front faces are clockwise on screen; sceGxm names the winding
    // to cull as it looks on screen too.
    state.cull = rs[d3d::RS_CULLMODE] == d3d::CULL_CW    ? SCE_GXM_CULL_CW
                 : rs[d3d::RS_CULLMODE] == d3d::CULL_CCW ? SCE_GXM_CULL_CCW
                                                         : SCE_GXM_CULL_NONE;
    if (type == d3d::PT_POINTLIST)
        state.polygon = SCE_GXM_POLYGON_MODE_POINT;
    else if (type == d3d::PT_LINELIST || type == d3d::PT_LINESTRIP)
        state.polygon = SCE_GXM_POLYGON_MODE_LINE;
    else
        state.polygon = rs[d3d::RS_FILLMODE] == d3d::FILL_POINT       ? SCE_GXM_POLYGON_MODE_TRIANGLE_POINT
                        : rs[d3d::RS_FILLMODE] == d3d::FILL_WIREFRAME ? SCE_GXM_POLYGON_MODE_TRIANGLE_LINE
                                                                      : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    const bool depth = d.sceneDepth != nullptr && rs[d3d::RS_ZENABLE];
    state.depthFunc = depth ? DepthFunc(rs[d3d::RS_ZFUNC]) : SCE_GXM_DEPTH_FUNC_ALWAYS;
    state.depthWrite = depth && rs[d3d::RS_ZWRITEENABLE] ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    const bool stencil = d.sceneDepth != nullptr && rs[d3d::RS_STENCILENABLE];
    for (int side = 0; side < 2; ++side)
    {
        state.stencilFunc[side] = stencil ? StencilFunc(rs[d3d::RS_STENCILFUNC]) : SCE_GXM_STENCIL_FUNC_ALWAYS;
        state.stencilFail[side] = stencil ? StencilOp(rs[d3d::RS_STENCILFAIL]) : SCE_GXM_STENCIL_OP_KEEP;
        state.stencilDepthFail[side] = stencil ? StencilOp(rs[d3d::RS_STENCILZFAIL]) : SCE_GXM_STENCIL_OP_KEEP;
        state.stencilPass[side] = stencil ? StencilOp(rs[d3d::RS_STENCILPASS]) : SCE_GXM_STENCIL_OP_KEEP;
    }
    if (stencil && rs[d3d::RS_TWOSIDEDSTENCILMODE])
        NotYet(device, "two-sided stencil (both faces use the clockwise state)");
    state.stencilMask = uint8_t(rs[d3d::RS_STENCILMASK]);
    state.stencilWriteMask = stencil ? uint8_t(rs[d3d::RS_STENCILWRITEMASK]) : 0;
    state.stencilRef = uint8_t(rs[d3d::RS_STENCILREF]);
    // D3D9's bias is in units of the depth range; sceGxm's in steps of a
    // 24-bit depth.
    const float bias = AsFloat(rs[d3d::RS_DEPTHBIAS]) * float(1u << 24);
    const float slope = AsFloat(rs[d3d::RS_SLOPESCALEDEPTHBIAS]);
    state.biasUnits = int32_t(std::clamp(std::lround(bias), -32768L, 32767L));
    state.biasFactor = int32_t(std::clamp(std::lround(slope), -32768L, 32767L));
}

// ---- Textures --------------------------------------------------------------------------------

SceGxmTextureAddrMode AddressMode(uint32_t address)
{
    switch (address)
    {
    case d3d::TADDRESS_MIRROR: return SCE_GXM_TEXTURE_ADDR_MIRROR;
    case d3d::TADDRESS_CLAMP:
    case d3d::TADDRESS_BORDER: return SCE_GXM_TEXTURE_ADDR_CLAMP;
    case d3d::TADDRESS_MIRRORONCE: return SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP;
    default: return SCE_GXM_TEXTURE_ADDR_REPEAT;
    }
}

void BindTextures(Device &device, const Variant &pixel, void *uniforms)
{
    DrawState &d = *device.draw;
    for (int i = 0; i < 16; ++i)
    {
        if (pixel.textureUnit[i] < 0)
            continue;
        const Texture *texture = device.textures[i];
        const uint32_t *ss = device.samplerStates[i];
        SceGxmTexture view;
        float depth = 1.0f;
        if (!texture)
            view = d.black;
        else if (texture->sampleable)
        {
            view = texture->sampled;
            if (texture->desc.kind == TextureKind::Volume)
                depth = float(texture->desc.depth);
        }
        else if (texture->desc.kind == TextureKind::Cube)
        {
            NotYet(device, "sampling a cube texture sceGxm cannot take (a grey stand-in is bound)");
            view = d.cube;
        }
        else
        {
            if (texture->format->depth)
                NotYet(device, "sampling depth textures (a white stand-in is bound)");
            else
                NotYet(device, "sampling a texture outside GPU memory (a white stand-in is bound)");
            view = d.white;
        }
        const bool linearMin = ss[d3d::SAMP_MINFILTER] >= d3d::TEXF_LINEAR;
        const bool linearMag = ss[d3d::SAMP_MAGFILTER] >= d3d::TEXF_LINEAR;
        sceGxmTextureSetMagFilter(&view, linearMag ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMinFilter(&view, linearMin ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMipFilter(&view, ss[d3d::SAMP_MIPFILTER] == d3d::TEXF_LINEAR
                                             ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                             : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
        sceGxmTextureSetUAddrMode(&view, AddressMode(ss[d3d::SAMP_ADDRESSU]));
        sceGxmTextureSetVAddrMode(&view, AddressMode(ss[d3d::SAMP_ADDRESSV]));
        sceGxmSetFragmentTexture(Gxm(), unsigned(pixel.textureUnit[i]), &view);
        if (pixel.depth[i] && uniforms)
            sceGxmSetUniformDataF(uniforms, pixel.depth[i], 0, 1, &depth);
    }
}

bool MakeStandIns(Device &device, std::string &error)
{
    DrawState &d = *device.draw;
    // Three 8x8 RGBA textures, the cube's six faces after them.
    constexpr uint32_t kFace = 8 * 8 * 4;
    constexpr uint32_t kCubeFace = 4096; // room for any face alignment
    d.standInMemory = static_cast<uint8_t *>(gxm::Allocate(gxm::MemoryKind::Cdram, 2 * kFace + 6 * kCubeFace, 4096));
    if (!d.standInMemory)
    {
        error = "no CDRAM for the stand-in textures";
        return false;
    }
    std::memset(d.standInMemory, 0, kFace);
    for (uint32_t i = 0; i < kFace; i += 4)
        d.standInMemory[i + 3] = 0xff; // black, opaque
    std::memset(d.standInMemory + kFace, 0xff, kFace);
    std::memset(d.standInMemory + 2 * kFace, 0x80, 6 * kCubeFace);
    if (sceGxmTextureInitLinear(&d.black, d.standInMemory, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 1) < 0 ||
        sceGxmTextureInitLinear(&d.white, d.standInMemory + kFace, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 1) < 0 ||
        sceGxmTextureInitCube(&d.cube, d.standInMemory + 2 * kFace, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 1) < 0)
    {
        error = "the stand-in textures could not be made";
        return false;
    }
    return true;
}

// ---- Clears ------------------------------------------------------------------------------------

const char kClearVertexCg[] = "// KisakCOD: clear quad\n"
                              "void main(float2 in_position, uniform float kisak_clear_depth,\n"
                              "          out float4 out_POSITION : POSITION)\n"
                              "{\n"
                              "    out_POSITION = float4(in_position, kisak_clear_depth, 1.0);\n"
                              "}\n";
const char kClearFragmentCg[] = "// KisakCOD: clear quad\n"
                                "float4 main(uniform float4 kisak_clear_color) : COLOR\n"
                                "{\n"
                                "    return kisak_clear_color;\n"
                                "}\n";

bool MakeClearPrograms(Device &device, std::string &error)
{
    DrawState &d = *device.draw;
    std::string log;
    if (!gxm::GetProgram(kClearVertexCg, gxm::ShaderStage::Vertex, d.clearVertex.gxp, log) ||
        !gxm::GetProgram(kClearFragmentCg, gxm::ShaderStage::Fragment, d.clearFragment.gxp, log))
    {
        error = "the clear programs did not compile: " + log.substr(0, log.find('\n'));
        return false;
    }
    if (!Register(device, d.clearVertex, "clear vertex program") ||
        !Register(device, d.clearFragment, "clear fragment program"))
    {
        error = "the clear programs could not be registered";
        return false;
    }
    const SceGxmProgramParameter *position = sceGxmProgramFindParameterByName(d.clearVertex.program(), "in_position");
    d.clearDepth = sceGxmProgramFindParameterByName(d.clearVertex.program(), "kisak_clear_depth");
    d.clearColor = sceGxmProgramFindParameterByName(d.clearFragment.program(), "kisak_clear_color");
    if (!position || !d.clearDepth || !d.clearColor)
    {
        error = "the clear programs lack their parameters";
        return false;
    }
    SceGxmVertexAttribute attribute{};
    attribute.streamIndex = 0;
    attribute.offset = 0;
    attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attribute.componentCount = 2;
    attribute.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(position));
    SceGxmVertexStream stream{8, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    const int result = sceGxmShaderPatcherCreateVertexProgram(gxm::ShaderPatcher(), d.clearVertex.id, &attribute, 1,
                                                              &stream, 1, &d.clearVertexProgram);
    if (result < 0)
    {
        char line[96];
        std::snprintf(line, sizeof(line), "the clear vertex program could not be made (0x%08x)", unsigned(result));
        error = line;
        return false;
    }
    return true;
}

const char kCopyVertexCg[] = "// KisakCOD: StretchRect quad\n"
                             "void main(float2 in_position, float2 in_texcoord,\n"
                             "          out float4 out_POSITION : POSITION, out float2 out_TEXCOORD0 : TEXCOORD0)\n"
                             "{\n"
                             "    out_POSITION = float4(in_position, 0.0, 1.0);\n"
                             "    out_TEXCOORD0 = in_texcoord;\n"
                             "}\n";
const char kCopyFragmentCg[] = "// KisakCOD: StretchRect quad\n"
                               "float4 main(float2 in_TEXCOORD0 : TEXCOORD0, uniform sampler2D s0) : COLOR\n"
                               "{\n"
                               "    return tex2D(s0, in_TEXCOORD0);\n"
                               "}\n";

bool MakeCopyPrograms(Device &device, std::string &error)
{
    DrawState &d = *device.draw;
    std::string log;
    if (!gxm::GetProgram(kCopyVertexCg, gxm::ShaderStage::Vertex, d.copyVertex.gxp, log) ||
        !gxm::GetProgram(kCopyFragmentCg, gxm::ShaderStage::Fragment, d.copyFragment.gxp, log))
    {
        error = "the copy programs did not compile: " + log.substr(0, log.find('\n'));
        return false;
    }
    if (!Register(device, d.copyVertex, "copy vertex program") ||
        !Register(device, d.copyFragment, "copy fragment program"))
    {
        error = "the copy programs could not be registered";
        return false;
    }
    const SceGxmProgramParameter *position = sceGxmProgramFindParameterByName(d.copyVertex.program(), "in_position");
    const SceGxmProgramParameter *texcoord = sceGxmProgramFindParameterByName(d.copyVertex.program(), "in_texcoord");
    d.copyUnit = d.copyFragment.textureUnit[0];
    if (!position || !texcoord || d.copyUnit < 0)
    {
        error = "the copy programs lack their parameters";
        return false;
    }
    SceGxmVertexAttribute attributes[2]{};
    for (int i = 0; i < 2; ++i)
    {
        attributes[i].streamIndex = 0;
        attributes[i].offset = uint16_t(i * 8);
        attributes[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        attributes[i].componentCount = 2;
        attributes[i].regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(i ? texcoord : position));
    }
    SceGxmVertexStream stream{16, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    const int result = sceGxmShaderPatcherCreateVertexProgram(gxm::ShaderPatcher(), d.copyVertex.id, attributes, 2,
                                                              &stream, 1, &d.copyVertexProgram);
    if (result < 0)
    {
        char line[96];
        std::snprintf(line, sizeof(line), "the copy vertex program could not be made (0x%08x)", unsigned(result));
        error = line;
        return false;
    }
    return true;
}

SceGxmFragmentProgram *ClearFragmentProgram(Device &device, bool writeColor)
{
    DrawState &d = *device.draw;
    SceGxmBlendInfo blend{};
    blend.colorMask = writeColor ? SCE_GXM_COLOR_MASK_ALL : SCE_GXM_COLOR_MASK_NONE;
    blend.colorFunc = blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    blend.colorSrc = blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.colorDst = blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    const FragmentProgramEntry *entry =
        GetFragmentProgram(device, d.clearFragment, d.clearVertex, writeColor ? nullptr : &blend);
    return entry ? entry->program : nullptr;
}

void SetPrograms(Device &device, const SceGxmVertexProgram *vertex, const SceGxmFragmentProgram *fragment)
{
    DrawState &d = *device.draw;
    if (d.boundVertex != vertex)
    {
        sceGxmSetVertexProgram(Gxm(), vertex);
        d.boundVertex = vertex;
    }
    if (d.boundFragment != fragment)
    {
        sceGxmSetFragmentProgram(Gxm(), fragment);
        d.boundFragment = fragment;
    }
}

uint32_t IndexCount(d3d::PrimitiveType type, uint32_t primitives)
{
    switch (type)
    {
    case d3d::PT_POINTLIST: return primitives;
    case d3d::PT_LINELIST: return primitives * 2;
    case d3d::PT_TRIANGLELIST: return primitives * 3;
    case d3d::PT_TRIANGLESTRIP:
    case d3d::PT_TRIANGLEFAN: return primitives + 2;
    default: return 0;
    }
}

bool Primitive(d3d::PrimitiveType type, SceGxmPrimitiveType &out)
{
    switch (type)
    {
    case d3d::PT_POINTLIST: out = SCE_GXM_PRIMITIVE_POINTS; return true;
    case d3d::PT_LINELIST: out = SCE_GXM_PRIMITIVE_LINES; return true;
    case d3d::PT_TRIANGLELIST: out = SCE_GXM_PRIMITIVE_TRIANGLES; return true;
    case d3d::PT_TRIANGLESTRIP: out = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; return true;
    case d3d::PT_TRIANGLEFAN: out = SCE_GXM_PRIMITIVE_TRIANGLE_FAN; return true;
    default: return false;
    }
}

void DropProgramsIf(Device &device, const std::function<bool(const VertexProgramEntry &)> &vertexMatch,
                    const std::function<bool(const FragmentProgramEntry &)> &fragmentMatch)
{
    DrawState &d = *device.draw;
    for (auto it = d.vertexPrograms.begin(); it != d.vertexPrograms.end();)
    {
        if (!vertexMatch(it->second))
        {
            ++it;
            continue;
        }
        if (it->second.program)
            ReleaseVertexProgram(device, it->second.program);
        if (d.boundVertex == it->second.program)
            d.boundVertex = nullptr;
        it = d.vertexPrograms.erase(it);
    }
    for (auto it = d.fragmentPrograms.begin(); it != d.fragmentPrograms.end();)
    {
        if (!fragmentMatch || !fragmentMatch(it->second))
        {
            ++it;
            continue;
        }
        if (it->second.program)
            ReleaseFragmentProgram(device, it->second.program);
        if (d.boundFragment == it->second.program)
            d.boundFragment = nullptr;
        it = d.fragmentPrograms.erase(it);
    }
}
} // namespace

// ---- The interface (gxm_legacy_internal.h) ----------------------------------------------------

bool InitDraw(Device &device, std::string &error)
{
    device.draw = new DrawState;
    DrawState &d = *device.draw;
    d.sequential = static_cast<uint16_t *>(
        gxm::Allocate(gxm::MemoryKind::Main, kSequentialIndices * sizeof(uint16_t), 16));
    d.ring = static_cast<uint8_t *>(gxm::Allocate(gxm::MemoryKind::Main, kRingSegment * kRingSegments, 4096));
    if (!d.sequential || !d.ring)
    {
        error = "no GPU main memory for the draw buffers";
        return false;
    }
    for (uint32_t i = 0; i < kSequentialIndices; ++i)
        d.sequential[i] = uint16_t(i);
    if (!MakeStandIns(device, error) || !MakeClearPrograms(device, error) || !MakeCopyPrograms(device, error))
        return false;
    // Run diagnostics: opaque green (no inputs, so it links with any vertex
    // program). Not having it only loses that test.
    std::string log;
    if (!gxm::GetProgram("// KisakCOD: diagnostics\nfloat4 main() : COLOR\n{\n    return float4(0.0, 1.0, 0.0, 1.0);\n}\n",
                         gxm::ShaderStage::Fragment, d.diagGreen.gxp, log))
        Log(device, "diag: the green program did not compile: %s", log.substr(0, log.find('\n')).c_str());
    Register(device, d.diagGreen, "diag green program");
    if (!gxm::GetProgram("// KisakCOD: diagnostics\nvoid main(float4 in_position, out float4 out_POSITION : POSITION)\n{\n"
                         "    out_POSITION = float4(in_position.x / 480.0 - 1.0, 1.0 - in_position.y / 272.0, 0.5, 1.0);\n}\n",
                         gxm::ShaderStage::Vertex, d.diagPass.gxp, log))
        Log(device, "diag: the pass-through program did not compile: %s", log.substr(0, log.find('\n')).c_str());
    if (Register(device, d.diagPass, "diag pass-through program"))
    {
        d.diagPassPosition = sceGxmProgramFindParameterByName(d.diagPass.program(), "in_position");
        if (!d.diagPassPosition)
            d.diagPass.failed = true;
    }
    return true;
}

void ShutdownDraw(Device &device)
{
    DrawState *d = device.draw;
    if (!d)
        return;
    // The GPU is idle: everything deferred can go now. The pools and the
    // shader patcher go whole with the display.
    for (auto &[frame, work] : d->deferred)
        work();
    delete d;
    device.draw = nullptr;
}

uint32_t LayoutTexture(Texture &texture)
{
    const FormatInfo &format = *texture.format;
    const TextureDesc &desc = texture.desc;
    const bool renderTarget = (desc.usage & d3d::USAGE_RENDERTARGET) != 0;
    const uint32_t block = format.compressed ? 4 : 1;
    const uint32_t depth = desc.kind == TextureKind::Volume ? desc.depth : 1;
    const uint32_t sampledLevels = SampledLevels(texture);
    // sceGxm sizes the levels of a mipmapped linear texture from the power
    // of two above the base, its rows rounded up to 8 texels.
    const bool powerOfTwo = sampledLevels > 1;
    const uint32_t layoutWidth = powerOfTwo ? NextPowerOfTwo(desc.width) : desc.width;
    const uint32_t layoutHeight = powerOfTwo ? NextPowerOfTwo(desc.height) : desc.height;
    uint32_t total = 0;
    for (uint32_t face = 0; face < texture.faces; ++face)
    {
        total = AlignUp(total, 256);
        for (uint32_t i = 0; i < desc.levels; ++i)
        {
            Level &level = texture.levels[face * desc.levels + i];
            level.width = LevelSize(desc.width, i);
            level.height = LevelSize(desc.height, i);
            level.depth = LevelSize(depth, i);
            uint32_t bytes;
            if (format.depth)
            {
                // Tiled depth and stencil, 32x32 tiles.
                level.pitch = AlignUp(level.width, 32) * 4;
                level.slicePitch = level.pitch * AlignUp(level.height, 32);
                bytes = level.slicePitch;
            }
            else if (renderTarget)
            {
                level.pitch = Pitch(format, level.width, true);
                level.slicePitch = level.pitch * Rows(format, level.height);
                bytes = level.slicePitch;
            }
            else
            {
                level.pitch = AlignUp(level.width, 8) / block * format.blockBytes;
                level.slicePitch = level.pitch * Rows(format, level.height);
                bytes = level.slicePitch * level.depth;
                if (i < sampledLevels && powerOfTwo)
                {
                    const uint32_t w = AlignUp(std::max(1u, layoutWidth >> i), 8);
                    const uint32_t h = AlignUp(std::max(1u, layoutHeight >> i), block);
                    bytes = std::max(bytes, w / block * (h / block) * format.blockBytes);
                }
            }
            // Levels sceGxm reads follow each other with no gap; the rest
            // (levels it does not sample) only need aligned rows.
            if (i >= sampledLevels)
                total = AlignUp(total, 16);
            level.offset = total;
            total += bytes;
        }
    }
    return total;
}

namespace
{
uint32_t Part1By1(uint32_t x)
{
    x &= 0x0000ffff;
    x = (x ^ (x << 8)) & 0x00ff00ff;
    x = (x ^ (x << 4)) & 0x0f0f0f0f;
    x = (x ^ (x << 2)) & 0x33333333;
    x = (x ^ (x << 1)) & 0x55555555;
    return x;
}

uint32_t Log2(uint32_t powerOfTwo)
{
    uint32_t bits = 0;
    while (powerOfTwo > 1)
    {
        powerOfTwo >>= 1;
        ++bits;
    }
    return bits;
}

// A texel's (or 4x4 block's) place in sceGxm's swizzled order, on a
// width x height grid (powers of two): Morton order over the largest square,
// y in the even bits and x in the odd ones, and the squares one after
// another along the longer side.
uint32_t Morton(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const uint32_t side = std::min(width, height);
    const uint32_t k = Log2(side);
    return ((x >> k) | (y >> k)) << (2 * k) | Part1By1(x & (side - 1)) << 1 | Part1By1(y & (side - 1));
}

// Blocks (or texels) across one side of a swizzled level.
uint32_t GridSide(const FormatInfo &format, uint32_t layoutSide, uint32_t level)
{
    const uint32_t block = format.compressed ? 4 : 1;
    return AlignUp(std::max(1u, layoutSide >> level), block) / block;
}

// sceGxm's swizzled layouts. A 2D texture's sampled levels follow each other,
// each sized from the power of two above the base. Each cube face holds every
// level down to 1x1 whatever the texture has, and faces of larger textures
// start on 2 KB.
bool LayOutSwizzled(Texture &texture)
{
    const FormatInfo &format = *texture.format;
    const TextureDesc &desc = texture.desc;
    if (desc.width > 4096 || desc.height > 4096)
        return false;
    const bool cube = desc.kind == TextureKind::Cube;
    if (cube && (desc.width & (desc.width - 1)))
        return false;
    texture.layoutWidth = NextPowerOfTwo(desc.width);
    texture.layoutHeight = NextPowerOfTwo(desc.height);
    const uint32_t levels = cube ? Log2(texture.layoutWidth) + 1 : SampledLevels(texture);
    uint32_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level)
    {
        if (level < 14)
            texture.mipOffset[level] = offset;
        offset += GridSide(format, texture.layoutWidth, level) * GridSide(format, texture.layoutHeight, level) *
                  format.blockBytes;
    }
    if (!cube)
    {
        texture.faceStride = AlignUp(offset, 4);
        return true;
    }
    const uint32_t size = desc.width;
    const uint32_t bits = format.compressed ? format.blockBytes * 8 / 16 : format.blockBytes * 8;
    const bool wide = (size >= 32 && (bits <= 8 || format.compressed)) || (size >= 16 && (bits == 16 || bits == 32)) ||
                      (size >= 8 && bits == 64);
    texture.faceStride = AlignUp(offset, wide ? 2048 : 4);
    return true;
}

// Calls f(linear byte offset, swizzled byte offset) for each block of a level
// D3D9 holds.
template <typename F> bool ForEachSwizzledBlock(const Texture &texture, uint32_t face, uint32_t level, uint32_t pitch, F f)
{
    if (!texture.swizzled.data || face >= 6 || level >= texture.desc.levels || level >= 14)
        return false;
    const FormatInfo &format = *texture.format;
    if (texture.desc.kind != TextureKind::Cube && level >= SampledLevels(texture))
        return false; // sceGxm never reads it
    const Level &l = texture.levels[face * texture.desc.levels + level];
    const uint32_t block = format.compressed ? 4 : 1;
    const uint32_t across = AlignUp(l.width, block) / block, down = AlignUp(l.height, block) / block;
    const uint32_t gridWidth = GridSide(format, texture.layoutWidth, level);
    const uint32_t gridHeight = GridSide(format, texture.layoutHeight, level);
    const size_t base = size_t(face) * texture.faceStride + texture.mipOffset[level];
    const uint32_t bytes = format.blockBytes;
    for (uint32_t y = 0; y < down && y < gridHeight; ++y)
        for (uint32_t x = 0; x < across && x < gridWidth; ++x)
            f(size_t(y) * pitch + size_t(x) * bytes, base + size_t(Morton(x, y, gridWidth, gridHeight)) * bytes);
    return true;
}
} // namespace

bool SwizzledTexture(const TextureDesc &desc, const FormatInfo &format)
{
    if (desc.pool == d3d::POOL_SYSTEMMEM || desc.pool == d3d::POOL_SCRATCH || format.depth ||
        (desc.usage & d3d::USAGE_RENDERTARGET))
        return false;
    return desc.kind == TextureKind::Cube || (desc.kind == TextureKind::Tex2D && format.compressed);
}

void WriteSwizzled(Texture &texture, uint32_t face, uint32_t level, const uint8_t *from, uint32_t pitch)
{
    const uint32_t bytes = texture.format->blockBytes;
    uint8_t *to = texture.swizzled.data;
    ForEachSwizzledBlock(texture, face, level, pitch,
                         [&](size_t linear, size_t swizzled) { std::memcpy(to + swizzled, from + linear, bytes); });
}

void ReadSwizzled(const Texture &texture, uint32_t face, uint32_t level, uint8_t *to, uint32_t pitch)
{
    const uint32_t bytes = texture.format->blockBytes;
    const uint8_t *from = texture.swizzled.data;
    ForEachSwizzledBlock(texture, face, level, pitch,
                         [&](size_t linear, size_t swizzled) { std::memcpy(to + linear, from + swizzled, bytes); });
}

void SetUpTexture(Device &device, Texture &texture)
{
    texture.sampleable = false;
    const TextureDesc &desc = texture.desc;
    if (texture.swizzledLayout)
    {
        SceGxmTextureFormat format;
        if (!TextureFormat(desc.format, format) || !LayOutSwizzled(texture))
            return;
        const uint32_t bytes = texture.faceStride * texture.faces;
        if (!AllocateMemory(device, texture.swizzled, bytes, 4096, d3d::POOL_DEFAULT, true, texture.name.c_str()))
            return;
        std::memset(texture.swizzled.data, desc.kind == TextureKind::Cube ? 0x80 : 0, bytes);
        int result;
        const char *call;
        if (desc.kind == TextureKind::Cube)
        {
            call = "sceGxmTextureInitCube";
            result = sceGxmTextureInitCube(&texture.sampled, texture.swizzled.data, format, desc.width, desc.width,
                                           std::min(desc.levels, 13u));
        }
        else if (texture.layoutWidth == desc.width && texture.layoutHeight == desc.height)
        {
            call = "sceGxmTextureInitSwizzled";
            result = sceGxmTextureInitSwizzled(&texture.sampled, texture.swizzled.data, format, desc.width,
                                               desc.height, SampledLevels(texture));
        }
        else
        {
            call = "sceGxmTextureInitSwizzledArbitrary";
            result = sceGxmTextureInitSwizzledArbitrary(&texture.sampled, texture.swizzled.data, format, desc.width,
                                                        desc.height, SampledLevels(texture));
        }
        if (result < 0)
        {
            Log(device, "%s for %s (%ux%u, format %u) failed (0x%08x)", call, texture.name.c_str(), desc.width,
                desc.height, unsigned(desc.format), unsigned(result));
            return;
        }
        texture.sampleable = true;
        return;
    }
    if (!texture.memory.gpu)
        return;
    if (texture.format->depth)
    {
        const int result = sceGxmDepthStencilSurfaceInit(&texture.depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                                         SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                                         AlignUp(desc.width, 32), texture.memory.data, nullptr);
        if (result < 0)
            Log(device, "sceGxmDepthStencilSurfaceInit for %s failed (0x%08x)", texture.name.c_str(), unsigned(result));
        // Scenes keep what earlier scenes left in it.
        sceGxmDepthStencilSurfaceSetForceLoadMode(&texture.depthSurface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&texture.depthSurface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        return;
    }
    SceGxmTextureFormat format;
    if (desc.kind == TextureKind::Cube || !TextureFormat(desc.format, format) || desc.width > 4096 ||
        desc.height > 4096)
        return;
    const Level &base = texture.levels[0];
    int result;
    if (desc.usage & d3d::USAGE_RENDERTARGET)
        result = sceGxmTextureInitLinearStrided(&texture.sampled, texture.memory.data, format, desc.width, desc.height,
                                                base.pitch);
    else if (desc.kind == TextureKind::Volume)
    {
        // The slices of level 0, one under the other (see the translator's
        // kisak_tex3D).
        if (desc.height * desc.depth > 4096)
            return;
        result = sceGxmTextureInitLinear(&texture.sampled, texture.memory.data, format, desc.width,
                                         desc.height * desc.depth, 1);
    }
    else
        result = sceGxmTextureInitLinear(&texture.sampled, texture.memory.data, format, desc.width, desc.height,
                                         SampledLevels(texture));
    if (result < 0)
    {
        Log(device, "sceGxmTextureInit for %s (%ux%u, format %u) failed (0x%08x)", texture.name.c_str(), desc.width,
            desc.height, unsigned(desc.format), unsigned(result));
        return;
    }
    texture.sampleable = true;
}

void Retire(Device &device, Memory &memory)
{
    if (memory.gpu && device.draw)
    {
        const gxm::MemoryKind kind = memory.kind;
        void *data = memory.data;
        Defer(device, [kind, data] { gxm::Free(kind, data); });
        memory = Memory{};
        return;
    }
    FreeMemory(memory);
}

void EndScene(Device &device)
{
    DrawState *d = device.draw;
    if (!d || !d->sceneOpen)
        return;
    d->sceneOpen = false;
    d->sceneColor = d->sceneDepth = nullptr;
    const int result = sceGxmEndScene(Gxm(), nullptr, nullptr);
    if (result < 0)
        Log(device, "sceGxmEndScene failed (0x%08x)", unsigned(result));
}

void FinishGpu(Device &device)
{
    EndScene(device);
    gxm::Finish();
}

void FrameDone(Device &device)
{
    DrawState &d = *device.draw;
    ++d.frame;
    d.ringUsed = 0;
    for (const uint64_t frame : kDiagnosedFrames)
        if (d.frame == frame)
            d.diagnosedDraws = 8;
    const int mode = DiagModeFor(d.frame);
    if (mode != d.diagMode)
        Log(device, "diag: frame %llu, test %d (1 pass-through, 2 copied vertices, 3 both, 4 green, 0 none)",
            (unsigned long long)d.frame, mode);
    d.diagMode = mode;
    // Work recorded in frame F was queued before Present F + 1, and after
    // Present P the frames before P - 1 have been shown.
    size_t done = 0;
    while (done < d.deferred.size() && d.deferred[done].first + kFramesInFlight <= d.frame)
        d.deferred[done++].second();
    d.deferred.erase(d.deferred.begin(), d.deferred.begin() + done);
    // Progress for the run logs: the first frame, ten seconds in, then every
    // minute.
    if (d.frame == 1 || d.frame == 600 || d.frame % 3600 == 0)
    {
        Log(device, "frame %llu:", (unsigned long long)d.frame);
        LogDrawSummary(device);
    }
}

void ForgetShader(Device &device, Shader &shader)
{
    if (!device.draw || shader.variants.empty())
        return;
    auto ours = [&](const Variant *variant) {
        for (const auto &v : shader.variants)
            if (v.get() == variant)
                return true;
        return false;
    };
    DropProgramsIf(
        device, [&](const VertexProgramEntry &e) { return ours(e.variant); },
        [&](const FragmentProgramEntry &e) { return ours(e.pixel) || ours(e.vertex); });
    // The programs go, then their registrations, then the code they point at.
    for (auto &variant : shader.variants)
    {
        Variant *owned = variant.release();
        Defer(device, [owned] {
            if (owned->id)
                sceGxmShaderPatcherUnregisterProgram(gxm::ShaderPatcher(), owned->id);
            delete owned;
        });
    }
    shader.variants.clear();
}

void ForgetDecl(Device &device, VertexDecl &decl)
{
    if (device.draw)
        DropProgramsIf(device, [&](const VertexProgramEntry &e) { return e.decl == &decl; }, nullptr);
}

void Rename(Device &device, GpuBuffer &buffer)
{
    Memory fresh;
    const bool dynamic = (buffer.usage & d3d::USAGE_DYNAMIC) != 0;
    if (!AllocateMemory(device, fresh, buffer.size, 16, buffer.pool, !dynamic, buffer.name.c_str()))
    {
        // No memory: wait for the GPU instead.
        FinishGpu(device);
        buffer.drawn = false;
        return;
    }
    Retire(device, buffer.memory);
    buffer.memory = std::move(fresh);
    buffer.drawn = false;
    ++device.draw->renames;
    ++device.stats.dynamicRenames;
}

void ClearTargets(Device &device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
                  uint32_t stencil)
{
    if (!OpenScene(device))
    {
        CountSkip(device, SkipNoScene);
        return;
    }
    DrawState &d = *device.draw;
    const bool writeColor = (flags & d3d::CLEAR_TARGET) && d.hasColor;
    const bool writeDepth = (flags & d3d::CLEAR_ZBUFFER) && d.sceneDepth;
    const bool writeStencil = (flags & d3d::CLEAR_STENCIL) && d.sceneDepth;
    if (!writeColor && !writeDepth && !writeStencil)
        return;
    SceGxmFragmentProgram *fragment = ClearFragmentProgram(device, writeColor);
    if (!fragment)
        return;
    uint32_t area[4];
    if (!DrawArea(device, area))
        return;

    FixedState state{};
    const float w = float(d.sceneWidth), h = float(d.sceneHeight);
    state.viewport[0] = w * 0.5f;
    state.viewport[1] = w * 0.5f;
    state.viewport[2] = h * 0.5f;
    state.viewport[3] = -h * 0.5f;
    state.viewport[4] = 0.0f;
    state.viewport[5] = 1.0f;
    std::copy(area, area + 4, state.clip);
    state.cull = SCE_GXM_CULL_NONE;
    state.polygon = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    state.depthFunc = SCE_GXM_DEPTH_FUNC_ALWAYS;
    state.depthWrite = writeDepth ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    for (int side = 0; side < 2; ++side)
    {
        state.stencilFunc[side] = SCE_GXM_STENCIL_FUNC_ALWAYS;
        state.stencilFail[side] = state.stencilDepthFail[side] = SCE_GXM_STENCIL_OP_KEEP;
        state.stencilPass[side] = writeStencil ? SCE_GXM_STENCIL_OP_REPLACE : SCE_GXM_STENCIL_OP_KEEP;
    }
    state.stencilMask = 0xff;
    state.stencilWriteMask = writeStencil ? 0xff : 0;
    state.stencilRef = uint8_t(stencil);
    ApplyFixedState(device, state);
    SetPrograms(device, d.clearVertexProgram, fragment);

    SceGxmContext *context = Gxm();
    const float depth = std::clamp(z, 0.0f, 1.0f);
    const float rgba[4] = {float((color >> 16) & 0xff) / 255.0f, float((color >> 8) & 0xff) / 255.0f,
                           float(color & 0xff) / 255.0f, float(color >> 24) / 255.0f};
    // The rects, cut by the area; none: the area itself.
    const uint32_t count = rects && rectCount ? rectCount : 1;
    for (uint32_t i = 0; i < count; ++i)
    {
        int32_t left = int32_t(area[0]), top = int32_t(area[1]);
        int32_t right = int32_t(area[2]) + 1, bottom = int32_t(area[3]) + 1;
        if (rects && rectCount)
        {
            left = std::max(left, rects[i].left);
            top = std::max(top, rects[i].top);
            right = std::min(right, rects[i].right);
            bottom = std::min(bottom, rects[i].bottom);
        }
        if (right <= left || bottom <= top)
            continue;
        float *quad = static_cast<float *>(Transient(device, 8 * sizeof(float)));
        if (!quad)
            return;
        const float x0 = 2.0f * left / w - 1.0f, x1 = 2.0f * right / w - 1.0f;
        const float y0 = 1.0f - 2.0f * top / h, y1 = 1.0f - 2.0f * bottom / h;
        const float corners[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
        std::memcpy(quad, corners, sizeof(corners));
        void *uniforms = nullptr;
        sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, d.clearDepth, 0, 1, &depth);
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, d.clearColor, 0, 4, rgba);
        sceGxmSetVertexStream(context, 0, quad);
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, d.sequential, 4);
    }
}

namespace
{
// ---- Run diagnostics -------------------------------------------------------------------------
//
// Stage 2d's first runs drew a black screen with no errors. These log what a
// few draws ask for and what lands in the back buffer, so a run tells a
// display problem from a draw-state one:
// - frames 300 and 1200: the first 8 draws' state, the first vertex's
//   position and the first vertex shader constants;
// - frames 300, 700 and 1200: what the back buffer holds before it is shown;
// - frames 600 to 779: a magenta rectangle (the top-left quarter) drawn
//   through the clear path after the game's frame, so it shows on screen
//   for three seconds and in frame 700's count.


void DiagnoseDraw(Device &device, d3d::PrimitiveType type, uint32_t indexCount, const void *indexData, bool index32,
                  int32_t baseVertex, const void *upVertices, uint32_t upStride, const FixedState &state,
                  const Variant &vertex, const Variant &pixel, const SceGxmBlendInfo *blend)
{
    const DrawState &d = *device.draw;
    const uint32_t *rs = device.renderStates;
    const Viewport &vp = device.viewport;
    const Surface *target = device.renderTargets[0];
    const char *targetKind = !target ? "none" : target->kind == SurfaceKind::BackBuffer ? "back buffer"
                                              : target->kind == SurfaceKind::FrontBuffer ? "front buffer"
                                                                                         : "texture";
    Log(device,
        "diag frame %llu draw: type %u, %u indices%s; target %s, scene %ux%u%s; viewport %u,%u %ux%u z %.2f-%.2f "
        "-> gxm %.1f %.1f %.1f %.1f %.2f %.2f; clip %u,%u-%u,%u; cull %u (d3d %u); z %u func %u write %u; stencil %u",
        (unsigned long long)d.frame, unsigned(type), indexCount, indexData ? (index32 ? " (32-bit)" : " (16-bit)") : "",
        targetKind, d.sceneWidth, d.sceneHeight, d.sceneDepth ? " with depth" : "", vp.x, vp.y, vp.width, vp.height,
        vp.minZ, vp.maxZ, state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3],
        state.viewport[4], state.viewport[5], state.clip[0], state.clip[1], state.clip[2], state.clip[3],
        unsigned(state.cull), rs[d3d::RS_CULLMODE], rs[d3d::RS_ZENABLE], rs[d3d::RS_ZFUNC], rs[d3d::RS_ZWRITEENABLE],
        rs[d3d::RS_STENCILENABLE]);
    Log(device,
        "diag frame %llu draw: blend %s (d3d on %u, src %u dst %u op %u), write mask d3d 0x%x gxm 0x%x; alpha test %u "
        "func %u ref %u; vs %u-%u constants %u (parameter %s, uniforms %s), ps %u-%u constants %u (parameter %s)",
        (unsigned long long)d.frame, blend ? "on" : "off", rs[d3d::RS_ALPHABLENDENABLE], rs[d3d::RS_SRCBLEND],
        rs[d3d::RS_DESTBLEND], rs[d3d::RS_BLENDOP], rs[d3d::RS_COLORWRITEENABLE], blend ? unsigned(blend->colorMask) : 0xfu,
        rs[d3d::RS_ALPHATESTENABLE], rs[d3d::RS_ALPHAFUNC], rs[d3d::RS_ALPHAREF],
        unsigned(device.vertexShader->parsed.reflection.major), unsigned(device.vertexShader->parsed.reflection.minor),
        unsigned(device.vertexShader->parsed.reflection.floatConstants), vertex.constants ? "found" : "MISSING",
        vertex.uniforms ? "yes" : "no", unsigned(device.pixelShader->parsed.reflection.major),
        unsigned(device.pixelShader->parsed.reflection.minor),
        unsigned(device.pixelShader->parsed.reflection.floatConstants), pixel.constants ? "found" : "MISSING");
    const float *c = device.vsConstants.data();
    Log(device,
        "diag frame %llu draw: vs c0 %g %g %g %g, c1 %g %g %g %g, c2 %g %g %g %g, c3 %g %g %g %g",
        (unsigned long long)d.frame, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12],
        c[13], c[14], c[15]);
    // The first vertex's position element, as the draw reads it.
    const VertexElement *position = nullptr;
    for (const VertexElement &e : device.decl->elements)
        if (e.usage == d3d::DECLUSAGE_POSITION && e.usageIndex == 0)
            position = &e;
    if (!position)
    {
        Log(device, "diag frame %llu draw: the declaration has no POSITION0", (unsigned long long)d.frame);
        return;
    }
    uint32_t first = 0;
    if (indexData)
        first = index32 ? *static_cast<const uint32_t *>(indexData) : *static_cast<const uint16_t *>(indexData);
    const uint8_t *base = nullptr;
    uint32_t stride = 0;
    if (upVertices && position->stream == 0)
    {
        base = static_cast<const uint8_t *>(upVertices);
        stride = upStride;
    }
    else if (const VertexBuffer *vb = device.streams[position->stream].buffer; vb && vb->memory.data)
    {
        stride = device.streams[position->stream].stride;
        base = vb->memory.data + device.streams[position->stream].offset + int64_t(baseVertex) * stride;
    }
    if (!base)
        return;
    const uint8_t *at = base + size_t(first) * stride + position->offset;
    float p[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const uint32_t n = position->type == d3d::DECLTYPE_FLOAT2   ? 2
                       : position->type == d3d::DECLTYPE_FLOAT3 ? 3
                       : position->type == d3d::DECLTYPE_FLOAT4 ? 4
                                                                : 0;
    std::memcpy(p, at, n * sizeof(float));
    Log(device, "diag frame %llu draw: vertex %u position (type %u, stride %u) %g %g %g %g", (unsigned long long)d.frame,
        first, unsigned(position->type), stride, p[0], p[1], p[2], p[3]);
}
} // namespace

void Draw(Device &device, d3d::PrimitiveType type, int32_t baseVertex, uint32_t startIndex, uint32_t primitiveCount,
          bool indexed, const void *upVertices, uint32_t upStride)
{
    DrawState &d = *device.draw;
    Shader *vs = device.vertexShader;
    Shader *ps = device.pixelShader;
    if (!vs || !ps)
    {
        CountSkip(device, SkipNoShader);
        return;
    }
    if (!vs->translated || !ps->translated)
    {
        CountSkip(device, SkipNotTranslated);
        return;
    }
    if (!device.decl)
    {
        CountSkip(device, SkipNoDecl);
        return;
    }
    SceGxmPrimitiveType primitive;
    const uint32_t indexCount = IndexCount(type, primitiveCount);
    if (!Primitive(type, primitive) || !indexCount)
    {
        CountSkip(device, SkipPrimitive);
        return;
    }

    // Indices.
    const void *indexData = d.sequential;
    bool index32 = false;
    if (indexed)
    {
        IndexBuffer *ib = device.indices;
        if (!ib || !ib->memory.gpu)
        {
            CountSkip(device, SkipNoIndices);
            return;
        }
        index32 = ib->indexFormat == d3d::FMT_INDEX32;
        const uint32_t bytes = index32 ? 4 : 2;
        if ((startIndex + indexCount) * bytes > ib->size)
        {
            CountSkip(device, SkipNoIndices, "the draw reads past the index buffer");
            return;
        }
        indexData = ib->memory.data + startIndex * bytes;
        ib->drawn = true;
    }
    else if (indexCount > kSequentialIndices)
    {
        CountSkip(device, SkipPrimitive, "a draw without indices of more than 60000 vertices");
        return;
    }

    // Programs.
    InputPlan plan;
    PlanInputs(device, *vs, *device.decl, plan);
    const int diag = d.diagMode;
    Variant *vertex = GetVariant(device, *vs, plan.options);
    gxm::cg::Options pixelOptions;
    if (device.renderStates[d3d::RS_ALPHATESTENABLE])
    {
        const uint32_t func = device.renderStates[d3d::RS_ALPHAFUNC];
        if (func >= d3d::CMP_NEVER && func < d3d::CMP_ALWAYS)
            pixelOptions.alphaFunc = gxm::cg::AlphaFunc(func);
    }
    Variant *pixel = GetVariant(device, *ps, pixelOptions);
    if (!vertex || !pixel)
    {
        CountSkip(device, SkipNoProgram);
        return;
    }
    if (diag != DiagNone && !d.diagGreen.failed)
        pixel = &d.diagGreen;
    const bool passThrough = (diag == DiagPassThrough || diag == DiagPassExpand) && !d.diagPass.failed;
    if (!OpenScene(device))
    {
        CountSkip(device, SkipNoScene);
        return;
    }
    if (!d.hasColor)
    {
        NotYet(device, "drawing with no colour target (depth only; skipped)");
        return;
    }
    uint32_t strides[kMaxStreams];
    for (uint32_t s = 0; s < kMaxStreams; ++s)
        strides[s] = device.streams[s].stride;
    if (upVertices)
        strides[0] = upStride;
    const VertexProgramEntry *vertexProgram = GetVertexProgram(device, *vertex, *device.decl, plan, index32, strides);
    if (!vertexProgram)
        return;
    SceGxmBlendInfo blend{};
    const bool blended = pixel != &d.diagGreen && MakeBlend(device, d.color.noAlpha, blend);
    const FragmentProgramEntry *fragmentProgram =
        GetFragmentProgram(device, *pixel, passThrough ? d.diagPass : *vertex, blended ? &blend : nullptr);
    if (!fragmentProgram)
        return;

    // Vertex data.
    const void *streamData[kMaxStreams];
    for (uint8_t i = 0; i < vertexProgram->streamCount; ++i)
    {
        const uint32_t s = vertexProgram->streams[i];
        if (upVertices && s == 0)
        {
            // The highest index a draw without indices reads.
            const uint32_t bytes = indexCount * upStride;
            void *copy = Transient(device, bytes);
            if (!copy)
            {
                CountSkip(device, SkipNoStream, "no memory for the vertices of a DrawPrimitiveUP");
                return;
            }
            std::memcpy(copy, upVertices, bytes);
            streamData[i] = copy;
            continue;
        }
        VertexBuffer *vb = device.streams[s].buffer;
        if (!vb || !vb->memory.gpu)
        {
            CountSkip(device, SkipNoStream);
            return;
        }
        vb->drawn = true;
        streamData[i] = vb->memory.data + int64_t(device.streams[s].offset) + int64_t(baseVertex) * strides[s];
    }

    FixedState state;
    MakeFixedState(device, type, state);
    if (!DrawArea(device, state.clip))
    {
        CountSkip(device, SkipEmptyClip);
        return;
    }
    ApplyFixedState(device, state);

    // Run-diagnostic tests: vertices copied out of the game's buffers, and a
    // pass-through vertex program.
    const SceGxmVertexProgram *vertexGxm = vertexProgram->program;
    const void *drawIndices = indexData;
    bool drawIndex32 = index32;
    uint8_t drawStreams = vertexProgram->streamCount;
    if ((diag == DiagExpand || diag == DiagPassExpand) && indexCount <= kSequentialIndices)
    {
        bool copied = true;
        for (uint8_t i = 0; i < vertexProgram->streamCount && copied; ++i)
        {
            const uint32_t stride = strides[vertexProgram->streams[i]];
            uint8_t *copy = static_cast<uint8_t *>(Transient(device, indexCount * stride + 16));
            if (!copy)
            {
                copied = false;
                break;
            }
            const uint8_t *from = static_cast<const uint8_t *>(streamData[i]);
            for (uint32_t k = 0; k < indexCount; ++k)
            {
                const uint32_t at = !indexData ? k
                                    : index32  ? static_cast<const uint32_t *>(indexData)[k]
                                               : static_cast<const uint16_t *>(indexData)[k];
                std::memcpy(copy + size_t(k) * stride, from + size_t(at) * stride, stride);
            }
            streamData[i] = copy;
        }
        if (copied)
        {
            drawIndices = d.sequential;
            drawIndex32 = false;
            if (const VertexProgramEntry *sequential =
                    GetVertexProgram(device, *vertex, *device.decl, plan, false, strides))
                vertexGxm = sequential->program;
        }
    }
    if (passThrough)
    {
        const VertexElement *position = nullptr;
        for (const VertexElement &e : device.decl->elements)
            if (e.usage == d3d::DECLUSAGE_POSITION && e.usageIndex == 0)
                position = &e;
        int slot = -1;
        for (uint8_t i = 0; position && i < vertexProgram->streamCount; ++i)
            if (vertexProgram->streams[i] == position->stream)
                slot = i;
        uint8_t format = 0, components = 0;
        if (position && slot >= 0 && AttributeFormat(position->type, format, components) &&
            format == SCE_GXM_ATTRIBUTE_FORMAT_F32)
        {
            const uint32_t stride = strides[position->stream];
            const uint32_t key = stride | uint32_t(position->offset) << 12 | uint32_t(components) << 24 |
                                 uint32_t(drawIndex32) << 30;
            SceGxmVertexProgram *&program = d.diagPassPrograms[key];
            if (!program)
            {
                SceGxmVertexAttribute attribute{};
                attribute.streamIndex = 0;
                attribute.offset = position->offset;
                attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                attribute.componentCount = components;
                attribute.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(d.diagPassPosition));
                SceGxmVertexStream stream{uint16_t(stride), drawIndex32 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                                                                        : SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
                const int made = sceGxmShaderPatcherCreateVertexProgram(gxm::ShaderPatcher(), d.diagPass.id,
                                                                        &attribute, 1, &stream, 1, &program);
                if (made < 0)
                {
                    Log(device, "diag: the pass-through vertex program could not be made (0x%08x)", unsigned(made));
                    program = nullptr;
                }
            }
            if (program)
            {
                vertexGxm = program;
                streamData[0] = streamData[slot];
                drawStreams = 1;
            }
        }
    }
    SetPrograms(device, vertexGxm, fragmentProgram->program);

    SceGxmContext *context = Gxm();
    if (vertex->uniforms && vertexGxm == vertexProgram->program)
    {
        void *uniforms = nullptr;
        const int reserved = sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        const uint16_t count = vs->parsed.reflection.floatConstants;
        int set = 0;
        if (vertex->constants && count && reserved >= 0)
            set = sceGxmSetUniformDataF(uniforms, vertex->constants, 0, count * 4u, device.vsConstants.data());
        if ((reserved < 0 || set < 0) && d.uniformFailures++ < 5)
            Log(device, "vertex constants not set: reserve 0x%08x, set 0x%08x (%u constants)", unsigned(reserved),
                unsigned(set), unsigned(count));
        if (d.diagnosedDraws && vertex->constants)
        {
            const float *u = static_cast<const float *>(uniforms);
            Log(device,
                "diag frame %llu draw: constants parameter category %u container %u resource %u components %u array "
                "%u; reserve 0x%08x set 0x%08x; buffer %g %g %g %g %g %g %g %g",
                (unsigned long long)d.frame, unsigned(sceGxmProgramParameterGetCategory(vertex->constants)),
                sceGxmProgramParameterGetContainerIndex(vertex->constants),
                sceGxmProgramParameterGetResourceIndex(vertex->constants),
                sceGxmProgramParameterGetComponentCount(vertex->constants),
                sceGxmProgramParameterGetArraySize(vertex->constants), unsigned(reserved), unsigned(set),
                u ? u[0] : 0.0f, u ? u[1] : 0.0f, u ? u[2] : 0.0f, u ? u[3] : 0.0f, u ? u[4] : 0.0f,
                u ? u[5] : 0.0f, u ? u[6] : 0.0f, u ? u[7] : 0.0f);
        }
    }
    void *pixelUniforms = nullptr;
    if (pixel->uniforms)
    {
        sceGxmReserveFragmentDefaultUniformBuffer(context, &pixelUniforms);
        const uint16_t count = ps->parsed.reflection.floatConstants;
        if (pixel->constants && count)
            sceGxmSetUniformDataF(pixelUniforms, pixel->constants, 0, count * 4u, device.psConstants.data());
        if (pixel->alphaRef)
        {
            const float ref = float(device.renderStates[d3d::RS_ALPHAREF] & 0xff) / 255.0f;
            sceGxmSetUniformDataF(pixelUniforms, pixel->alphaRef, 0, 1, &ref);
        }
    }
    BindTextures(device, *pixel, pixelUniforms);
    for (uint8_t i = 0; i < drawStreams; ++i)
        sceGxmSetVertexStream(context, i, streamData[i]);
    if (d.diagnosedDraws)
    {
        --d.diagnosedDraws;
        DiagnoseDraw(device, type, indexCount, indexed ? indexData : nullptr, index32, baseVertex, upVertices,
                     upStride, state, *vertex, *pixel, blended ? &blend : nullptr);
    }
    const int result = sceGxmDraw(context, primitive, drawIndex32 ? SCE_GXM_INDEX_FORMAT_U32 : SCE_GXM_INDEX_FORMAT_U16,
                                  drawIndices, indexCount);
    if (result < 0)
    {
        if (d.drawFailures++ < 5)
            Log(device, "sceGxmDraw failed (0x%08x), %u indices", unsigned(result), indexCount);
        return;
    }
    ++d.drawn;
}

void Copy(Device &device, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear)
{
    DrawState &d = *device.draw;
    if (!src || !dst || src == dst)
        return;
    // The source as a texture: the back buffer, or one level of a texture.
    SceGxmTexture view;
    uint32_t width = 0, height = 0;
    int result = -1;
    if (src->kind == SurfaceKind::BackBuffer)
    {
        width = gxm::kDisplayWidth;
        height = gxm::kDisplayHeight;
        result = sceGxmTextureInitLinearStrided(&view, gxm::BackBufferData(), SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                                width, height, gxm::kDisplayStride * 4);
    }
    else if (src->kind == SurfaceKind::TextureLevel && src->texture->memory.gpu && !src->texture->format->depth)
    {
        const Texture &texture = *src->texture;
        const Level &level = texture.levels[src->face * texture.desc.levels + src->level];
        SceGxmTextureFormat format;
        width = level.width;
        height = level.height;
        if (TextureFormat(texture.desc.format, format))
        {
            if (!texture.format->compressed)
                result = sceGxmTextureInitLinearStrided(&view, texture.memory.data + level.offset, format, width,
                                                        height, level.pitch);
            else if (texture.sampleable && src->level == 0 && texture.desc.kind == TextureKind::Tex2D)
            {
                view = texture.sampled;
                result = 0;
            }
        }
    }
    if (result < 0)
    {
        NotYet(device, "StretchRect from this kind of surface (skipped)");
        return;
    }
    Rect from = srcRect ? *srcRect : Rect{0, 0, int32_t(width), int32_t(height)};

    // Draw into the destination, then let the next draw go back to the
    // device's own targets.
    Surface *targets = device.renderTargets[0];
    Surface *depth = device.depthStencil;
    device.renderTargets[0] = dst;
    device.depthStencil = nullptr;
    const bool open = OpenScene(device);
    device.renderTargets[0] = targets;
    device.depthStencil = depth;
    if (!open || !d.hasColor)
        return;
    const FragmentProgramEntry *fragment = GetFragmentProgram(device, d.copyFragment, d.copyVertex, nullptr);
    if (!fragment)
        return;
    const float w = float(d.sceneWidth), h = float(d.sceneHeight);
    Rect to = dstRect ? *dstRect : Rect{0, 0, int32_t(d.sceneWidth), int32_t(d.sceneHeight)};
    to.left = std::max(to.left, 0);
    to.top = std::max(to.top, 0);
    to.right = std::min(to.right, int32_t(d.sceneWidth));
    to.bottom = std::min(to.bottom, int32_t(d.sceneHeight));
    if (to.right <= to.left || to.bottom <= to.top)
        return;

    FixedState state{};
    state.viewport[0] = w * 0.5f;
    state.viewport[1] = w * 0.5f;
    state.viewport[2] = h * 0.5f;
    state.viewport[3] = -h * 0.5f;
    state.viewport[4] = 0.0f;
    state.viewport[5] = 1.0f;
    state.clip[0] = uint32_t(to.left);
    state.clip[1] = uint32_t(to.top);
    state.clip[2] = uint32_t(to.right - 1);
    state.clip[3] = uint32_t(to.bottom - 1);
    state.cull = SCE_GXM_CULL_NONE;
    state.polygon = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    state.depthFunc = SCE_GXM_DEPTH_FUNC_ALWAYS;
    state.depthWrite = SCE_GXM_DEPTH_WRITE_DISABLED;
    for (int side = 0; side < 2; ++side)
    {
        state.stencilFunc[side] = SCE_GXM_STENCIL_FUNC_ALWAYS;
        state.stencilFail[side] = state.stencilDepthFail[side] = state.stencilPass[side] = SCE_GXM_STENCIL_OP_KEEP;
    }
    ApplyFixedState(device, state);
    SetPrograms(device, d.copyVertexProgram, fragment->program);

    float *quad = static_cast<float *>(Transient(device, 16 * sizeof(float)));
    if (!quad)
        return;
    const float x0 = 2.0f * to.left / w - 1.0f, x1 = 2.0f * to.right / w - 1.0f;
    const float y0 = 1.0f - 2.0f * to.top / h, y1 = 1.0f - 2.0f * to.bottom / h;
    const float u0 = float(from.left) / width, u1 = float(from.right) / width;
    const float v0 = float(from.top) / height, v1 = float(from.bottom) / height;
    const float corners[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
    std::memcpy(quad, corners, sizeof(corners));
    const SceGxmTextureFilter filter = linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
    sceGxmTextureSetMagFilter(&view, filter);
    sceGxmTextureSetMinFilter(&view, filter);
    sceGxmTextureSetMipFilter(&view, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(&view, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&view, SCE_GXM_TEXTURE_ADDR_CLAMP);
    SceGxmContext *context = Gxm();
    sceGxmSetFragmentTexture(context, unsigned(d.copyUnit), &view);
    sceGxmSetVertexStream(context, 0, quad);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, d.sequential, 4);
    ++d.copies;
}

void DiagnoseFrame(Device &device)
{
    DrawState *d = device.draw;
    if (!d)
        return;
    // The frame being drawn is d->frame (FrameDone counts it after Present).
    if (d->frame >= kTestRectFirst && d->frame <= kTestRectLast)
    {
        // Draw into the back buffer whatever the game left bound.
        Surface *saved = device.renderTargets[0];
        const Viewport savedViewport = device.viewport;
        const uint32_t savedScissor = device.renderStates[d3d::RS_SCISSORTESTENABLE];
        device.renderTargets[0] = &device.backBuffer;
        device.viewport = Viewport{0, 0, gxm::kDisplayWidth, gxm::kDisplayHeight, 0.0f, 1.0f};
        device.renderStates[d3d::RS_SCISSORTESTENABLE] = 0;
        const Rect quarter{0, 0, int32_t(gxm::kDisplayWidth / 2), int32_t(gxm::kDisplayHeight / 2)};
        ClearTargets(device, 1, &quarter, d3d::CLEAR_TARGET, 0xffff00ffu, 0.0f, 0);
        device.renderTargets[0] = saved;
        device.viewport = savedViewport;
        device.renderStates[d3d::RS_SCISSORTESTENABLE] = savedScissor;
        if (d->frame == kTestRectFirst)
            Log(device, "diag: drawing a magenta rectangle over the top-left quarter until frame %llu",
                (unsigned long long)kTestRectLast);
    }
    bool readBack = false;
    for (const uint64_t frame : kReadBackFrames)
        readBack |= d->frame == frame;
    if (!readBack)
        return;
    FinishGpu(device);
    const uint32_t *pixels = static_cast<const uint32_t *>(gxm::BackBufferData());
    if (!pixels)
        return;
    uint32_t lit = 0, magenta = 0, green = 0, litTopLeft = 0;
    uint32_t sum[3] = {};
    for (uint32_t y = 0; y < gxm::kDisplayHeight; ++y)
        for (uint32_t x = 0; x < gxm::kDisplayWidth; ++x)
        {
            const uint32_t p = pixels[y * gxm::kDisplayStride + x];
            if (p & 0x00ffffffu)
            {
                ++lit;
                if (x < gxm::kDisplayWidth / 2 && y < gxm::kDisplayHeight / 2)
                    ++litTopLeft;
            }
            if ((p & 0x00ffffffu) == 0x00ff00ffu)
                ++magenta;
            if ((p & 0x00ffffffu) == 0x0000ff00u)
                ++green;
            sum[0] += p & 0xff;
            sum[1] += (p >> 8) & 0xff;
            sum[2] += (p >> 16) & 0xff;
        }
    const uint32_t total = gxm::kDisplayWidth * gxm::kDisplayHeight;
    auto at = [&](uint32_t x, uint32_t y) { return pixels[y * gxm::kDisplayStride + x]; };
    Log(device,
        "diag frame %llu (test %d) back buffer: %u of %u pixels not black (%u in the top-left quarter), %u "
        "magenta, %u green; mean r %u g %u b %u; samples (A8B8G8R8) centre %08x, quarter points %08x %08x %08x %08x, "
        "corner %08x",
        (unsigned long long)d->frame, d->diagMode, lit, total, litTopLeft, magenta, green, sum[0] / total, sum[1] / total, sum[2] / total,
        at(480, 272), at(240, 136), at(720, 136), at(240, 408), at(720, 408), at(2, 2));
}

void LogDrawSummary(Device &device)
{
    const DrawState *d = device.draw;
    if (!d)
        return;
    Log(device,
        "draws: %llu drawn and %llu copies in %llu scenes; %u vertex and %u fragment programs, %u shader variants made "
        "(%u did not compile); %llu buffer renames, %llu blocks past the transient ring, %u failed draws",
        (unsigned long long)d->drawn, (unsigned long long)d->copies, (unsigned long long)d->scenes, unsigned(d->vertexPrograms.size()),
        unsigned(d->fragmentPrograms.size()), d->variantsMade, d->variantFailures, (unsigned long long)d->renames,
        (unsigned long long)d->ringOverflows, d->drawFailures);
    for (int i = 0; i < SkipCount; ++i)
        if (d->skipped[i])
            Log(device, "draws skipped: %llu x %s", (unsigned long long)d->skipped[i], kSkipNames[i]);
}
} // namespace gfx_vk::legacy
