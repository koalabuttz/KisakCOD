#pragma once

// The Legacy shader translator: the game's Direct3D 9 shader bytecode (vs/ps
// 1.1 to 3.0) to SPIR-V for the Vulkan back end's Legacy path.
//
// dxbc-spirv (MIT, the converter DXVK 3.1 uses for D3D9) does the conversion.
// What D3D9 keeps outside the shader (constant registers, alpha test, fog,
// clip planes, point sprites, sampler and texture stage state) reaches the
// converted shader as "legacy inputs"; this file lowers each one to a uniform
// buffer at a fixed binding (the Legacy descriptor layout below), records what
// every shader reads (Reflection), and links a pixel shader's inputs to the
// vertex shader it is drawn with.
//
// Two steps, so the slow one can be cached:
//   Translate   bytecode -> Shader (legalised IR + reflection). Once per
//               shader, at load; the result can be stored and reused.
//   BuildSpirv  Shader (+ the vertex shader, for a pixel shader) -> SPIR-V
//               for Vulkan 1.3. Once per pipeline's shader pair; usually only
//               copies, unless the two stages' I/O need linking.
//
// Nothing here knows Vulkan, the engine or game data. Thread-safe: calls on
// different Shader objects may run on different threads at once.
// See docs/vulkan-legacy-shaders.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gfx_vk::legacy_shader
{
enum class Stage : uint8_t
{
    Vertex,
    Pixel,
};

// ---------------------------------------------------------------------------
// The Legacy descriptor layout
//
// Set 0 holds uniform buffers, one binding per legacy input and stage; set 1
// holds the textures and samplers, separately (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
// and VK_DESCRIPTOR_TYPE_SAMPLER). Every binding is optional: a shader declares
// only what it reads (Reflection::uniforms, Reflection::samplers).
// ---------------------------------------------------------------------------
constexpr uint32_t kUniformSet = 0;
constexpr uint32_t kTextureSet = 1;

enum class Uniform : uint8_t
{
    FloatConstants, // vec4 c[N], N = Reflection::floatConstants
    IntConstants,   // ivec4 i[N], N = Reflection::intConstants
    BoolConstants,  // uint b[16], one per register; nonzero = true
    AlphaTest,      // AlphaTestBlock (pixel shaders)
    Fog,            // FogBlock
    ClipPlanes,     // ClipPlaneBlock (vertex shaders)
    PointArgs,      // PointArgsBlock
    SamplerState,   // SamplerStateBlock[16] (pixel shaders)
    TextureStage,   // TextureStageBlock[16] (pixel shaders, ps 1.x bump mapping)
    Count,
};
constexpr uint32_t kUniformCount = static_cast<uint32_t>(Uniform::Count);
constexpr uint32_t kSamplerCount = 16; // ps s0-s15; vs s0-s3 (D3DVERTEXTEXTURESAMPLER0-3)

// Set 0: vertex shader blocks at 0-8, pixel shader blocks at 16-24.
constexpr uint32_t UniformBinding(Stage stage, Uniform uniform)
{
    return (stage == Stage::Vertex ? 0u : 16u) + static_cast<uint32_t>(uniform);
}
// Set 1: pixel shader images at 0-15 and samplers at 16-31; vertex shader
// images at 32-47 and samplers at 48-63.
constexpr uint32_t ImageBinding(Stage stage, uint32_t sampler)
{
    return (stage == Stage::Vertex ? 32u : 0u) + sampler;
}
constexpr uint32_t SamplerBinding(Stage stage, uint32_t sampler)
{
    return (stage == Stage::Vertex ? 48u : 16u) + sampler;
}

// The blocks' layouts. dxbc-spirv lays uniform blocks out tightly (scalar
// block layout, Vulkan 1.2's scalarBlockLayout, which the core requires), so
// these structs match them byte for byte; scripts/tests/test_vulkan_legacy_shaders.py
// checks the offsets the generated SPIR-V declares. Constant arrays: float and
// int registers have a 16-byte stride, bools a 4-byte stride (uint b[16]).
// Bools are 32-bit, nonzero = true.
struct AlphaTestBlock
{
    uint32_t compareOp;     // AlphaCompare below; Always skips the test
    uint32_t precisionBits; // extra precision bits beyond 8 (0 = compare as 8-bit)
    uint32_t alphaRef;      // D3DRS_ALPHAREF, 0-255
};
enum class AlphaCompare : uint32_t // D3DCMPFUNC - 1
{
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7,
};
struct FogBlock
{
    uint32_t enable;
    uint32_t useZ;     // fog from depth (true) or from the vertex fog value
    uint32_t mode;     // D3DFOGMODE: 0 none, 1 exp, 2 exp2, 3 linear
    float color[3];
    float scale;       // linear: 1 / (end - start)
    float end;
    float density;
};
struct ClipPlaneBlock
{
    uint32_t count;
    float planes[6][4];
};
struct PointArgsBlock
{
    uint32_t isPointSprite;
    float size;
    float sizeMin;
    float sizeMax;
};
struct SamplerStateBlock
{
    uint32_t textureType;     // bound texture: 0 2D, 1 3D, 2 cube (dxbc-spirv's SamplerStateType)
    uint32_t useDepthCompare; // bound texture is a depth format: compare against the reference
    uint32_t useProjection;   // ps 1.x: D3DTTFF_PROJECTED
    uint32_t isNull;          // nothing bound: samples return (0, 0, 0, 1)
    uint32_t useGather;
    uint32_t drefClamp;
    float drefScale;
};
struct TextureStageBlock
{
    float bumpMat[2][2];
    float bumpScale;
    float bumpOffset;
};
static_assert(sizeof(AlphaTestBlock) == 12 && sizeof(FogBlock) == 36 && offsetof(FogBlock, color) == 12 &&
                  offsetof(FogBlock, scale) == 24 && sizeof(ClipPlaneBlock) == 100 &&
                  offsetof(ClipPlaneBlock, planes) == 4 && sizeof(PointArgsBlock) == 16 &&
                  sizeof(SamplerStateBlock) == 28 && sizeof(TextureStageBlock) == 24,
              "the Legacy uniform blocks must match the scalar layout the shaders declare");

enum class TextureType : uint8_t
{
    None,  // the shader does not sample this sampler
    Tex2D,
    Cube,
    Tex3D,
    Any,   // ps 1.x: no declared type; the shader declares all three at the
           // same binding and picks by SamplerStateBlock::textureType
};

// ---------------------------------------------------------------------------
// Reflection: what a shader reads and writes
// ---------------------------------------------------------------------------

// One shader input or output. Semantic names are dxbc-spirv's lower-case names
// ("position", "texcoord", "color", "normal", "fog", ...); builtIn entries
// (gl_Position, gl_FragDepth, ...) have no semantic.
struct IoEntry
{
    bool builtIn;
    uint8_t location;   // location, or dxbc-spirv's ir::BuiltIn value for builtIn
    uint8_t mask;       // components, bit 0 = x
    uint8_t semanticUsage; // D3DDECLUSAGE (0 position ... 12 depth); 0xff: none
    uint32_t semanticIndex;
    std::string semantic;
};

struct Reflection
{
    Stage stage = Stage::Vertex;
    uint8_t major = 0, minor = 0;

    // Constant registers to bind: highest register read + 1, 0 when none.
    // When floatIndexed is set the shader indexes c[] with a0/aL and every
    // register up to floatConstants (the stage's whole range) must be bound.
    uint32_t floatConstants = 0;
    bool floatIndexed = false;
    uint32_t intConstants = 0;
    uint32_t boolConstants = 0;

    uint32_t uniforms = 0;      // bit per Uniform the SPIR-V declares
    uint32_t samplers = 0;      // bit per sampler the SPIR-V declares
    TextureType samplerTypes[kSamplerCount] = {};

    std::vector<IoEntry> inputs;   // vertex shader: vertex attributes (location = attribute)
    std::vector<IoEntry> outputs;  // pixel shader: colour targets (location = target)
};

// A translated shader: legalised dxbc-spirv IR, serialised, plus reflection.
struct Shader
{
    Reflection reflection;
    std::vector<uint8_t> ir;
    uint32_t bytecodeHash = 0; // FNV-1a of the D3D9 bytecode, for logs and caches
    std::string warnings;      // the converter's warnings, if any
};

// How translated shaders multiply. D3D9 hardware gives 0 for 0 times anything,
// infinity and NaN included; Vulkan does not. Fast (the default, as DXVK on
// drivers without a native multiply of that kind, such as Intel's and
// NVIDIA's) multiplies plainly and keeps infinities out instead: rcp, rsq, exp
// and nrm are clamped to the largest float and log to the lowest, so a product
// never meets one. Strict checks both factors of every multiply for zero
// (dxbc-spirv's legacy multiply: two compares and two selects per component),
// which Mesa's Intel compiler made 4 times as long in vertex shaders (laptop
// run 17). Set once, before the first Translate; TranslatorRevision follows it,
// so a cache made under the other rule is not used.
void SetStrictFloats(bool strict);
bool StrictFloats();

// Translates D3D9 bytecode (a version token, then tokens, ending at the end
// token). On failure returns false and says why in error.
bool Translate(const void *bytecode, size_t bytes, Shader &shader, std::string &error);

// Builds SPIR-V (Vulkan 1.3, entry point "main"). For a pixel shader, pass the
// vertex shader it is drawn with: inputs the vertex shader does not write read
// as zero, and inputs it writes at another location are moved there. Pass
// nullptr to keep the pixel shader's own locations.
//
// fixedDepthCompare, when given, decides each sampler's depth compare at build
// time instead of by SamplerStateBlock::useDepthCompare: bit N set means
// sampler N is bound to a depth texture, clear means a colour one, and the
// branch not taken is removed. MoltenVK needs this: its shader converter
// (SPIRV-Cross) declares an image sampled both with and without a depth
// reference as a Metal depth texture, which cannot sample colour.
bool BuildSpirv(const Shader &shader, const Shader *vertexShader, std::vector<uint32_t> &spirv,
                std::string &error, const uint32_t *fixedDepthCompare = nullptr);

// True when the pixel shader's inputs read the vertex shader's outputs as they
// stand (BuildSpirv then needs no linking).
bool StagesLinkDirectly(const Shader &vertexShader, const Shader &pixelShader);

// dxbc-spirv's IR as text, for the tool's --ir.
std::string DisassembleIr(const Shader &shader);

// What makes translations differ between builds: a digest of the translator's
// sources and dxbc-spirv's commit (KISAK_LEGACY_SHADER_REVISION, from
// vulkan_shaders.cmake), and the float rule (SetStrictFloats). A cache of
// translated shaders keeps it and drops everything when it changes.
const char *TranslatorRevision();

// A translated shader as bytes, and back, for caches. Deserialize refuses
// anything malformed or cut short.
void Serialize(const Shader &shader, std::vector<uint8_t> &out);
bool Deserialize(const uint8_t *data, size_t size, Shader &shader);

// Names for logs and the tool.
const char *UniformName(Uniform uniform);
const char *TextureTypeName(TextureType type);
} // namespace gfx_vk::legacy_shader
