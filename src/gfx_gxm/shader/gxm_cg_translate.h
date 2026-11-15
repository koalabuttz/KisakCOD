#pragma once

// Direct3D 9 shader bytecode to Cg, for the PS Vita's GXM back end.
//
// GXM runs programs that the Vita's own compiler (libshacccg, see
// gxm_shader_compiler.h) builds from Cg source, so each of the game's D3D9
// shaders (vs_1_1 to vs_3_0, ps_1_1 to ps_3_0) becomes a small Cg program:
// one float4 local per register, one statement per instruction, in the order
// the bytecode gives them. The shaders the game ships use no loops, calls or
// predicates; those, ps_1_4 and a few rare registers are refused with a
// reason. Volume textures, which GXM lacks, are sampled from their slices
// laid out in a 2D texture.
//
// ps_1_1 to ps_1_3 (the game's shader model 2 set draws its depth pass with
// them) are rewritten as ps_2_0 when parsed: "tex t0" becomes a read of
// sampler 0 at texture coordinate 0 into a temporary that stands for t0 from
// then on, the colour left in r0 is written to oC0. Their samplers are taken
// to be 2D, their registers are not clamped to -1..1, and the instructions
// that address one texture by another's value are refused.
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
//     Options::alphaFunc tests anything;
//   - texture coordinates a pixel shader works out the same way at every
//     pixel can be worked out by the vertex program (PlanHoist below).
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
    // Pixel shaders: the 2D samplers s<n> whose plain reads (texld) go
    // through a sharpening filter: the texel and its four neighbours, a
    // step of `kisak_sharp.xy` apart and no further than `kisak_sharp_max`,
    // as an unsharp mask of strength `kisak_sharp.z` kept between the
    // least and the most of the five. The Vita's scaled 3D picture is read
    // this way by the pass that stretches it (gxm_engine_options.h).
    uint16_t sharpSamplers = 0;
    // Pixel shaders: a pixel that fails the alpha test is written as
    // (0, 0, 0, 0) and not discarded. For draws whose blend leaves the
    // target as it was for such a source (gxm_idle_alpha_test.h): the
    // picture is the same and the program has no discard.
    bool alphaZero = false;
    // Vertex shaders: keep the position's z just inside the far plane
    // (z <= w * 0.99999). D3D9 draws z == w; a run-diagnostic option while
    // finding out whether GXM clips it.
    bool farPlaneInside = false;
    // Vertex shaders, run diagnostics: replace part of the position the
    // program computed, to find which part a console gets wrong.
    //   1: all of it, from the POSITION input read as 960x544 pixels
    //   2: x and y from dot(position, c[0]) and dot(position, c[1]); z 0.5, w 1
    //   3: z 0.5 and w 1; x and y as computed
    //   4: z half of the computed w
    // 5 to 8 draw at the POSITION input's pixels (as 1) and, when the
    // program writes TEXCOORD0, put numbers there for a fragment program
    // to show as a colour:
    //   5: the computed position: x / 2 + 0.5, y / 2 + 0.5, w / 2
    //   6: constants: c[1].w - 0.5, c[3].w / 2, c[7].w / 2
    //   7: constants: c[4].x / 2, c[5].y / 2, c[6].z / 2
    //   8: the computed position: z / 4 + 0.5, w / 4 + 0.5, 0.25
    uint8_t diagPosition = 0;
    // Both stages: the pixel shader's texture reads whose coordinates the
    // vertex program hands over (PlanHoist). Null for none. Only read
    // during Emit.
    const struct HoistPlan *hoist = nullptr;
    // Both stages, run diagnostics for the hoist (bits):
    //   1: the fragment program's colour is how far each hoisted
    //      coordinate is from the one the pixel shader works out
    //      (red and green, x 256; blue 0.25 marks a program with hoisted reads)
    //   2: the coordinates are handed over as float4 (in x and y)
    //   4: only the plan's first coordinate is handed over
    //   8: handed over in z and w of a float4, so the reads at them are
    //      not fetched ahead
    //  16: the colour is how far each fetched-ahead read's red, green and
    //      blue are from the same read made in the program (x 4)
    //  32: the same for alpha, as grey
    //  64: the fetched-ahead reads are written tex2D<float4>
    // 128: reads of sampler 2 are not hoisted; 256: nor those of sampler 3
    uint16_t diagHoist = 0;
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
    bool coissue = false; // ps 1.x: runs beside the instruction before it
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

// ---- Texture coordinates worked out by the vertex program ---------------------------------
//
// The Vita's GPU fetches a texture before the fragment program runs when the
// coordinate is the first two components of an interpolated input. A read at
// any other coordinate, another pair of components or one the program works
// out, is fetched while it runs, which costs far more (console run CN). Many
// of the game's lit shaders read their lightmaps at the last two components
// of texture coordinate 0, scaled and moved by literal constants. Such a
// coordinate is the same function of that input at every pixel, and a sum of
// its components times constants, so interpolating the result gives what
// working it out at each pixel gives. The vertex program can write it to a
// texture coordinate of its own, and the fragment program reads at that.
//
// PlanHoist finds those reads in a pixel shader: plain texld of a 2D sampler
// whose coordinate is made from one TEXCOORD input by mov, add, sub and mul
// or mad by literal constants. Each distinct coordinate takes a TEXCOORD
// the pixel shader does not read, from 9 down. CanHoist says whether a vertex
// shader can feed a plan: it writes each coordinate's input and none of the
// plan's own slots. Options::hoist then goes to both programs' Emit.
struct HoistCoord
{
    uint8_t root = 0; // made from TEXCOORD<root>
    uint8_t slot = 0; // handed over in TEXCOORD<slot>, as a float2
    // x = root.x * x[0] + root.y * x[1] + root.z * x[2] + root.w * x[3] + x[4]
    float x[5] = {}, y[5] = {};
};

struct HoistRead
{
    uint32_t instruction; // index in Shader::instructions
    uint8_t coord;        // index in HoistPlan::coords
};

struct HoistPlan
{
    std::vector<HoistCoord> coords;
    std::vector<HoistRead> reads; // sorted by instruction
    uint64_t hash = 0; // of the coordinates; never 0 for a plan with any
    bool Empty() const
    {
        return coords.empty();
    }
};

HoistPlan PlanHoist(const Shader &pixel);
bool CanHoist(const Shader &vertex, const HoistPlan &plan);
// The plan in words, for logs: each coordinate's sum and the samplers read
// at it, as "TEXCOORD9 = (tc0.z, tc0.w * 0.5 + 0.5) for s3".
std::string DescribeHoist(const Shader &pixel, const HoistPlan &plan);

// A stable 64-bit hash (FNV-1a) for cache keys.
uint64_t Hash(const void *data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ull);
} // namespace gfx_gxm::cg
