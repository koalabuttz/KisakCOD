// The Modern lit shaders of our own (vk_legacy.h ModernLitShader): GLSL
// shaders compiled at build time (shaders/lm_sun.frag, lp_sun.frag) that a lit pass's
// pixel shader is drawn as in the linear-light scene. Each reads what the
// game's shader it stands in for reads, from the same samplers and constant
// registers, so the pipeline layout and descriptors stay that shader's. Only
// the vertex shader's outputs need linking: the GLSL declares its inputs at
// placeholder locations (texcoord N at N, colour 0 at 10), and here they are
// moved, in the SPIR-V, to the locations where the vertex shader writes those
// semantics, and the specialization constants' defaults are set (the
// strengths, the depth reference's clamp, the mode and the alpha test).

#include "vk_legacy_internal.h"
#include "vk_legacy_shaders.h"

#include <bit>
#include <cstring>

namespace gfx_vk::legacy
{
namespace
{
constexpr uint32_t kSpirvMagic = 0x07230203;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpSpecConstant = 50;
constexpr uint32_t kDecorationSpecId = 1;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kStorageInput = 1;

// A placeholder location, the semantic it stands for, and the components the
// shader reads.
struct Placeholder
{
    uint32_t location;
    uint8_t usage; // D3DDECLUSAGE: 5 texcoord, 10 colour
    uint32_t index;
    uint8_t components;
};
constexpr uint8_t kUsageTexcoord = 5;
constexpr uint8_t kUsageColor = 10;

// Where vs writes usage/index with at least `components` components from x,
// alone at its location; -1 when it does not.
int OutputLocation(const ls::Shader &vs, uint8_t usage, uint32_t index, uint8_t components)
{
    const ls::IoEntry *found = nullptr;
    for (const ls::IoEntry &entry : vs.reflection.outputs)
    {
        if (!entry.builtIn && entry.semanticUsage == usage && entry.semanticIndex == index)
            found = &entry;
    }
    if (!found)
        return -1;
    // It must be one vector of exactly the components the shader declares:
    // without maintenance4 a vertex output may not have more components than
    // the input that reads it.
    if (found->mask != (1u << components) - 1u)
        return -1;
    for (const ls::IoEntry &entry : vs.reflection.outputs)
    {
        if (&entry != found && !entry.builtIn && entry.location == found->location)
            return -1;
    }
    return found->location;
}

// The lm_sun.frag and lp_sun.frag builds for each set of maps
// (gen_spirv_header.py), by family and ModernLitShader flags (shadow 1,
// normal 2, specular 4, detail 8); without the sun (ModernLitNoSun) the same
// sources without it, by the maps alone.
struct LitBuild
{
    const uint32_t *words;
    size_t count;
};
#define LIT_BUILD(name) LitBuild{shaders::name, sizeof(shaders::name) / sizeof(uint32_t)}
#define LIT_FAMILY(prefix)                                                                                         \
    switch (maps)                                                                                                  \
    {                                                                                                              \
    case 0: return LIT_BUILD(k##prefix##Frag);                                                                     \
    case ModernLitNormal: return LIT_BUILD(k##prefix##NFrag);                                                      \
    case ModernLitSpecular: return LIT_BUILD(k##prefix##SFrag);                                                    \
    case ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##NSFrag);                                 \
    case ModernLitDetail: return LIT_BUILD(k##prefix##DFrag);                                                      \
    case ModernLitDetail | ModernLitNormal: return LIT_BUILD(k##prefix##DNFrag);                                   \
    case ModernLitDetail | ModernLitSpecular: return LIT_BUILD(k##prefix##DSFrag);                                 \
    case ModernLitDetail | ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##DNSFrag);              \
    case ModernLitShadow: return LIT_BUILD(k##prefix##ShadowFrag);                                                 \
    case ModernLitShadow | ModernLitNormal: return LIT_BUILD(k##prefix##ShadowNFrag);                              \
    case ModernLitShadow | ModernLitSpecular: return LIT_BUILD(k##prefix##ShadowSFrag);                            \
    case ModernLitShadow | ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##ShadowNSFrag);         \
    case ModernLitShadow | ModernLitDetail: return LIT_BUILD(k##prefix##ShadowDFrag);                              \
    case ModernLitShadow | ModernLitDetail | ModernLitNormal: return LIT_BUILD(k##prefix##ShadowDNFrag);           \
    case ModernLitShadow | ModernLitDetail | ModernLitSpecular: return LIT_BUILD(k##prefix##ShadowDSFrag);         \
    case ModernLitShadow | ModernLitDetail | ModernLitNormal | ModernLitSpecular:                                   \
        return LIT_BUILD(k##prefix##ShadowDNSFrag);                                                                \
    default: return LitBuild{nullptr, 0};                                                                          \
    }
// A spot or omni light's: no shadow map with a detail map.
#define LIT_LIGHT(prefix)                                                                                          \
    switch (maps)                                                                                                  \
    {                                                                                                              \
    case 0: return LIT_BUILD(k##prefix##Frag);                                                                     \
    case ModernLitNormal: return LIT_BUILD(k##prefix##NFrag);                                                      \
    case ModernLitSpecular: return LIT_BUILD(k##prefix##SFrag);                                                    \
    case ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##NSFrag);                                 \
    case ModernLitDetail: return LIT_BUILD(k##prefix##DFrag);                                                      \
    case ModernLitDetail | ModernLitNormal: return LIT_BUILD(k##prefix##DNFrag);                                   \
    case ModernLitDetail | ModernLitSpecular: return LIT_BUILD(k##prefix##DSFrag);                                 \
    case ModernLitDetail | ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##DNSFrag);              \
    case ModernLitShadow: return LIT_BUILD(k##prefix##ShadowFrag);                                                 \
    case ModernLitShadow | ModernLitNormal: return LIT_BUILD(k##prefix##ShadowNFrag);                              \
    case ModernLitShadow | ModernLitSpecular: return LIT_BUILD(k##prefix##ShadowSFrag);                            \
    case ModernLitShadow | ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##ShadowNSFrag);         \
    default: return LitBuild{nullptr, 0};                                                                          \
    }
#define LIT_NO_SUN(prefix)                                                                                         \
    switch (maps)                                                                                                  \
    {                                                                                                              \
    case 0: return LIT_BUILD(k##prefix##Frag);                                                                     \
    case ModernLitNormal: return LIT_BUILD(k##prefix##NFrag);                                                      \
    case ModernLitSpecular: return LIT_BUILD(k##prefix##SFrag);                                                    \
    case ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##NSFrag);                                 \
    case ModernLitDetail: return LIT_BUILD(k##prefix##DFrag);                                                      \
    case ModernLitDetail | ModernLitNormal: return LIT_BUILD(k##prefix##DNFrag);                                   \
    case ModernLitDetail | ModernLitSpecular: return LIT_BUILD(k##prefix##DSFrag);                                 \
    case ModernLitDetail | ModernLitNormal | ModernLitSpecular: return LIT_BUILD(k##prefix##DNSFrag);              \
    default: return LitBuild{nullptr, 0};                                                                          \
    }
LitBuild LitBuildFor(uint16_t shader)
{
    if (ModernLitAmbientOnly(shader))
        return LIT_BUILD(kLpAmbFrag);
    if (ModernLitPointLight(shader))
    {
        const uint8_t maps = shader & (ModernLitShadow | ModernLitNormal | ModernLitSpecular | ModernLitDetail);
        const bool spot = (shader & ModernLitLightMask) == ModernLitSpot;
        if (ModernLitModel(shader))
        {
            if (spot)
            {
                LIT_LIGHT(LpSpot)
            }
            LIT_LIGHT(LpOmni)
        }
        if (spot)
        {
            LIT_LIGHT(LmSpot)
        }
        LIT_LIGHT(LmOmni)
    }
    if (!ModernLitSun(shader))
    {
        const uint8_t maps = shader & (ModernLitNormal | ModernLitSpecular | ModernLitDetail);
        if (ModernLitModel(shader))
        {
            LIT_NO_SUN(LpNoSun)
        }
        LIT_NO_SUN(LmNoSun)
    }
    const uint8_t maps = shader & (ModernLitShadow | ModernLitNormal | ModernLitSpecular | ModernLitDetail);
    if (ModernLitModel(shader))
    {
        LIT_FAMILY(LpSun)
    }
    LIT_FAMILY(LmSun)
}
#undef LIT_FAMILY
#undef LIT_LIGHT
#undef LIT_NO_SUN
#undef LIT_BUILD
} // namespace

bool BuildModernLitSpirv(uint16_t shader, const ls::Shader &vs, float sunStrength, float skyStrength,
                         bool clampDepthReference, bool alphaTest, std::vector<uint32_t> &spirv)
{
    if (!ModernLitShaderValid(shader))
        return false;
    const LitBuild build = LitBuildFor(shader);
    if (!build.words)
        return false;
    const uint32_t *words = build.words;
    const size_t count = build.count;
    std::vector<Placeholder> placeholders = {
        {0, kUsageTexcoord, 0, 4},
        {1, kUsageTexcoord, 1, 4},
        {10, kUsageColor, 0, 4},
    };
    // The world's normal-mapped surfaces without the sun need the normal (and
    // so the binormal and tangent) only for the specular: their normal map
    // turns only the lightmap's light (lit shader facts 5), and the game's
    // vertex shader for them writes no tangent frame.
    const bool tangentFrame = (shader & ModernLitNormal)
        && (ModernLitSun(shader) || ModernLitPointLight(shader) || ModernLitModel(shader)
            || (shader & ModernLitSpecular));
    if (tangentFrame)
    {
        placeholders.push_back({2, kUsageTexcoord, 2, 4});
        placeholders.push_back({3, kUsageTexcoord, 3, 4});
    }
    if (ModernLitShadowed(shader))
        placeholders.push_back({4, kUsageTexcoord, 4, 4});
    // The eye-to-point vector: for the specular, and a spot or omni light's
    // direction and distance.
    if ((shader & ModernLitSpecular) || ModernLitPointLight(shader))
        placeholders.push_back({5, kUsageTexcoord, 5, 4});
    if (ModernLitModel(shader) && !ModernLitAmbientOnly(shader))
        placeholders.push_back({6, kUsageTexcoord, 6, 4}); // the lighting-volume coordinate (lp_amb: texcoord 1)
    const uint32_t mode = (shader & ModernLitModeMask) >> ModernLitModeShift;
    // Where each placeholder goes.
    std::vector<int> moved(placeholders.size());
    for (size_t i = 0; i < placeholders.size(); ++i)
    {
        const Placeholder &p = placeholders[i];
        moved[i] = OutputLocation(vs, p.usage, p.index, p.components);
        if (moved[i] < 0)
            return false;
    }
    std::vector<uint32_t> out(words, words + count);
    if (out.size() < 5 || out[0] != kSpirvMagic)
        return false;
    // The input variables, then their Location decorations and the
    // specialization constants' ids.
    std::vector<uint32_t> inputs;
    std::vector<std::pair<uint32_t, uint32_t>> specIds; // result id, SpecId
    for (size_t at = 5; at < out.size();)
    {
        const uint32_t length = out[at] >> 16, op = out[at] & 0xffffu;
        if (!length || at + length > out.size())
            return false;
        if (op == kOpVariable && length >= 4 && out[at + 3] == kStorageInput)
            inputs.push_back(out[at + 2]);
        if (op == kOpDecorate && length == 4 && out[at + 2] == kDecorationSpecId)
            specIds.emplace_back(out[at + 1], out[at + 3]);
        at += length;
    }
    uint32_t placed = 0;
    for (size_t at = 5; at < out.size();)
    {
        const uint32_t length = out[at] >> 16, op = out[at] & 0xffffu;
        if (op == kOpDecorate && length == 4 && out[at + 2] == kDecorationLocation)
        {
            bool input = false;
            for (const uint32_t id : inputs)
                input = input || id == out[at + 1];
            if (input)
            {
                bool known = false;
                for (size_t i = 0; i < placeholders.size(); ++i)
                {
                    if (out[at + 3] == placeholders[i].location && !known)
                    {
                        out[at + 3] = static_cast<uint32_t>(moved[i]);
                        known = true;
                        ++placed;
                    }
                }
                if (!known)
                    return false; // an input this file does not know
            }
        }
        else if (op == kOpSpecConstant && length == 4)
        {
            for (const auto &[id, specId] : specIds)
            {
                if (id != out[at + 2])
                    continue;
                if (specId == 0)
                    out[at + 3] = std::bit_cast<uint32_t>(sunStrength);
                else if (specId == 1)
                    out[at + 3] = std::bit_cast<uint32_t>(skyStrength);
                else if (specId == 2)
                    out[at + 3] = clampDepthReference ? 1u : 0u;
                else if (specId == 3)
                    out[at + 3] = mode;
                else if (specId == 4)
                    out[at + 3] = alphaTest ? 1u : 0u;
            }
        }
        at += length;
    }
    if (placed != placeholders.size())
        return false;
    spirv = std::move(out);
    return true;
}
} // namespace gfx_vk::legacy
