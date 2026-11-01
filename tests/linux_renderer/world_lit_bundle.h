// tests/linux_renderer/world_lit_bundle.h
//
// Bounded reader for ONE static LIT world surface bundle (document
// "kisak_static_lit_world_surface_bundle", schema 1).
//
// The JSON reader, the field accessors, the FNV-1a-64 helpers, the payload reader and the SM3
// framing walk all live in world_surface_bundle.h and are reused from there. This header adds
// only what is specific to the lit profile:
//
//   * the four-element world declaration (position, colour, texcoord, normal UBYTE4 at 36);
//   * the lit argument set: codes 60, 76, 41 and 42, with NO code-40 material colour, because
//     the lit pass takes its colour from the bound colour map times the lightmap;
//   * one custom sampler only, the secondary lightmap at stage 3 with raw state 0x62, and a
//     captured lightmap whose layout, length and hash must match its payload.
//
// Every rule is a refusal, never a repair. The unlit loader and oracle are untouched.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "world_surface_bundle.h"

namespace world_lit_bundle {

using world_surface_bundle::JsonValue;

// --- profile pins ----------------------------------------------------------
constexpr std::uint32_t kSchemaVersion = 1;
constexpr const char* kDocumentName = "kisak_static_lit_world_surface_bundle";
constexpr std::uint32_t kTechniqueTypeLit = 7;
constexpr const char* kTechniqueName = "lm_r0c0_sm3";
constexpr const char* kDeclarationName = "plain_world";

// The pass carries the secondary-lightmap bit and nothing else: this gate captures that one
// resource and refuses a pass that also asks for the reflection probe.
constexpr std::uint32_t kCustomSamplerFlags = 4;
constexpr std::uint32_t kLightmapStage = 3;
constexpr std::uint32_t kLightmapSamplerState = 98; // 0x62
constexpr const char* kLightmapResource = "lightmap_secondary";

// src/gfx_d3d/r_state.h constant sources and the registers they own.
constexpr std::uint32_t kCodeMaterialColor = 40; // unlit profile only
constexpr std::uint32_t kCodeFog = 41;
constexpr std::uint32_t kCodeFogColor = 42;
constexpr std::uint32_t kCodeTransposeWorld = 60;
constexpr std::uint32_t kCodeTransposeViewProjection = 76;

constexpr std::uint32_t kVsDestTransposeViewProjection = 0;
constexpr std::uint32_t kVsDestTransposeWorld = 4;
constexpr std::uint32_t kVsDestFog = 21;
constexpr std::uint32_t kPsDestFogColor = 0;
constexpr std::uint32_t kPsDestMaterialColor = 1; // refused here, see CheckArguments

// src/gfx_d3d/r_material.h MTL_ARG_* stage values.
constexpr std::uint32_t kArgVertexConst = 3;
constexpr std::uint32_t kArgPixelConst = 5;
constexpr std::uint32_t kArgPixelSampler = 2;

// Captured lightmap: one level of D3DFMT_A8R8G8B8, 512x2048x1, 4194304 bytes.
constexpr std::uint32_t kLightmapFormat = 21;
constexpr std::uint32_t kLightmapLevels = 1;
constexpr std::uint32_t kLightmapFlags = 2;
constexpr std::uint32_t kLightmapWidth = 512;
constexpr std::uint32_t kLightmapHeight = 2048;

// ---------------------------------------------------------------------------
// The audited program pair, named by value rather than by the bundle's word.
//
// lm_tc0_sm3 (vertex): 179 DWORDs / 716 bytes, FNV-1a-64 e1989ed410dbcfd6.
// lm_r0c0_sm3 (pixel): 148 DWORDs / 592 bytes, FNV-1a-64 3551ae4bda0872e4.
//
// The loader recomputes the FNV-1a-64 of the payload bytes it read from disk and compares that
// value with these constants. Comparing it only with the bundle's own fnv1a64 field would make
// the check answer "the file matches what the file says", which a substituted pair satisfies.
// The word count is checked the same way, because two programs of the same length are not the
// same program and a hash alone would not stop a truncated rebuild that happens to re-hash.
// ---------------------------------------------------------------------------
constexpr std::uint64_t kExpectedVertexShaderFnv = 0xe1989ed410dbcfd6ull;
constexpr std::uint32_t kExpectedVertexShaderWords = 179u;
constexpr std::uint64_t kExpectedPixelShaderFnv = 0x3551ae4bda0872e4ull;
constexpr std::uint32_t kExpectedPixelShaderWords = 148u;
constexpr std::uint32_t kExpectedVertexShaderBytes = kExpectedVertexShaderWords * 4u;
constexpr std::uint32_t kExpectedPixelShaderBytes = kExpectedPixelShaderWords * 4u;
constexpr const char* kExpectedVertexShaderName = "lm_tc0_sm3.hlsl";

// The material colour map: stage 0, the stable group's sampler argument, raw state 19 (0x13).
// s0 is the only sampler this profile binds from a material argument; the lightmap is the
// custom sampler at stage 3 with raw state 0x62. The two raw words are different on purpose
// and are never merged into one decoded state.
constexpr std::uint32_t kColorSamplerDest = 0;
constexpr std::uint32_t kColorSamplerState = 19; // 0x13
constexpr const char* kStableGroup = "stable";

// Structural caps. These are the exporter's caps block and the unlit loader's limits; the
// loader reads the bundle's record of them and refuses a bundle that disagrees, so a widened
// count cannot arrive through a JSON field this file never looks at.
constexpr std::uint32_t kMaxBundleVertices = 65535u;
constexpr std::uint32_t kMaxBundleIndices = 1048576u;
constexpr std::uint64_t kMaxBundleBytes = 67108864ull;
constexpr std::uint32_t kMaxShaderBytes = 262144u;
constexpr std::uint32_t kIndexStride = 2u;

// A 64-bit FNV-1a-64 as the 16 lower-case hex digits the bundle writes, so a mismatch can name
// the value that was measured instead of only the value that was hoped for.
inline std::string FnvHex(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return std::string(text);
}

// ---------------------------------------------------------------------------
// Sampling geometry: the arithmetic that decides which pixel was evaluated and which texel that
// pixel samples. It lives in this header so the analytic tests exercise the same code the replay
// runs. Nothing here touches a device, a bundle or a file.
//
// The screen mapping is the ordinary D3D9 viewport transform with no half-pixel term:
//
//     x = (ndc.x + 1) * 0.5 * width      y = (1 - ndc.y) * 0.5 * height
//
// An integer sample coordinate is a pixel index. Shifting the projection or the sample point by
// half a pixel does not move the GPU; it only moves the CPU's evaluation point away from the pixel
// being compared, and that error has to show up in the texel-boundary margin rather than be hidden
// in the transform.
// ---------------------------------------------------------------------------
namespace sampling {

inline bool ProjectToScreen(const float clip[4], std::uint32_t width, std::uint32_t height,
    float* screenX, float* screenY)
{
    if (!(clip[3] > 0.0f))
        return false;
    const float reciprocalW = 1.0f / clip[3];
    *screenX = (clip[0] * reciprocalW * 0.5f + 0.5f) * static_cast<float>(width);
    *screenY = (0.5f - clip[1] * reciprocalW * 0.5f) * static_cast<float>(height);
    return true;
}

inline bool ScreenBarycentric(const float screen[3][2], float px, float py, float out[3])
{
    const float area = (screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1])
        - (screen[2][0] - screen[0][0]) * (screen[1][1] - screen[0][1]);
    if (std::fabs(area) < 1.0e-9f)
        return false;

