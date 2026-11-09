#pragma once

// Direct3D 9 shader bytecode to Cg, for the PS Vita's GXM back end.
//
// GXM runs programs that the Vita's own compiler (libshacccg, see
// gxm_shader_compiler.h) builds from Cg source, so each of the game's D3D9
// shaders (vs_1_1 to vs_3_0, ps_2_0 to ps_3_0) becomes a small Cg program:
// one float4 local per register, one statement per instruction, in the order
// the bytecode gives them. The shaders the game ships use no loops, calls or
// predicates; those, ps 1.x and a few rare registers are refused with a
// reason. Volume textures, which GXM lacks, are sampled from their slices
// laid out in a 2D texture.
//
// Two steps:
//   Parse  bytecode -> Shader (instructions + Reflection). Once per shader.
//   Emit   Shader + Options -> Cg text. Once per variant: the options carry
//          the draw state D3D9 keeps outside the shader (alpha test, vertex
//          colours stored in D3DCOLOR byte order).
//
// How the Cg program meets the rest of the back end:
//   - float constants: the uniform array "c", Reflection::floatConstants
//     long (256 for a vertex shader that indexes it with a0);
//   - samplers: uniforms "s0" to "s15" (Reflection::samplers); a volume
//     sampler is a sampler2D over its slices stacked vertically, with the
//     slice count in the uniform "kisak_depth_s<n>";
//   - vertex inputs: parameters "in_v0" to "in_v15", no semantics, bound by
//     name (Reflection::inputs gives each one's D3D9 usage);
//   - varyings: GXM semantics, TEXCOORD0-9, COLOR0-1, FOG, PSIZE, from the
//     D3D9 usage (texcoord n -> TEXCOORDn, color n -> COLORn);
//   - alpha test: the uniform "kisak_alpha_ref" (0 to 1) when
//     Options::alphaFunc tests anything.
//
// Nothing here knows sceGxm or the engine, so it builds and is tested on the
// host (scripts/tests/test_vita_cg_translate.py). Thread-safe per Shader.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gfx_gxm::cg
{
enum class Stage : uint8_t
{
    Vertex,
    Pixel,
};

enum class SamplerType : uint8_t
{
    None,
    Tex2D,
    Cube,
    Volume,
};

// GXM's varying semantics.
enum class Semantic : uint8_t
{
    Position,
    Texcoord, // index 0-9
    Color,    // index 0-1
    Fog,
    PointSize,
};
const char *SemanticName(Semantic semantic); // "TEXCOORD", ...

struct VertexInput
{
    uint8_t reg;        // v<reg>, parameter in_v<reg>
    uint8_t usage;      // D3DDECLUSAGE
    uint8_t usageIndex; //
    uint8_t mask;       // components the shader declares (bit 0 = x)
};

struct Varying
{
    Semantic semantic;
    uint8_t index;
};

struct Reflection
{
    Stage stage = Stage::Vertex;
    uint8_t major = 0, minor = 0;
    // Length of the uniform array c; 0 when the shader reads no constant
    // from the API.
    uint16_t floatConstants = 0;
    bool relativeConstants = false;
    SamplerType samplers[16] = {};
    std::vector<VertexInput> inputs; // vertex shaders
    // What a vertex shader writes, or what a pixel shader reads, sorted by
    // semantic then index. POSITION is not listed.
    std::vector<Varying> varyings;
    bool kills = false;       // texkill
    bool dropsDepth = false;  // writes oDepth, which GXM programs cannot
    uint32_t instructions = 0;
};

// D3DCMPFUNC numbers.
enum class AlphaFunc : uint8_t
{
    Never = 1,
    Less = 2,
    Equal = 3,
    LessEqual = 4,
    Greater = 5,
    NotEqual = 6,
    GreaterEqual = 7,
    Always = 8,
};

struct Options
{
    // Vertex shaders: inputs v<n> whose bytes are a D3DCOLOR (B, G, R, A in
    // memory). GXM reads them as R, G, B, A, so the program swaps them back.
    uint16_t bgraInputs = 0;
    // Vertex shaders: inputs v<n> no vertex element feeds. D3D9 reads them
    // as (0, 0, 0, 1); the program then has no in_v<n> parameter.
    uint16_t absentInputs = 0;
    // Vertex shaders: how many components the vertex element feeding v<n>
    // has (1 to 4; 0 means 4). The parameter is declared that wide and the
    // rest filled as D3D9 does: y and z 0, w 1.
    uint8_t inputComponents[16] = {};
    // Pixel shaders: D3DRS_ALPHATESTENABLE's test, done in the program
    // since GXM has none. Always is no test.
    AlphaFunc alphaFunc = AlphaFunc::Always;
};

// ---- Parsed form ---------------------------------------------------------------------------

struct Operand
{
    uint8_t type = 0;     // D3DSPR_*
    uint16_t number = 0;  //
    uint8_t mask = 0xf;   // destination write mask
    uint8_t swizzle = 0xe4; // source swizzle, 2 bits per component
    uint8_t modifier = 0; // source D3DSPSM_* or destination D3DSPDM_* bits
    uint8_t shift = 0;    // destination result shift (4 bits)
    bool relative = false;
    uint8_t relativeType = 0;   // the register that indexes
    uint16_t relativeNumber = 0;
    uint8_t relativeComponent = 0;
};

struct Instruction
{
    uint16_t opcode = 0;
    uint8_t control = 0; // bits 16-23: comparison or texld variant
    bool hasDestination = false;
    Operand destination;
    uint8_t sourceCount = 0;
    Operand sources[4];
    uint32_t declaration = 0; // dcl: the usage token
    uint32_t literal[4] = {}; // def, defi, defb
};

struct Shader
{
    Reflection reflection;
    std::vector<Instruction> instructions;
};

// False with the reason when the bytecode is malformed or uses something
// the translator does not handle.
bool Parse(const void *bytecode, size_t bytes, Shader &shader, std::string &error);

bool Emit(const Shader &shader, const Options &options, std::string &cg, std::string &error);

// A stable 64-bit hash (FNV-1a) for cache keys.
uint64_t Hash(const void *data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ull);
} // namespace gfx_gxm::cg