    const float weight1 = ((px - screen[0][0]) * (screen[2][1] - screen[0][1])
                              - (screen[2][0] - screen[0][0]) * (py - screen[0][1]))
        / area;
    const float weight2 = ((screen[1][0] - screen[0][0]) * (py - screen[0][1])
                              - (px - screen[0][0]) * (screen[1][1] - screen[0][1]))
        / area;
    out[0] = 1.0f - weight1 - weight2;
    out[1] = weight1;
    out[2] = weight2;
    return true;
}

// Perspective-correct value of one varying at (px, py): sum(b_i r_i a_i) / sum(b_i r_i).
inline bool PerspectiveInterpolate(const float screen[3][2], const float reciprocalW[3],
    const float attribute[3], float px, float py, float* out)
{
    float barycentric[3];
    if (!ScreenBarycentric(screen, px, py, barycentric))
        return false;

    float numerator = 0.0f;
    float denominator = 0.0f;
    for (int corner = 0; corner < 3; ++corner)
    {
        const float scaled = barycentric[corner] * reciprocalW[corner];
        numerator += scaled * attribute[corner];
        denominator += scaled;
    }
    if (denominator == 0.0f)
        return false;
    *out = numerator / denominator;
    return true;
}

// Inside the triangle and at least `marginPixels` from every edge. The edge function is twice a
// signed area in pixels squared; dividing by the edge length in pixels makes it a distance.
inline bool InsideTriangle(const float screen[3][2], float px, float py, float marginPixels)
{
    const float area = (screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1])
        - (screen[2][0] - screen[0][0]) * (screen[1][1] - screen[0][1]);
    if (std::fabs(area) < 1.0e-9f)
        return false;
    const float sign = area > 0.0f ? 1.0f : -1.0f;

    for (int edge = 0; edge < 3; ++edge)
    {
        const int next = (edge + 1) % 3;
        const float dx = screen[next][0] - screen[edge][0];
        const float dy = screen[next][1] - screen[edge][1];
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0f))
            return false;
        const float edgeFunction = dx * (py - screen[edge][1]) - dy * (px - screen[edge][0]);
        if (edgeFunction * sign < marginPixels * length)
            return false;
    }
    return true;
}

// Distance from a texture coordinate to the nearest texel boundary, in texels.
inline float TexelBoundaryMargin(float coordinate, std::uint32_t size)
{
    if (size == 0)
        return 0.0f;
    const float scaled = coordinate * static_cast<float>(size);
    const float fraction = scaled - std::floor(scaled);
    return std::min(fraction, 1.0f - fraction);
}

// The largest singular value of the map from a screen-space step to a texture-space step, in
// texels per pixel: the worst-direction texel travel of one pixel at this point, finite-differenced
// from the same interpolation the prediction uses.
inline bool MaxTexelTravelPerPixel(const float screen[3][2], const float reciprocalW[3],
    const float texelU[3], const float texelV[3], float px, float py, float* travel)
{
    const float step = 1.0e-3f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float ux = 0.0f;
    float vx = 0.0f;
    float uy = 0.0f;
    float vy = 0.0f;
    if (!PerspectiveInterpolate(screen, reciprocalW, texelU, px, py, &u0)
        || !PerspectiveInterpolate(screen, reciprocalW, texelV, px, py, &v0)
        || !PerspectiveInterpolate(screen, reciprocalW, texelU, px + step, py, &ux)
        || !PerspectiveInterpolate(screen, reciprocalW, texelV, px + step, py, &vx)
        || !PerspectiveInterpolate(screen, reciprocalW, texelU, px, py + step, &uy)
        || !PerspectiveInterpolate(screen, reciprocalW, texelV, px, py + step, &vy))
        return false;

    const float du_dx = (ux - u0) / step;
    const float dv_dx = (vx - v0) / step;
    const float du_dy = (uy - u0) / step;
    const float dv_dy = (vy - v0) / step;

    const float sum = du_dx * du_dx + dv_dx * dv_dx + du_dy * du_dy + dv_dy * dv_dy;
    const float determinant = du_dx * dv_dy - du_dy * dv_dx;
    const float discriminant = sum * sum - 4.0f * determinant * determinant;
    *travel = std::sqrt(std::max(0.0f, (sum + std::sqrt(std::max(0.0f, discriminant))) * 0.5f));
    return true;
}

} // namespace sampling

// ---------------------------------------------------------------------------
// Typed contents.
// ---------------------------------------------------------------------------
struct Element
{
    std::uint32_t offset = 0;
    std::uint32_t type = 0; // D3DDECLTYPE value as recorded
    std::uint32_t usageIndex = 0;
    std::string usageName;
};

struct CodeArgument
{
    std::string group;
    std::uint32_t type = 0;
    std::uint32_t code = 0;
    std::uint32_t dest = 0;
    std::uint32_t rows = 0;
};

struct TextureArgument
{
    std::string group; // the arguments group the entry was read from
    std::uint32_t type = 0;
    std::uint32_t dest = 0;
    std::uint32_t index = 0;
    std::uint32_t semantic = 0;
    std::uint32_t samplerState = 0;
    std::uint32_t nameHash = 0;
    std::string image;
};

// A recorded texture dependency. `bound` marks the one the pass's texture argument names; the
// rest stay recorded and unbound, because the live closure of this draw is the bound set.
struct TextureDependency
{
    std::uint32_t slot = 0;
    std::uint32_t semantic = 0;
    std::uint32_t nameHash = 0;
    std::uint32_t samplerState = 0;
    std::string image;
    std::string iwiEntry;
    bool bound = false;
};

struct Shader
{
    std::string name;
    std::string file;
    std::uint32_t words = 0;
    std::uint32_t bytes = 0;
    std::uint64_t fnv = 0;
    std::vector<std::uint8_t> payload;
};

struct Capture
{
    std::string resource;
    std::string file;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t bytes = 0;
    std::uint32_t format = 0;
    std::uint32_t levels = 0;
    std::uint32_t flags = 0;
    std::uint64_t fnv = 0;
    std::vector<std::uint8_t> payload;
};

struct Bundle
{
    bool renderReady = false;
    bool replayClaimed = false;

    std::string map;
    std::uint32_t surfaceIndex = 0;
    std::uint32_t surfaceFlags = 0;
    std::uint32_t lightmapIndex = 0;
    std::uint32_t reflectionProbeIndex = 0;

    std::uint32_t vertexCount = 0;
    std::uint32_t triCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t vertexStride = 0;

    std::string materialName;
    std::string techniqueName;
    std::uint32_t techniqueType = 0;
    std::uint32_t stateBits0 = 0;
    std::uint32_t stateBits1 = 0;

    std::vector<Element> elements;
    std::vector<CodeArgument> codeArguments;
    std::vector<TextureArgument> textureArguments;
    std::vector<TextureDependency> dependencies;

    Shader vertexShader;
    Shader pixelShader;

    std::vector<std::uint8_t> vertices;
    std::vector<std::uint8_t> indices;

    Capture capture;

    // The bundle's own summary. Reported and cross-checked, never used as a drawable verdict.
    bool allRequirementsMet = false;
};

// ---------------------------------------------------------------------------
// Profile checks.
// ---------------------------------------------------------------------------

// One entry per argument the lit pass may carry. A bundle argument matches when its group,
// stage, code, destination and row count match; matching is one-to-one in both directions, so
// neither a missing nor an extra argument can pass.
struct ExpectedArgument
{
    const char* group;
    std::uint32_t type;
    bool texture;
    std::uint32_t code;
    std::uint32_t dest;
    std::uint32_t rows;
    std::uint32_t samplerState; // texture arguments only
};

inline const ExpectedArgument kExpectedArguments[] = {
    {"per_prim", kArgVertexConst, false, kCodeTransposeWorld, kVsDestTransposeWorld, 4u, 0u},
    {"per_object", kArgVertexConst, false, kCodeTransposeViewProjection,
        kVsDestTransposeViewProjection, 4u, 0u},
    {"stable", kArgPixelSampler, true, 0u, kColorSamplerDest, 0u, kColorSamplerState},
    {"stable", kArgVertexConst, false, kCodeFog, kVsDestFog, 1u, 0u},
    {"stable", kArgPixelConst, false, kCodeFogColor, kPsDestFogColor, 1u, 0u},
};

inline bool CheckArguments(const Bundle& bundle, std::string* error)
{
    for (const CodeArgument& argument : bundle.codeArguments)
    {
        if (argument.code == kCodeMaterialColor)
        {
            *error = "the pass carries a code-40 material-colour argument. Code 40 belongs to the "
                     "unlit profile; the lit pass takes its colour from the bound colour map "
                     "times the lightmap, so accepting this would draw an unlit substitution "
                     "under a lit technique's name";
            return false;
        }
        if (argument.code != kCodeFog && argument.code != kCodeFogColor
            && argument.code != kCodeTransposeWorld && argument.code != kCodeTransposeViewProjection)
        {
            *error = "constant code " + std::to_string(argument.code)
                + " has no value in this test context (41, 42, 60 and 76 only)";
            return false;
        }
    }

    // Four codes, no more: 60, 76, 41 and 42 each exactly once. Stated as a count as well as a
    // one-to-one match so a bundle carrying a duplicate of an accepted code cannot hide behind the
    // matching below.
    if (bundle.codeArguments.size() != 4u)
    {
        *error = "the pass carries " + std::to_string(bundle.codeArguments.size())
            + " constant argument(s); the lit profile has exactly four (60, 76, 41, 42) and no "
              "code 40";
        return false;
    }

    std::vector<bool> usedCode(bundle.codeArguments.size(), false);
    std::vector<bool> usedTexture(bundle.textureArguments.size(), false);
    const std::size_t expectedCount = sizeof(kExpectedArguments) / sizeof(kExpectedArguments[0]);

    for (std::size_t index = 0; index < expectedCount; ++index)
    {
        const ExpectedArgument& expected = kExpectedArguments[index];
        std::size_t matches = 0;
        if (expected.texture)
        {
            for (std::size_t candidate = 0; candidate < bundle.textureArguments.size(); ++candidate)
            {
                const TextureArgument& argument = bundle.textureArguments[candidate];
                if (argument.group != expected.group || argument.type != expected.type
                    || argument.dest != expected.dest
                    || argument.samplerState != expected.samplerState)
                    continue;
                ++matches;
                usedTexture[candidate] = true;
            }
        }
        else
        {
            for (std::size_t candidate = 0; candidate < bundle.codeArguments.size(); ++candidate)
            {
                const CodeArgument& argument = bundle.codeArguments[candidate];
                if (argument.group != expected.group || argument.type != expected.type
                    || argument.code != expected.code || argument.dest != expected.dest
                    || argument.rows != expected.rows)
                    continue;
                ++matches;
                usedCode[candidate] = true;
            }
        }
        if (matches != 1)
        {
            *error = "the lit argument set needs exactly one "
                + std::string(expected.texture
                        ? "material colour-map sampler in the '" + std::string(expected.group)
                            + "' group at register " + std::to_string(expected.dest) + " with raw "
                            + "sampler state " + std::to_string(expected.samplerState)
                        : "argument for code " + std::to_string(expected.code) + " in the '"
                            + std::string(expected.group) + "' group at register "
                            + std::to_string(expected.dest) + " with "
                            + std::to_string(expected.rows) + " rows")
                + "; the bundle has " + std::to_string(matches);
            return false;
        }
    }

    for (std::size_t candidate = 0; candidate < usedCode.size(); ++candidate)
        if (!usedCode[candidate])
        {
            *error = "the pass carries code " + std::to_string(bundle.codeArguments[candidate].code)
                + " at register " + std::to_string(bundle.codeArguments[candidate].dest)
                + ", which the lit profile did not ask for";
            return false;
        }
    for (std::size_t candidate = 0; candidate < usedTexture.size(); ++candidate)
        if (!usedTexture[candidate])
        {
            *error = "the pass binds an extra material sampler, which the lit profile did not ask for";
            return false;
        }
    return true;
}

// The declaration resolves to the four world elements at their recorded offsets, one stream,
// and the two lists the exporter writes must agree.
inline bool CheckDeclaration(const Bundle& bundle, std::string* error)
{
    static const struct
    {
        std::uint32_t offset;
        std::uint32_t type;
        const char* usage;
    } pinned[4] = {{0u, 3u, "position"}, {16u, 4u, "color"}, {20u, 3u, "texcoord"}, {36u, 5u, "normal"}};

    if (bundle.vertexStride != 44u)
    {
        *error = "geometry.vertex_stride is " + std::to_string(bundle.vertexStride)
            + ", expected 44 (the lit declaration adds the normal at offset 36)";
        return false;
    }
    if (bundle.elements.size() != 4u)
    {
        *error = "declaration.elements has " + std::to_string(bundle.elements.size())
            + " entries, expected 4";
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index)
    {
        const Element& element = bundle.elements[index];
        if (element.offset != pinned[index].offset || element.type != pinned[index].type
            || element.usageName != pinned[index].usage
            || element.usageIndex != (index == 0 || index == 1 ? 0u : 0u))
        {
            *error = "declaration element " + std::to_string(index) + " ('" + element.usageName
                + "' at offset " + std::to_string(element.offset) + " type "
                + std::to_string(element.type) + ") is not the pinned lit layout";
            return false;
        }
    }
    return true;
}

inline bool CheckCapture(const Bundle& bundle, std::string* error)
{
    const Capture& capture = bundle.capture;
    const std::uint64_t expected = static_cast<std::uint64_t>(kLightmapWidth) * kLightmapHeight * 4u;

    if (capture.format != kLightmapFormat || capture.levels != kLightmapLevels
        || capture.flags != kLightmapFlags)
    {
        *error = "the captured lightmap records format " + std::to_string(capture.format)
            + " levels " + std::to_string(capture.levels) + " flags " + std::to_string(capture.flags)
            + "; this profile binds one level of D3DFMT_A8R8G8B8 (" + std::to_string(kLightmapFormat)
            + ") with flags " + std::to_string(kLightmapFlags);
        return false;
    }
    if (capture.width != kLightmapWidth || capture.height != kLightmapHeight || capture.depth != 1u)
    {
        *error = "the captured lightmap is " + std::to_string(capture.width) + "x"
            + std::to_string(capture.height) + "x" + std::to_string(capture.depth) + ", expected "
            + std::to_string(kLightmapWidth) + "x" + std::to_string(kLightmapHeight) + "x1";
        return false;
    }
    if (capture.bytes != expected || capture.payload.size() != expected)
    {
        *error = "the captured lightmap records " + std::to_string(capture.bytes)
            + " bytes and the payload is " + std::to_string(capture.payload.size())
            + "; " + std::to_string(kLightmapWidth) + "x" + std::to_string(kLightmapHeight)
            + " A8R8G8B8 is exactly " + std::to_string(expected);
        return false;
    }
    if (bundle.lightmapIndex != 0u)
    {
        *error = "surface.lightmap_index is " + std::to_string(bundle.lightmapIndex)
            + ", but the capture is lightmap 0";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Loader.
// ---------------------------------------------------------------------------
inline void ReadElement(const JsonValue& entry, Element* element, std::string* error)
{
    std::uint32_t order = 0;
    if (!world_surface_bundle::GetU32(entry, "offset", &element->offset, error)
        || !world_surface_bundle::GetU32(entry, "element_type", &element->type, error)
        || !world_surface_bundle::GetU32(entry, "usage_index", &element->usageIndex, error)
        || !world_surface_bundle::GetString(entry, "usage_name", &element->usageName, error)
        || !world_surface_bundle::GetU32(entry, "routing_order", &order, error))
        return;
    std::uint32_t stream = 0;
    if (!world_surface_bundle::GetU32(entry, "stream", &stream, error))
        return;
    if (stream != 0u)
        *error = "declaration element '" + element->usageName + "' comes from stream "
            + std::to_string(stream) + "; this replay binds one stream";
}

inline bool ReadShader(const JsonValue& shaders, const std::string& directory, const char* role,
    bool vertexStage, Shader* shader, std::string* error)
{
    const JsonValue* record = nullptr;
    if (!world_surface_bundle::GetObject(shaders, role, &record, error))
        return false;
    if (!world_surface_bundle::GetString(*record, "name", &shader->name, error)
        || !world_surface_bundle::GetString(*record, "file", &shader->file, error)
        || !world_surface_bundle::GetU32(*record, "word_count", &shader->words, error)
        || !world_surface_bundle::GetU32(*record, "byte_count", &shader->bytes, error))
        return false;

    std::string recorded;
    if (!world_surface_bundle::GetString(*record, "fnv1a64", &recorded, error))
        return false;
    if (!world_surface_bundle::ParseFnv1a64(recorded, &shader->fnv))
    {
        *error = std::string("the ") + role + " program's fnv1a64 is not 16 hex digits";
        return false;
    }
    if (shader->bytes != shader->words * 4u || shader->bytes == 0
        || shader->bytes > kMaxShaderBytes)
    {
        *error = std::string("the ") + role + " program records " + std::to_string(shader->words)
            + " DWORDs / " + std::to_string(shader->bytes) + " bytes";
        return false;
    }
    if (!world_surface_bundle::ReadWholeFile(world_surface_bundle::JoinPath(directory, shader->file),
            &shader->payload, error))
        return false;
    if (shader->payload.size() != shader->bytes)
    {
        *error = std::string("'") + shader->file + "' is " + std::to_string(shader->payload.size())
            + " bytes, the bundle records " + std::to_string(shader->bytes);
        return false;
    }
    if (world_surface_bundle::Fnv1a64(shader->payload.data(), shader->payload.size()) != shader->fnv)
    {
        *error = std::string("fnv1a64 mismatch for '") + shader->file + "'";
        return false;
    }

    // The value the pair is pinned to, not the value the bundle claims. `shader->fnv` above only
    // says the file matches its own record; this says the file is one of the two audited programs.
    const std::uint64_t expectedFnv =
        vertexStage ? kExpectedVertexShaderFnv : kExpectedPixelShaderFnv;
    const std::uint32_t expectedWords =
        vertexStage ? kExpectedVertexShaderWords : kExpectedPixelShaderWords;
    const std::uint32_t expectedBytes =
        vertexStage ? kExpectedVertexShaderBytes : kExpectedPixelShaderBytes;
    if (shader->fnv != expectedFnv || shader->words != expectedWords
        || shader->bytes != expectedBytes || shader->payload.size() != expectedBytes)
    {
        *error = std::string("the ") + role + " program '" + shader->name + "' is "
            + std::to_string(shader->words) + " DWORDs / " + std::to_string(shader->payload.size())
            + " bytes, FNV-1a-64 " + FnvHex(shader->fnv)
            + "; this profile is written against exactly " + std::to_string(expectedWords)
            + " DWORDs / " + std::to_string(expectedBytes) + " bytes, FNV-1a-64 "
            + FnvHex(expectedFnv);
        return false;
    }

    // Complete SM3 framing with the version token for this stage: the blob is a whole program,
    // whatever it computes. The audit, not this check, is what knows the instructions.
    world_surface_bundle::ShaderFraming framing{};
    std::string framingError;
    if (!world_surface_bundle::ValidateShaderBytecode(shader->payload, vertexStage, shader->words,
            &framing, &framingError))
    {
        *error = std::string("the ") + role + " program is not a walkable SM3 program: "
            + framingError;
        return false;
    }
    return true;
}

inline bool Load(const std::string& bundlePath, Bundle* bundle, std::string* error)
{
    std::vector<std::uint8_t> document;
    if (!world_surface_bundle::ReadWholeFile(bundlePath, &document, error))
        return false;
    const std::string text(document.begin(), document.end());

    JsonValue root;
    {
        world_surface_bundle::detail::Parser parser(text.data(), text.size());
        parser.SetOrigin(text.data());
        if (!parser.ParseDocument(&root, error))
        {
            *error = "bundle JSON: " + *error;
            return false;
        }
    }

    std::uint32_t schema = 0;
    std::string name;
    if (!world_surface_bundle::GetU32(root, "schema_version", &schema, error)
        || !world_surface_bundle::GetString(root, "document", &name, error))
        return false;
    if (schema != kSchemaVersion || name != kDocumentName)
    {
        *error = "this is '" + name + "' schema " + std::to_string(schema)
            + ", not a schema-1 lit world surface bundle. An unlit bundle has a three-element "
              "declaration, a code-40 argument and a different document, and is refused here";
        return false;
    }

    if (!world_surface_bundle::GetBool(root, "render_ready", &bundle->renderReady, error)
        || !world_surface_bundle::GetBool(root, "replay_claimed", &bundle->replayClaimed, error))
        return false;
    if (bundle->renderReady || bundle->replayClaimed)
    {
        *error = "the bundle claims render_ready or replay_claimed; this replay is the party that "
                 "decides what was replayed";
        return false;
    }

    // --- caps --------------------------------------------------------------
    // The exporter's own limits. Read and pinned so the counts below are checked against a
    // recorded cap rather than only against this file's constants, and so a bundle that widened a
    // cap cannot present a widened count as authorised.
    {
        const JsonValue* caps = nullptr;
        if (!world_surface_bundle::GetObject(root, "caps", &caps, error))
            return false;
        std::uint32_t maxVertices = 0;
        std::uint32_t maxIndices = 0;
        std::uint32_t maxShaderBytes = 0;
        std::uint32_t maxBundleBytes = 0;
        if (!world_surface_bundle::GetU32(*caps, "max_bundle_vertices", &maxVertices, error)
            || !world_surface_bundle::GetU32(*caps, "max_bundle_indices", &maxIndices, error)
            || !world_surface_bundle::GetU32(*caps, "max_bundle_bytes", &maxBundleBytes, error)
            || !world_surface_bundle::GetU32(*caps, "max_shader_bytes", &maxShaderBytes, error))
            return false;
        if (static_cast<std::uint64_t>(maxVertices) != kMaxBundleVertices
            || static_cast<std::uint64_t>(maxIndices) != kMaxBundleIndices
            || static_cast<std::uint64_t>(maxBundleBytes) != kMaxBundleBytes
            || static_cast<std::uint64_t>(maxShaderBytes) != kMaxShaderBytes)
        {
            *error = "caps are " + std::to_string(maxVertices) + " vertices / "
                + std::to_string(maxIndices) + " indices / " + std::to_string(maxBundleBytes)
                + " bytes / " + std::to_string(maxShaderBytes) + " shader bytes; this profile binds "
                + std::to_string(kMaxBundleVertices) + " / " + std::to_string(kMaxBundleIndices)
                + " / " + std::to_string(kMaxBundleBytes) + " / "
                + std::to_string(kMaxShaderBytes);
            return false;
        }
    }

    // --- request / surface -------------------------------------------------
    const JsonValue* request = nullptr;
    const JsonValue* surface = nullptr;
    if (!world_surface_bundle::GetObject(root, "request", &request, error)
        || !world_surface_bundle::GetObject(root, "surface", &surface, error))
        return false;
    std::string techniqueValue;
    if (!world_surface_bundle::GetString(*request, "map", &bundle->map, error)
        || !world_surface_bundle::GetU32(*request, "surface_index", &bundle->surfaceIndex, error)
        || !world_surface_bundle::GetString(*request, "technique_value", &techniqueValue, error))
        return false;
    if (techniqueValue != "lit")
    {
        *error = "request.technique_value is '" + techniqueValue
            + "', expected 'lit': this gate replays the lit technique and will not accept an "
              "export that asked for something else";
        return false;
    }
    if (!world_surface_bundle::GetU32(*surface, "flags", &bundle->surfaceFlags, error)
        || !world_surface_bundle::GetU32(*surface, "lightmap_index", &bundle->lightmapIndex, error)
        || !world_surface_bundle::GetU32(*surface, "reflection_probe_index",
            &bundle->reflectionProbeIndex, error))
        return false;

    // --- geometry ----------------------------------------------------------
    const JsonValue* geometry = nullptr;
    if (!world_surface_bundle::GetObject(root, "geometry", &geometry, error))
        return false;
    if (!world_surface_bundle::GetU32(*geometry, "vertex_count", &bundle->vertexCount, error)
        || !world_surface_bundle::GetU32(*geometry, "tri_count", &bundle->triCount, error)
        || !world_surface_bundle::GetU32(*geometry, "index_count", &bundle->indexCount, error)
        || !world_surface_bundle::GetU32(*geometry, "vertex_stride", &bundle->vertexStride, error))
        return false;
    // Stride first: every byte count below is derived from it, so a widened stride would widen
    // the arithmetic that follows.
    if (bundle->vertexStride != 44u)
    {
        *error = "geometry.vertex_stride is " + std::to_string(bundle->vertexStride)
            + ", expected 44 (position 0, colour 16, texcoord 20, normal 36)";
        return false;
    }
    if (bundle->vertexCount == 0)
    {
        *error = "geometry.vertex_count is zero";
        return false;
    }
    if (bundle->vertexCount > kMaxBundleVertices)
    {
        *error = "geometry.vertex_count " + std::to_string(bundle->vertexCount)
            + " exceeds the recorded cap of " + std::to_string(kMaxBundleVertices);
        return false;
    }
    if (bundle->indexCount == 0 || bundle->indexCount % 3u != 0)
    {
        *error = "geometry.index_count is " + std::to_string(bundle->indexCount)
            + ", not a positive multiple of three";
        return false;
    }
    if (bundle->indexCount > kMaxBundleIndices)
    {
        *error = "geometry.index_count " + std::to_string(bundle->indexCount)
            + " exceeds the recorded cap of " + std::to_string(kMaxBundleIndices);
        return false;
    }
    if (bundle->triCount != bundle->indexCount / 3u)
    {
        *error = "geometry.tri_count is " + std::to_string(bundle->triCount) + " but index_count / "
            + "3 is " + std::to_string(bundle->indexCount / 3u)
            + "; the draw is one triangle per three indices";
        return false;
    }
    // Widened to 64 bits before multiplying: vertex_count * 44 overflows 32 bits for a large
    // slice, and an overflowing product can equal a small recorded byte count.
    const std::uint64_t expectedVertexBytes =
        static_cast<std::uint64_t>(bundle->vertexCount) * 44ull;
    const std::uint64_t expectedIndexBytes =
        static_cast<std::uint64_t>(bundle->indexCount) * static_cast<std::uint64_t>(kIndexStride);
    if (expectedVertexBytes + expectedIndexBytes > kMaxBundleBytes)
    {
        *error = "the geometry alone is " + std::to_string(expectedVertexBytes + expectedIndexBytes)
            + " bytes, over the recorded bundle cap of " + std::to_string(kMaxBundleBytes);
        return false;
    }


    // --- material / technique ----------------------------------------------
    const JsonValue* material = nullptr;
    const JsonValue* technique = nullptr;
    if (!world_surface_bundle::GetObject(root, "material", &material, error)
        || !world_surface_bundle::GetObject(root, "technique", &technique, error))
        return false;
    if (!world_surface_bundle::GetString(*material, "name", &bundle->materialName, error)
        || !world_surface_bundle::GetU32(*technique, "type", &bundle->techniqueType, error)
        || !world_surface_bundle::GetString(*technique, "name", &bundle->techniqueName, error))
        return false;
    if (bundle->techniqueType != kTechniqueTypeLit)
    {
        *error = "technique.type is " + std::to_string(bundle->techniqueType)
            + ", expected 7 (TECHNIQUE_LIT)";
        return false;
    }
    if (bundle->techniqueName != kTechniqueName)
    {
        *error = "technique.name is '" + bundle->techniqueName + "', but this profile is written "
            "against '" + kTechniqueName + "': a different technique is a different program pair";
        return false;
    }

    // --- declaration -------------------------------------------------------
    const JsonValue* declaration = nullptr;
    if (!world_surface_bundle::GetObject(root, "declaration", &declaration, error))
        return false;
    const JsonValue* elements = nullptr;
    if (!world_surface_bundle::GetArray(*declaration, "elements", &elements, error))
        return false;
    for (const auto& entry : elements->items)
    {
        Element element{};
        ReadElement(entry, &element, error);
        if (!error->empty())
            return false;
        bundle->elements.push_back(element);
    }
    if (!CheckDeclaration(*bundle, error))
        return false;

    // --- pass --------------------------------------------------------------
    const JsonValue* pass = nullptr;
    if (!world_surface_bundle::GetObject(root, "pass", &pass, error))
        return false;

    const JsonValue* state = nullptr;
    if (!world_surface_bundle::GetObject(*pass, "state", &state, error))
        return false;
    std::uint32_t flags = 0;
    if (!world_surface_bundle::GetU32(*state, "custom_sampler_flags", &flags, error))
        return false;
    if (flags != kCustomSamplerFlags)
    {
        *error = "pass custom_sampler_flags is 0x" + std::to_string(flags) + ", expected 0x4. "
            "This gate captures the secondary lightmap only, so a pass that also asks for the "
            "reflection probe is refused rather than drawn with an unattached stage";
        return false;
    }
    const JsonValue* loadBits = nullptr;
    if (!world_surface_bundle::GetArray(*state, "load_bits", &loadBits, error))
        return false;
    if (loadBits->items.size() != 2)
    {
        *error = "pass.state.load_bits is not two words";
        return false;
    }
    bundle->stateBits0 = static_cast<std::uint32_t>(loadBits->items[0].number);
    bundle->stateBits1 = static_cast<std::uint32_t>(loadBits->items[1].number);

    const JsonValue* samplers = nullptr;
    if (!world_surface_bundle::GetArray(*pass, "custom_samplers", &samplers, error))
        return false;
    if (samplers->items.size() != 1)
    {
        *error = "pass.custom_samplers has " + std::to_string(samplers->items.size())
            + " entries, expected 1";
        return false;
    }
    {
        std::uint32_t stage = 0;
        std::uint32_t samplerState = 0;
        std::string resource;
        if (!world_surface_bundle::GetU32(samplers->items[0], "stage", &stage, error)
            || !world_surface_bundle::GetU32(samplers->items[0], "sampler_state", &samplerState, error)
            || !world_surface_bundle::GetString(samplers->items[0], "resource", &resource, error))
            return false;
        if (stage != kLightmapStage || samplerState != kLightmapSamplerState
            || resource != kLightmapResource)
        {
            *error = "the custom sampler is stage " + std::to_string(stage) + " state "
                + std::to_string(samplerState) + " resource '" + resource
                + "', expected stage 3 state 98 ('lightmap_secondary')";
            return false;
        }
    }

    const JsonValue* arguments = nullptr;
    if (!world_surface_bundle::GetObject(*pass, "arguments", &arguments, error))
        return false;
    for (const char* group : {"per_prim", "per_object", "stable"})
    {
        const JsonValue* entries = nullptr;
        if (!world_surface_bundle::GetArray(*arguments, group, &entries, error))
            return false;
        for (const auto& entry : entries->items)
        {
            if (const JsonValue* texture = entry.Member("material_texture"))
            {
                TextureArgument argument{};
                argument.group = group;
                if (!world_surface_bundle::GetU32(entry, "type", &argument.type, error)
                    || !world_surface_bundle::GetU32(entry, "dest", &argument.dest, error)
                    || !world_surface_bundle::GetU32(entry, "name_hash", &argument.nameHash, error)
                    || !world_surface_bundle::GetU32(*texture, "index", &argument.index, error)
                    || !world_surface_bundle::GetU32(*texture, "semantic", &argument.semantic, error)
                    || !world_surface_bundle::GetU32(*texture, "sampler_state",
                        &argument.samplerState, error))
                    return false;
                const JsonValue* image = nullptr;
                if (!world_surface_bundle::GetObject(*texture, "image", &image, error)
                    || !world_surface_bundle::GetString(*image, "name", &argument.image, error))
                    return false;
                bundle->textureArguments.push_back(argument);
                continue;
            }

            CodeArgument argument{};
            argument.group = group;
            if (!world_surface_bundle::GetU32(entry, "type", &argument.type, error)
                || !world_surface_bundle::GetU32(entry, "dest", &argument.dest, error)
                || !world_surface_bundle::GetU32(entry, "code_index", &argument.code, error)
                || !world_surface_bundle::GetU32(entry, "row_count", &argument.rows, error))
                return false;
            std::uint32_t firstRow = 0;
            if (!world_surface_bundle::GetU32(entry, "first_row", &firstRow, error))
                return false;
            if (firstRow != 0u)
            {
                *error = "code " + std::to_string(argument.code) + " starts at row "
                    + std::to_string(firstRow) + ", but the context holds row 0 only";
                return false;
            }
            bundle->codeArguments.push_back(argument);
        }
    }
    if (!CheckArguments(*bundle, error))
        return false;

    // The directory every payload path below is resolved against, hoisted so the payload and
    // shader readers share one resolution of the bundle's own location.
    const std::string directory = world_surface_bundle::DirectoryOf(bundlePath);

    // --- texture dependencies ----------------------------------------------
    const JsonValue* dependencies = nullptr;
    if (!world_surface_bundle::GetArray(root, "texture_dependencies", &dependencies, error))
        return false;
    for (const auto& entry : dependencies->items)
    {
        TextureDependency dependency{};
        std::string payload;
        if (!world_surface_bundle::GetU32(entry, "slot", &dependency.slot, error)
            || !world_surface_bundle::GetU32(entry, "semantic", &dependency.semantic, error)
            || !world_surface_bundle::GetU32(entry, "name_hash", &dependency.nameHash, error)
            || !world_surface_bundle::GetU32(entry, "sampler_state", &dependency.samplerState, error)
            || !world_surface_bundle::GetString(entry, "image_name", &dependency.image, error)
            || !world_surface_bundle::GetString(entry, "iwi_entry", &dependency.iwiEntry, error)
            || !world_surface_bundle::GetString(entry, "payload", &payload, error))
            return false;
        if (payload != "external" || dependency.iwiEntry != "images/" + dependency.image + ".iwi")
        {
            *error = "texture dependency '" + dependency.image
                + "' is not an external image with a derived IWI entry";
            return false;
        }
        bundle->dependencies.push_back(dependency);
    }
    if (bundle->textureArguments.empty())
    {
        *error = "the pass binds no material colour map, so there is nothing to resolve in "
                 "texture_dependencies";
        return false;
    }
    // The bound dependency is the one the pass's sampler argument names: same slot (the
    // material texture index), same semantic, same name hash, same image. Matching on the image
    // name alone would accept a dependency recorded under a different slot, semantic or hash,
    // which is a different binding with a familiar name.
    {
        const TextureArgument& argument = bundle->textureArguments.front();
        for (TextureDependency& dependency : bundle->dependencies)
        {
            if (dependency.slot != argument.index || dependency.semantic != argument.semantic
                || dependency.nameHash != argument.nameHash || dependency.image != argument.image)
                continue;
            dependency.bound = true;
            if (dependency.samplerState != argument.samplerState)
            {
                *error = "texture dependency '" + dependency.image + "' records sampler "
                    + std::to_string(dependency.samplerState) + ", the pass binds "
                    + std::to_string(argument.samplerState);
                return false;
            }
        }
    }
    {
        std::size_t boundCount = 0;
        for (const TextureDependency& dependency : bundle->dependencies)
            if (dependency.bound)
                ++boundCount;
        if (boundCount != 1)
        {
            *error = "the pass's colour map matches " + std::to_string(boundCount)
                + " recorded texture dependencies, expected 1";
            return false;
        }
    }

    // --- payloads ----------------------------------------------------------
    const JsonValue* files = nullptr;
    if (!world_surface_bundle::GetArray(root, "files", &files, error))
        return false;
    world_surface_bundle::FileRecord record{};
    std::uint64_t hash = 0;
    if (!world_surface_bundle::LoadPayload(*files, directory, "world_vertices", &bundle->vertices,
            &hash, &record, error))
        return false;
    if (record.count != bundle->vertexCount || record.stride != bundle->vertexStride)
    {
        *error = "files[] records " + std::to_string(record.count) + " vertices at stride "
            + std::to_string(record.stride) + ", geometry records "
            + std::to_string(bundle->vertexCount) + " at " + std::to_string(bundle->vertexStride);
        return false;
    }
    if (bundle->vertices.size() != expectedVertexBytes)
    {
        *error = "the vertex payload is " + std::to_string(bundle->vertices.size())
            + " bytes; " + std::to_string(bundle->vertexCount) + " vertices at stride 44 is "
            + std::to_string(expectedVertexBytes);
        return false;
    }
    if (!world_surface_bundle::LoadPayload(*files, directory, "world_indices", &bundle->indices,
            &hash, &record, error))
        return false;
    if (record.count != bundle->indexCount || record.stride != kIndexStride)
    {
        *error = "files[] records " + std::to_string(record.count) + " indices at stride "
            + std::to_string(record.stride) + ", geometry records "
            + std::to_string(bundle->indexCount) + " at 2";
        return false;
    }
    if (bundle->indices.size() != expectedIndexBytes)
    {
        *error = "the index payload is " + std::to_string(bundle->indices.size())
            + " bytes; " + std::to_string(bundle->indexCount) + " 16-bit indices is "
            + std::to_string(expectedIndexBytes);
        return false;
    }

    // Every surface-local index must address this slice. The walk is bounded by the recorded
    // index count, which the byte size above has already tied to the payload, so no index past
    // the draw's own cap is ever read or skipped.
    for (std::size_t index = 0; index < bundle->indexCount; ++index)
    {
        std::uint16_t value = 0;
        std::memcpy(&value, bundle->indices.data() + index * kIndexStride, sizeof(value));
        if (value >= bundle->vertexCount)
        {
            *error = "index " + std::to_string(index) + " is " + std::to_string(value)
                + ", outside the " + std::to_string(bundle->vertexCount) + "-vertex slice";
            return false;
        }
    }

    // --- capture -----------------------------------------------------------
    const JsonValue* capture = nullptr;
    if (!world_surface_bundle::GetObject(root, "capture", &capture, error))
        return false;
    bool required = false;
    bool available = false;
    if (!world_surface_bundle::GetBool(*capture, "required", &required, error)
        || !world_surface_bundle::GetBool(*capture, "available", &available, error)
        || !world_surface_bundle::GetString(*capture, "resource", &bundle->capture.resource, error)
        || !world_surface_bundle::GetU32(*capture, "bytes", &bundle->capture.bytes, error)
        || !world_surface_bundle::GetU32(*capture, "format", &bundle->capture.format, error)
        || !world_surface_bundle::GetU32(*capture, "level_count", &bundle->capture.levels, error)
        || !world_surface_bundle::GetU32(*capture, "flags", &bundle->capture.flags, error)
        || !world_surface_bundle::GetString(*capture, "payload_file", &bundle->capture.file, error))
        return false;
    std::string recorded;
    if (!world_surface_bundle::GetString(*capture, "fnv1a64", &recorded, error)
        || !world_surface_bundle::ParseFnv1a64(recorded, &bundle->capture.fnv))
    {
        *error = "capture.fnv1a64 is not 16 hex digits";
        return false;
    }
    const JsonValue* dimensions = nullptr;
    if (!world_surface_bundle::GetArray(*capture, "dimensions", &dimensions, error))
        return false;
    if (dimensions->items.size() != 3)
    {
        *error = "capture.dimensions is not three numbers";
        return false;
    }
    bundle->capture.width = static_cast<std::uint32_t>(dimensions->items[0].number);
    bundle->capture.height = static_cast<std::uint32_t>(dimensions->items[1].number);
    bundle->capture.depth = static_cast<std::uint32_t>(dimensions->items[2].number);
    if (!world_surface_bundle::ReadWholeFile(
            world_surface_bundle::JoinPath(directory, bundle->capture.file),
            &bundle->capture.payload, error))
        return false;
    if (world_surface_bundle::Fnv1a64(bundle->capture.payload.data(), bundle->capture.payload.size())
        != bundle->capture.fnv)
    {
        *error = "the captured lightmap does not match its recorded fnv1a64";
        return false;
    }
    if (!required || !available || !CheckCapture(*bundle, error))
    {
        if (error->empty())
            *error = "the captured lightmap is not required/available";
        return false;
    }

    // --- shaders -----------------------------------------------------------
    // Last, and deliberately so: the bytecode is the largest thing the loader reads, and
    // a bundle that is wrong about its own geometry, its dependencies or its capture
    // should be refused for that reason before any of it is hashed.
    const JsonValue* shaders = nullptr;
    if (!world_surface_bundle::GetObject(*pass, "shaders", &shaders, error))
        return false;
    if (!ReadShader(*shaders, directory, "vertex", true, &bundle->vertexShader, error)
        || !ReadShader(*shaders, directory, "pixel", false, &bundle->pixelShader, error))
        return false;
    if (bundle->vertexShader.fnv == bundle->pixelShader.fnv)
    {
        *error = "the vertex and pixel programs have identical bytes; a lit pass is a pair";
        return false;
    }
    if (bundle->pixelShader.name != bundle->techniqueName + ".hlsl")
    {
        *error = "the pixel program is '" + bundle->pixelShader.name + "', but the technique is '"
            + bundle->techniqueName + "'";
        return false;
    }
    if (bundle->vertexShader.name != kExpectedVertexShaderName)
    {
        *error = "the vertex program is '" + bundle->vertexShader.name + "', expected '"
            + kExpectedVertexShaderName + "': the audited pair is one vertex and one pixel program";
        return false;
    }

    if (!world_surface_bundle::GetBool(root, "all_requirements_met", &bundle->allRequirementsMet,
            error))
        return false;
    return true;
}

// A short report of what was loaded. The bundle's own all_requirements_met is printed as the
// summary it is; whether the pass can be drawn is decided by the profile checks above.
inline std::string Describe(const Bundle& bundle)
{
    std::string text = "lit profile: " + bundle.map + " surface " + std::to_string(bundle.surfaceIndex)
        + ", technique " + bundle.techniqueName + " (type " + std::to_string(bundle.techniqueType)
        + "), material " + bundle.materialName + "\n";

    text += "  declaration: ";
    for (std::size_t index = 0; index < bundle.elements.size(); ++index)
    {
        const Element& element = bundle.elements[index];
        if (index != 0)
            text += ", ";
        text += element.usageName + "@" + std::to_string(element.offset) + " type "
            + std::to_string(element.type);
    }
    text += "\n  constants: ";
    for (std::size_t index = 0; index < bundle.codeArguments.size(); ++index)
    {
        const CodeArgument& argument = bundle.codeArguments[index];
        if (index != 0)
            text += ", ";
        text += "code " + std::to_string(argument.code) + "->" + std::to_string(argument.dest) + "x"
            + std::to_string(argument.rows);
    }
    text += "; code 40 absent\n";

    for (const TextureDependency& dependency : bundle.dependencies)
        text += "  texture: slot " + std::to_string(dependency.slot) + " '" + dependency.image + "'"
            + (dependency.bound ? " BOUND" : " unbound (not in this draw's closure)") + "\n";

    text += "  lightmap: " + bundle.capture.file + " " + std::to_string(bundle.capture.width) + "x"
        + std::to_string(bundle.capture.height) + " format " + std::to_string(bundle.capture.format)
        + " levels " + std::to_string(bundle.capture.levels) + " " + std::to_string(bundle.capture.bytes)
        + " bytes, measured FNV-1a-64 "
        + FnvHex(world_surface_bundle::Fnv1a64(bundle.capture.payload.data(),
              bundle.capture.payload.size()))
        + "\n";
    // The two hashes printed here are measured from the payload bytes in memory, not copied from
    // the bundle's fnv1a64 fields, so the report shows what was hashed rather than what was asked
    // for. The loader has already refused anything but the audited pair.
    text += "  programs: VS " + bundle.vertexShader.name + " "
        + std::to_string(bundle.vertexShader.words) + " DWORDs, FNV-1a-64 "
        + FnvHex(world_surface_bundle::Fnv1a64(bundle.vertexShader.payload.data(),
              bundle.vertexShader.payload.size()))
        + "; PS " + bundle.pixelShader.name + " " + std::to_string(bundle.pixelShader.words)
        + " DWORDs, FNV-1a-64 "
        + FnvHex(world_surface_bundle::Fnv1a64(bundle.pixelShader.payload.data(),
              bundle.pixelShader.payload.size()))
        + "; both framed\n";
    text += "  geometry: " + std::to_string(bundle.vertexCount) + " vertices stride "
        + std::to_string(bundle.vertexStride) + ", " + std::to_string(bundle.triCount)
        + " triangles surface-local\n";
    text += std::string("  bundle all_requirements_met=")
        + (bundle.allRequirementsMet ? "true" : "false")
        + " (the export's own summary; it is not a drawable verdict and is not treated as one)\n";
    return text;
}

} // namespace world_lit_bundle
