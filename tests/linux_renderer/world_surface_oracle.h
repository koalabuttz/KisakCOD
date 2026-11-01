// tests/linux_renderer/world_surface_oracle.h
//
// Independent CPU pixel oracle for ONE audited world-surface draw.
//
// What this is
// ------------
// A pure-CPU prediction of the RGBA a specific D3D9 device should produce for a
// specific, audited vertex/pixel shader pair, at sample positions chosen before the
// GPU is asked anything. It exists so the replay's readback can be compared against
// an answer derived only from the bundle bytes, the uploaded constant payloads and
// the decoded mip0 texels -- not against the readback itself.
//
// What this is NOT
// ----------------
// It is not a shader interpreter. Every instruction below was transcribed by hand
// from one disassembly (vs_3_0 146 words, ps_3_0 95 words), and the two entry points
// implement exactly that algebra with the token operation order preserved. A shader
// that is not that pair is rejected by CheckAuditedPrograms rather than approximated.
// There is no flow control, no predicates, no relative addressing and no loop to
// interpret: the audited programs contain none.
//
// Why the operation order is spelled out
// --------------------------------------
// The pixel stage is NOT the textbook lerp written in a nice order. Its tokens are
//    r1.xyz = v0.xyz * (-r0.xyz) + c1.xyz     // MAD, reads the sample BEFORE r0 is overwritten
//    r0     = r0 * v0                        // MUL_pp, all four channels
//    r0.xyz = c1.www * r1.xyz + r0.xyz       // MAD, preserves r0.w
//    oC0.w  = r0.w                           // MOV_pp
//    r0.xyz = r0.xyz - c0.xyz                // ADD
//    oC0.xyz= v1.zzz * r0.xyz + c0.xyz       // MAD
// which reduces to
//    A = T.rgb * C.rgb
//    B = A + M.a * (M.rgb - A)
//    out.rgb = F.rgb + k * (B - F.rgb)
//    out.a   = T.a * C.a
// The reduction is stated so a reader can check the intent, but EvalPixel executes
// the token order, because "equivalent mathematically" is not "equivalent in float".
// Note in particular that M.a scales RGB only: it never touches output alpha.
//
// What the sample positions are for
// ---------------------------------
// A comparison at one flat colour would pass with the wrong UVs. SelectSamples
// therefore preselects a stratified set of pixel centres inside the geometry's own
// screen-space bounding box, each at least kMinEdgeMarginPixels from every triangle
// edge and at least kMinTexelBoundaryMargin texels from any texel boundary, so a
// one-texel error in the interpolated UV cannot be mistaken for a match and a sample
// cannot land where the hardware's rounding could legally pick the neighbouring texel.
// The selection reads only CPU-side numbers, so it is declared before the GPU runs:
// nothing here is fitted to a readback.
//
// Deliberately asset-free and dependency-free: no D3D headers, no engine headers, no
// files, no globals. <cmath> and the standard containers are the whole dependency list.

#ifndef WORLD_SURFACE_ORACLE_H
#define WORLD_SURFACE_ORACLE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace world_surface_oracle {

// ---------------------------------------------------------------------------
// The audited program pair.
//
// These are the FNV-1a 64 hashes of the byte payloads the bundle ships (the same
// hash world_surface_bundle.h records as vertex/pixel_shader_hash_actual), plus the
// exact word counts. A hash alone pins bytes; the structural walk in
// CheckAuditedPrograms pins the program, so an unrelated shader cannot be accepted
// by editing a recorded hash.
// ---------------------------------------------------------------------------
constexpr std::uint64_t kAuditedVertexShaderFnv1a64 = UINT64_C(0x5068db98e3f4c40e);
constexpr std::uint64_t kAuditedPixelShaderFnv1a64 = UINT64_C(0x1e7bdf7881fe189f);
constexpr std::uint32_t kAuditedVertexShaderWords = 146; // END at 145
constexpr std::uint32_t kAuditedPixelShaderWords = 95;   // END at 94

// VS `def c8, 1.0, 0.0, 1.4426950216293335, 0.0`. c8.z is log2(e), the scale the
// shader applies to its natural-exponent fog argument before exp_sat.
constexpr float kLog2E = 1.4426950216293335f; // bits 0x3FB8AA3B
constexpr std::uint32_t kLog2EBits = 0x3FB8AA3Bu;

// D3D9 shader opcodes used by the audited pair (D3DSIO_*).
enum : std::uint16_t
{
    kOpMov = 1,
    kOpAdd = 2,
    kOpMad = 4,
    kOpMul = 5,
    kOpRcp = 6,
    kOpRsq = 7,
    kOpDp3 = 8,
    kOpDp4 = 9,
    kOpExp = 14,
    kOpDcl = 31,
    kOpTex = 66,
    kOpDef = 81,
    kOpComment = 0xFFFE,
    kOpEnd = 0xFFFF,
};

// The audited instruction sequences, in executable order. DEF and the DCL list come
// first because that is the order the assembler emitted them.
inline const std::uint16_t* AuditedVertexOpcodes(std::size_t* count)
{
    static const std::uint16_t opcodes[] = {
        kOpDef,
        kOpDcl, kOpDcl, kOpDcl, kOpDcl, kOpDcl, kOpDcl,
        kOpMad,
        kOpDp4, kOpDp4, kOpDp4, kOpDp4,
        kOpDp4, kOpDp4, kOpDp4, kOpDp4,
        kOpDp3,
        kOpRsq,
        kOpRcp,
        kOpMad,
        kOpMul,
        kOpExp,
        kOpMov, kOpMov,
        kOpEnd,
    };
    *count = sizeof(opcodes) / sizeof(opcodes[0]);
    return opcodes;
}

inline const std::uint16_t* AuditedPixelOpcodes(std::size_t* count)
{
    static const std::uint16_t opcodes[] = {
        kOpDcl, kOpDcl, kOpDcl,
        kOpTex,
        kOpMad,
        kOpMul,
        kOpMad,
        kOpMov,
        kOpAdd,
        kOpMad,
        kOpEnd,
    };
    *count = sizeof(opcodes) / sizeof(opcodes[0]);
    return opcodes;
}

// ---------------------------------------------------------------------------
// FNV-1a 64, the same basis and prime world_surface_bundle.h uses for its recorded
// payload hashes. Defined here so this header can be exercised with no bundle in the
// translation unit; the replay static_asserts that the two implementations agree.
// ---------------------------------------------------------------------------
inline std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<std::uint64_t>(data[index]);
        hash *= UINT64_C(0x100000001B3);
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Structural walk. Returns the instruction stream as (offset, opcode) pairs and
// whether any instruction carried a predicate or co-issue bit, which the audited
// programs do not use and this reference does not model.
// ---------------------------------------------------------------------------
struct Instruction
{
    std::uint32_t offset = 0;
    std::uint16_t opcode = 0;
    std::uint32_t token = 0;
};

struct Walk
{
    bool ok = false;
    std::string reason;
    std::vector<Instruction> instructions;
    bool sawControlBits = false;
    bool ended = false;
    std::uint32_t endOffset = 0;
};

inline Walk WalkProgram(const std::uint32_t* words, std::uint32_t wordCount, std::uint32_t start)
{
    Walk walk;
    if (wordCount < 2)
    {
        walk.reason = "program is shorter than one instruction token";
        return walk;
    }

    std::uint32_t cursor = start;
    while (cursor < wordCount)
    {
        const std::uint32_t token = words[cursor];
        const std::uint16_t opcode = static_cast<std::uint16_t>(token & 0xFFFFu);

        if (opcode == kOpComment)
        {
            // A comment token puts its DWORD count in bits 16-30, not in the
            // instruction-length field (the length field is zero for it).
            const std::uint32_t commentWords = (token >> 16) & 0x7FFFu;
            cursor += 1u + commentWords;
            continue;
        }

        Instruction instruction;
        instruction.offset = cursor;
        instruction.opcode = opcode;
        instruction.token = token;
        walk.instructions.push_back(instruction);

        // D3DSI_PREDICATED is bit 28 and D3DSI_COISSUE bit 30; the instruction length
        // occupies bits 24-27, so this mask touches neither.
        if ((token & 0x50000000u) != 0u)
            walk.sawControlBits = true;

        if (opcode == kOpEnd)
        {
            walk.ended = true;
            walk.endOffset = cursor;
            cursor += 1u;
            break;
        }

        // A zero length field on a non-END, non-comment token would loop forever.
        //
        // The length field counts OPERAND tokens, not words: the instruction occupies one
        // opcode word plus that many operands. `def c8, 1,0,log2e,0` declares 5 (the
        // destination plus four literals) and spans six words, and `dcl_position0 v0`
        // declares 2 and spans three. Advancing by the raw field would land inside the next
        // instruction's operands, which is exactly how this was found.
        const std::uint32_t operandCount = (token >> 24) & 0xFu;
        if (operandCount == 0u)
        {
            walk.reason = "instruction at word " + std::to_string(cursor)
                + " declares zero operands";
            return walk;
        }
        cursor += 1u + operandCount;
    }

    if (!walk.ended)
    {
        walk.reason = "program has no END token";
        return walk;
    }
    if (cursor != wordCount)
    {
        walk.reason = "program has " + std::to_string(wordCount - cursor)
            + " word(s) after END";
        return walk;
    }
    walk.ok = true;
    return walk;
}

// ---------------------------------------------------------------------------
// Program identity check. Three independent gates, all of which must hold:
//   1. the byte payload hashes to the audited FNV-1a 64 and has the audited length;
//   2. the instruction opcode sequence is exactly the audited one;
//   3. the immediate payloads the reference algebra depends on are the audited ones
//      (the c8 DEF constants and the saturating fog write), and no instruction is
//      predicated or co-issued.
// Gate 3 is what makes the oracle's assumptions checkable rather than asserted: if
// the fog write were not saturating, or c8.z were not log2(e), the algebra below
// would be a guess.
// ---------------------------------------------------------------------------
struct ProgramCheck
{
    bool ok = false;
    std::string reason;
    std::uint64_t vertexFnv = 0;
    std::uint64_t pixelFnv = 0;
    std::uint32_t vertexWords = 0;
    std::uint32_t pixelWords = 0;
    std::uint32_t vertexInstructions = 0;
    std::uint32_t pixelInstructions = 0;
};

inline bool CheckOpcodeSequence(const Walk& walk, const std::uint16_t* expected,
    std::size_t expectedCount, const char* which, std::string* reason)
{
    if (walk.instructions.size() != expectedCount)
    {
        *reason = std::string(which) + " has " + std::to_string(walk.instructions.size())
            + " instruction(s), the audited program has " + std::to_string(expectedCount);
        return false;
    }
    for (std::size_t index = 0; index < expectedCount; ++index)
    {
        if (walk.instructions[index].opcode != expected[index])
        {
            *reason = std::string(which) + " instruction " + std::to_string(index)
                + " at word " + std::to_string(walk.instructions[index].offset) + " is opcode "
                + std::to_string(walk.instructions[index].opcode) + ", the audited program has "
                + std::to_string(expected[index]);
            return false;
        }
    }
    return true;
}

inline bool FindInstruction(const Walk& walk, std::uint16_t opcode, const Instruction** found)
{
    for (const Instruction& instruction : walk.instructions)
    {
        if (instruction.opcode == opcode)
        {
            *found = &instruction;
            return true;
        }
    }
    return false;
}

inline ProgramCheck CheckAuditedPrograms(const std::vector<std::uint8_t>& vertexBytes,
    const std::vector<std::uint8_t>& pixelBytes)
{
    ProgramCheck check;

    const std::size_t vertexExpectedBytes = static_cast<std::size_t>(kAuditedVertexShaderWords) * 4u;
    const std::size_t pixelExpectedBytes = static_cast<std::size_t>(kAuditedPixelShaderWords) * 4u;
    if (vertexBytes.size() != vertexExpectedBytes || pixelBytes.size() != pixelExpectedBytes)
    {
        check.reason = "shader payload sizes are " + std::to_string(vertexBytes.size()) + " and "
            + std::to_string(pixelBytes.size()) + " bytes; the audited pair is "
            + std::to_string(vertexExpectedBytes) + " and " + std::to_string(pixelExpectedBytes);
        return check;
    }

    check.vertexFnv = Fnv1a64(vertexBytes.data(), vertexBytes.size());
    check.pixelFnv = Fnv1a64(pixelBytes.data(), pixelBytes.size());
    if (check.vertexFnv != kAuditedVertexShaderFnv1a64)
    {
        check.reason = "vertex shader hashes to 0x" + std::to_string(check.vertexFnv)
            + ", the audited program hashes to 0x5068db98e3f4c40e";
        return check;
    }
    if (check.pixelFnv != kAuditedPixelShaderFnv1a64)
    {
        check.reason = "pixel shader hashes to 0x" + std::to_string(check.pixelFnv)
            + ", the audited program hashes to 0x1e7bdf7881fe189f";
        return check;
    }

    // The payloads are 4-byte aligned by construction (word counts, checked above), and
    // std::vector<uint8_t> is byte-addressed, so copy into word buffers rather than
    // aliasing through a reinterpret_cast.
    std::vector<std::uint32_t> vertexWords(kAuditedVertexShaderWords);
    std::vector<std::uint32_t> pixelWords(kAuditedPixelShaderWords);
    std::memcpy(vertexWords.data(), vertexBytes.data(), vertexExpectedBytes);
    std::memcpy(pixelWords.data(), pixelBytes.data(), pixelExpectedBytes);
    check.vertexWords = static_cast<std::uint32_t>(vertexWords.size());
    check.pixelWords = static_cast<std::uint32_t>(pixelWords.size());

    // vs_3_0 is 0xFFFE0300; ps_3_0 is 0xFFFF0300.
    if (vertexWords[0] != 0xFFFE0300u)
    {
        check.reason = "vertex shader version token is not vs_3_0";
        return check;
    }
    if (pixelWords[0] != 0xFFFF0300u)
    {
        check.reason = "pixel shader version token is not ps_3_0";
        return check;
    }

    // Both payloads open with one comment token carrying their whole CTAB, then the
    // first executable instruction. The counts are part of the audited framing.
    const std::uint32_t vertexCommentWords = (vertexWords[1] >> 16) & 0x7FFFu;
    if ((vertexWords[1] & 0xFFFFu) != kOpComment || vertexCommentWords != 54u)
    {
        check.reason = "vertex shader does not open with the audited 54-word comment";
        return check;
    }
    const std::uint32_t pixelCommentWords = (pixelWords[1] >> 16) & 0x7FFFu;
    if ((pixelWords[1] & 0xFFFFu) != kOpComment || pixelCommentWords != 53u)
    {
        check.reason = "pixel shader does not open with the audited 53-word comment";
        return check;
    }

    const std::uint32_t vertexStart = 2u + vertexCommentWords;
    const std::uint32_t pixelStart = 2u + pixelCommentWords;

    const Walk vertexWalk = WalkProgram(vertexWords.data(), check.vertexWords, vertexStart);
    if (!vertexWalk.ok)
    {
        check.reason = "vertex shader framing: " + vertexWalk.reason;
        return check;
    }
    const Walk pixelWalk = WalkProgram(pixelWords.data(), check.pixelWords, pixelStart);
    if (!pixelWalk.ok)
    {
        check.reason = "pixel shader framing: " + pixelWalk.reason;
        return check;
    }

    check.vertexInstructions = static_cast<std::uint32_t>(vertexWalk.instructions.size());
    check.pixelInstructions = static_cast<std::uint32_t>(pixelWalk.instructions.size());

    if (vertexWalk.sawControlBits || pixelWalk.sawControlBits)
    {
        check.reason = "a shader instruction is predicated or co-issued; this reference models "
                       "neither";
        return check;
    }

    std::size_t expectedCount = 0;
    const std::uint16_t* expected = AuditedVertexOpcodes(&expectedCount);
    if (!CheckOpcodeSequence(vertexWalk, expected, expectedCount, "vertex shader", &check.reason))
        return check;
    expected = AuditedPixelOpcodes(&expectedCount);
    if (!CheckOpcodeSequence(pixelWalk, expected, expectedCount, "pixel shader", &check.reason))
        return check;

    // `def c8, 1.0, 0.0, log2(e), 0.0` supplies the homogeneous w and the exponent scale.
    const Instruction* definition = nullptr;
    if (!FindInstruction(vertexWalk, kOpDef, &definition))
    {
        check.reason = "vertex shader has no def";
        return check;
    }
    const std::uint32_t* immediate = vertexWords.data() + definition->offset + 2u;
    if (immediate[0] != 0x3F800000u || immediate[1] != 0x00000000u
        || immediate[2] != kLog2EBits || immediate[3] != 0x00000000u)
    {
        check.reason = "vertex shader def c8 is not (1, 0, 1.4426950216293335, 0)";
        return check;
    }
    if (((vertexWords[definition->offset + 1] >> 0) & 0x7FFu) != 8u
        || ((vertexWords[definition->offset + 1] >> 28) & 0x7u) != 2u)
    {
        check.reason = "vertex shader def does not target constant register c8";
        return check;
    }

    // The fog write is `exp_sat o2.z`, whose destination token carries both the z mask
    // and the SATURATE modifier. Without the saturation the reference's clamp would be
    // wrong, so it is checked rather than assumed.
    const Instruction* exponent = nullptr;
    if (!FindInstruction(vertexWalk, kOpExp, &exponent))
    {
        check.reason = "vertex shader has no exp";
        return check;
    }
    const std::uint32_t fogDestination = vertexWords[exponent->offset + 1u];
    if (fogDestination != 0xE0140002u)
    {
        check.reason = "vertex shader fog write is not `exp_sat o2.z`";
        return check;
    }

    // texld_pp r0, v1, s0 must be a plain 2D sample: no projection and no bias bits,
    // because Sample2DPointWrapMip0 divides by nothing and reads uv.xy only.
    const Instruction* texture = nullptr;
    if (!FindInstruction(pixelWalk, kOpTex, &texture))
    {
        check.reason = "pixel shader has no texld";
        return check;
    }
    if ((texture->token & 0x00030000u) != 0u)
    {
        check.reason = "pixel shader texld sets the projection or bias control bit";
        return check;
    }

    // The third DCL is `dcl_2d s0`; the sampler must be 2D. The texture type lives in the
    // declaration token, which is the instruction's first operand, not in the instruction
    // token itself.
    const Instruction& samplerDeclaration = pixelWalk.instructions[2];
    const std::uint32_t samplerToken = pixelWords[samplerDeclaration.offset + 1u];
    if (((samplerToken >> 27) & 0xFu) != 2u)
    {
        check.reason = "pixel shader sampler is not a 2D sampler";
        return check;
    }

    check.ok = true;
    return check;
}

// ---------------------------------------------------------------------------
// Vertex stage: q, r1, clip, and the fog scalar.
//
// The uploaded payloads are used exactly as the API would hand them over: c0..c3 are
// the transposed view-projection rows, c4..c7 the transposed world rows, c21 the fog
// vector. No extra transpose happens here, because R_GetCodeMatrix already applied it.
// ---------------------------------------------------------------------------
struct VertexConstants
{
    float clipRows[4][4];  // VS c0..c3
    float worldRows[4][4]; // VS c4..c7
    float fog[4];          // VS c21
};

struct VertexInput
{
    float position[4]; // POSITION0, FLOAT4 as uploaded (w is replaced below)
    float texcoord[4]; // TEXCOORD0, FLOAT4 as uploaded (only xy are read)
    std::uint32_t color; // COLOR0, D3DCOLOR as uploaded (BGRA in memory)
};

struct VertexOutput
{
    float clip[4];
    float color[4];
    float uv[2];
    float fog;
};

inline float Dot4(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

inline float Dot3(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline bool IsFinite(float value)
{
    // A comparison, not std::isfinite, so a fast-math flag cannot fold the check away.
    return value == value && value - value == 0.0f;
}

inline float Saturate(float value)
{
    if (!(value > 0.0f))
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

// D3DCOLOR is 0xAARRGGBB in the DWORD and BGRA in memory; the shader receives it
// normalised by 255, not by 256, so white is exactly 1.0 in every channel.
inline void DecodeD3dColor(std::uint32_t color, float out[4])
{
    out[0] = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>(color & 0xFFu) / 255.0f;
    out[3] = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
}

// rsq then rcp, kept as two operations.
//
// The pair is algebraically the square root, and writing it as sqrt() would be one
// rounding where the shader makes two. That difference is far below the one-level
// tolerance, but the point of this file is that the transcription can be checked
// operation by operation, so it is left as two.
inline float RsqThenRcp(float squaredLength)
{
    const float rsq = 1.0f / std::sqrt(squaredLength);
    return 1.0f / rsq;
}

inline bool EvalVertex(const VertexInput& input, const VertexConstants& constants,
    VertexOutput* output)
{
    // mad r0, v0.xyzx, c8.xxxy, c8.yyyx  ==  (x, y, z, 1): the input w is deliberately
    // discarded, and the vertex's own w happens to be 1 but is not what reaches r1.
    const float q[4] = {input.position[0], input.position[1], input.position[2], 1.0f};

    float r1[4];
    for (int row = 0; row < 4; ++row)
        r1[row] = Dot4(q, constants.worldRows[row]);

    for (int row = 0; row < 4; ++row)
        output->clip[row] = Dot4(r1, constants.clipRows[row]);

    const float squaredLength = Dot3(r1, r1);
    if (!IsFinite(squaredLength) || !(squaredLength > 0.0f))
    {
        // rsq of zero is infinite and the fog scalar stops being a number; the audited
        // fixture has a finite nonzero eye-relative distance, so this is a refusal.
        return false;
    }

    float fogArgument = RsqThenRcp(squaredLength);
    fogArgument = fogArgument * constants.fog[2] + constants.fog[3]; // mad r0.x, r0.x, c21.z, c21.w
    fogArgument = fogArgument * kLog2E;                              // mul r0.x, r0.x, c8.z
    output->fog = Saturate(std::exp2(fogArgument));                  // exp_sat o2.z

    DecodeD3dColor(input.color, output->color); // mov o1, v1
    output->uv[0] = input.texcoord[0];          // mov o2.xy, v2
    output->uv[1] = input.texcoord[1];

    for (int channel = 0; channel < 4; ++channel)
        if (!IsFinite(output->clip[channel]))
            return false;
    for (int channel = 0; channel < 4; ++channel)
        if (!IsFinite(output->color[channel]))
            return false;
    return IsFinite(output->uv[0]) && IsFinite(output->uv[1]) && IsFinite(output->fog);
}

// ---------------------------------------------------------------------------
// Mip 0 of the prepared texture, as uploaded: RGBA bytes, row-major, first row is
// v in [0, 1/height). No V flip happens in the shader or in the upload path.
// ---------------------------------------------------------------------------
struct Mip0View
{
    const std::uint8_t* rgba = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool Valid() const { return rgba != nullptr && width > 0 && height > 0; }
};

// floor(v * size) wrapped into [0, size): nonnegative modulo, so a negative uv wraps
// to the top of the texture instead of being clamped to texel 0.
inline std::uint32_t WrapTexelIndex(float coordinate, std::uint32_t size)
{
    const double floored = std::floor(static_cast<double>(coordinate));
    const double wrapped = std::fmod(floored, static_cast<double>(size));
    const double positive = wrapped < 0.0 ? wrapped + static_cast<double>(size) : wrapped;
    return static_cast<std::uint32_t>(positive);
}

// Point sampling at mip 0 with wrap addressing: the texel containing the coordinate,
// with no half-texel offset (that belongs to linear filtering) and no mip selection
// (the pass samples the top level because its uv derivatives select no mip with the
// filter entry this replay decodes).
inline void Sample2DPointWrapMip0(const Mip0View& mip, float u, float v, float out[4])
{
    const std::uint32_t texelX = WrapTexelIndex(u * static_cast<float>(mip.width), mip.width);
    const std::uint32_t texelY = WrapTexelIndex(v * static_cast<float>(mip.height), mip.height);
    const std::size_t offset = (static_cast<std::size_t>(texelY) * mip.width + texelX) * 4u;
    for (int channel = 0; channel < 4; ++channel)
        out[channel] = static_cast<float>(mip.rgba[offset + static_cast<std::size_t>(channel)])
            / 255.0f;
}

// ---------------------------------------------------------------------------
// Pixel stage.
// ---------------------------------------------------------------------------
struct PixelConstants
{
    float fogColor[4]; // PS c0
    float material[4]; // PS c1
};

// Perspective-correct weights at a pixel centre.
//
// Screen-space barycentrics are affine in screen position; the varyings are not, so
// each is divided by its clip w and the three results renormalised. With equal w this
// collapses to the barycentrics, which is exactly why the test uses unequal w: a
// linear interpolation would agree there and be wrong everywhere else.
inline void PerspectiveWeights(const float barycentric[3], const float clipW[3], float out[3])
{
    float inverse[3];
    float numerator[3];
    for (int corner = 0; corner < 3; ++corner)
    {
        inverse[corner] = 1.0f / clipW[corner];
        numerator[corner] = barycentric[corner] * inverse[corner];
    }
    const float sum = numerator[0] + numerator[1] + numerator[2];
    if (!(sum != 0.0f) || !IsFinite(sum))
    {
        for (int corner = 0; corner < 3; ++corner)
            out[corner] = barycentric[corner];
        return;
    }
    for (int corner = 0; corner < 3; ++corner)
        out[corner] = numerator[corner] / sum;
}

// Perspective interpolation of the three varyings the pixel stage reads. Split out from
// EvalPixel so a caller can move one varying deliberately -- the replay's wrong-UV control
// shifts u by a known number of texels and needs the same color and fog the baseline used.
inline void InterpolateVaryings(const float weights[3], const VertexOutput outputs[3],
    float color[4], float uv[2], float* fog)
{
    for (int channel = 0; channel < 4; ++channel)
        color[channel] = 0.0f;
    uv[0] = 0.0f;
    uv[1] = 0.0f;
    *fog = 0.0f;
    for (int corner = 0; corner < 3; ++corner)
    {
        for (int channel = 0; channel < 4; ++channel)
            color[channel] += weights[corner] * outputs[corner].color[channel];
        uv[0] += weights[corner] * outputs[corner].uv[0];
        uv[1] += weights[corner] * outputs[corner].uv[1];
        *fog += weights[corner] * outputs[corner].fog;
    }
}

// The pixel program itself, at already-interpolated varyings. Executes the audited token
// order; see the file header for why the order is not rearranged into a lerp.
inline bool EvalPixelAt(const float uv[2], float fog, const float color[4], const Mip0View& mip,
    const PixelConstants& constants, float out[4])
{
    float texel[4];
    Sample2DPointWrapMip0(mip, uv[0], uv[1], texel);

    const float* fogColor = constants.fogColor;
    const float* material = constants.material;

    // mad r1.xyz, v0.xyzw, -r0.xyzw, c1.xyzw   (read before r0 is overwritten)
    float intermediate[3];
    for (int channel = 0; channel < 3; ++channel)
        intermediate[channel] = color[channel] * (-texel[channel]) + material[channel];

    // mul_pp r0.xyzw, r0.xyzw, v0.xyzw          (all four channels)
    float register0[4];
    for (int channel = 0; channel < 4; ++channel)
        register0[channel] = texel[channel] * color[channel];

    // mad r0.xyz, c1.wwww, r1.xyz, r0.xyz       (r0.w survives)
    for (int channel = 0; channel < 3; ++channel)
        register0[channel] = material[3] * intermediate[channel] + register0[channel];

    // mov_pp oC0.w, r0.w
    out[3] = register0[3];

    // add r0.xyz, r0.xyzw, -c0.xyzw
    for (int channel = 0; channel < 3; ++channel)
        register0[channel] = register0[channel] - fogColor[channel];

    // mad oC0.xyz, v1.zzzz, r0.xyzw, c0.xyzw
    for (int channel = 0; channel < 3; ++channel)
        out[channel] = fog * register0[channel] + fogColor[channel];

    for (int channel = 0; channel < 4; ++channel)
        if (!IsFinite(out[channel]))
            return false;
    return true;
}

// Convenience: interpolate at the given weights and run the program.
inline bool EvalPixel(const float weights[3], const VertexOutput outputs[3], const Mip0View& mip,
    const PixelConstants& constants, float out[4])
{
    float color[4];
    float uv[2];
    float fog = 0.0f;
    InterpolateVaryings(weights, outputs, color, uv, &fog);
    return EvalPixelAt(uv, fog, color, mip, constants, out);
}

// The shader's output is unclamped; the A8R8G8B8 target clamps and rounds to nearest.
inline std::uint8_t QuantizeUnorm8(float value)
{
    if (!(value > 0.0f))
        return 0;
    if (value >= 1.0f)
        return 255;
    return static_cast<std::uint8_t>(std::floor(value * 255.0f + 0.5f));
}

// ---------------------------------------------------------------------------
// Sample selection.
//
// The grid is laid over the geometry's own screen-space bounding box, not over the
// whole viewport: a small triangle in a 256x256 target would otherwise share one or
// two viewport cells and the "spread out" requirement would be unsatisfiable by
// construction. Cells are therefore geometry-relative, and the quotas are stated
// before any pixel is predicted.
//
// Geometry is bounded: at most kMaxTriangles triangles are accepted and a larger
// array is refused rather than truncated, because silently dropping a triangle would
// turn "the oracle checked the surface" into "the oracle checked part of it".
// ---------------------------------------------------------------------------
constexpr std::uint32_t kMaxTriangles = 256;

// D3DRS_CULLMODE, expressed as which winding survives rather than as the API constant.
// The names D3DCULL_CCW / D3DCULL_CW read backwards for what they do -- D3DCULL_CCW
// keeps the clockwise triangles -- so this header states the kept winding and the
// caller translates. On this device the pixel grid's y axis points down, and the
// twice-signed area computed below is POSITIVE for a clockwise-on-screen winding,
// which is D3D's default front face.
enum class CullMode : std::uint32_t
{
    None = 0,                // D3DCULL_NONE: both windings are drawn
    ClockwiseOnly = 1,       // D3DCULL_CCW: keeps D3D's front faces (clockwise on screen)
    CounterClockwiseOnly = 2 // D3DCULL_CW: keeps the back faces
};

inline const char* CullModeName(CullMode mode)
{
    switch (mode)
    {
    case CullMode::None: return "none";
    case CullMode::ClockwiseOnly: return "clockwise-only (D3DCULL_CCW)";
    case CullMode::CounterClockwiseOnly: return "counter-clockwise-only (D3DCULL_CW)";
    }
    return "unknown";
}

// One triangle as the vertex stage produced it. `index` and `group` are labels the
// caller supplies: the index values let the report name the triangle with the numbers
// the bundle recorded, and the group lets the caller require that each coplanar patch
// of a multi-plane surface is sampled rather than accepting samples from one plane
// only. Neither affects the arithmetic.
struct TriangleInput
{
    VertexOutput output[3];
    std::uint32_t index[3] = {0, 0, 0};
    std::uint32_t group = 0;
};

// Per-group census, so a surface whose planes differ in size cannot hide a plane that
// was never sampled behind a good total.
struct GroupStats
{
    std::uint32_t group = 0;
    std::uint32_t trianglesKept = 0;
    std::uint32_t coveredPixels = 0;  // pixels whose nearest visible triangle is in this group
    std::uint32_t eligiblePixels = 0; // ...and which passed every margin
    std::uint32_t samples = 0;

    bool PixelsAvailable() const { return coveredPixels > 0; }
};

struct OracleConfig
{
    std::uint32_t viewportWidth = 256;
    std::uint32_t viewportHeight = 256;

    // Strict interior: at least this many pixels from every triangle edge.
    float minEdgeMarginPixels = 2.0f;

    // At least this many texels from any texel boundary, so the hardware cannot legally
    // land on the neighbouring texel and a genuine UV error still shows up.
    float minTexelBoundaryMargin = 0.25f;

    // Spatial stratification: cellCount x cellCount cells over the geometry bounding box.
    std::uint32_t cellCount = 4;
    std::uint32_t minSamples = 8;
    std::uint32_t minOccupiedCells = 4;
    std::uint32_t maxSamples = 16;

    // --- multi-triangle extensions -----------------------------------------
    // A one-triangle geometry leaves every field below at its neutral value, so the
    // single-triangle selection is the same computation over one element.
    //
    // The decoded depth of the surface grid is not replicated here: no triangle is
    // assumed coplanar with any other, and no per-group plane equation is fitted.
    // What the device does -- nearest interpolated z/w wins -- is what is computed.

    // Which winding the device keeps, from the recorded state block.
    CullMode cull = CullMode::None;

    // Per-group quotas. A group with pixels available has to supply at least
    // minSamplesPerGroup samples of its own; 0 disables the requirement, which is what
    // the single-triangle caller wants. maxSamplesPerGroup == 0 means maxSamples.
    std::uint32_t minSamplesPerGroup = 0;
    std::uint32_t maxSamplesPerGroup = 0;

    // A pixel counts as order-sensitive -- and only then is it evidence that the depth
    // test is doing work -- when the first-drawn and last-drawn triangles covering it
    // differ AND both put the sample point this far inside their own edges. At a
    // hairline overlap the device's fill rule rather than the depth test decides the
    // pixel, and a prediction there would be a coincidence.
    float overlapEdgeMarginPixels = 1.0f;

    // A sample whose nearest triangle is not separated from the next nearest by at
    // least this much NDC z is refused: its colour is chosen by draw order, not by
    // depth, so it cannot support a depth-order claim. 0 disables the filter, and a
    // pixel covered by one triangle has an infinite gap by definition.
    float minDepthGap = 0.0f;

    // Where the fragment is evaluated, relative to the integer pixel coordinate.
    //
    // Zero, not one half. The transform this file implements is ClipToScreen's: NDC x in
    // [-1, 1] maps to [0, width], so the window coordinate of pixel x is x and pixel
    // (0, 0) sits at NDC (-1, +1). A half-pixel offset here would move every prediction
    // of a frame, so the value is a fixed contract of this oracle, stated once and never
    // chosen per frame or per fixture.
    //
    // Provenance of the choice, stated as what it is: the convention was established in
    // an earlier session against this DXVK-native D3D9 device, by comparing predicted
    // coverage with a rendered target over all 65536 pixels and by grouping the rendered
    // pixels by the texel this offset selects. That run's output is not retained in the
    // repository and no check in the current tree reproduces it, so this is a record of
    // a historical measurement, not a live guarantee. What the current tree does check is
    // the convention itself and the transform around it: the asset-free test asserts that
    // fragmentSampleOffset is zero and that ClipToScreen maps the NDC endpoints to 0 and
    // width (height and 0 in y). Re-establishing the device convention end to end would
    // mean a separate known-geometry run that declares both hypotheses before it draws.
    float fragmentSampleOffset = 0.0f;
};

struct Sample
{
    std::uint32_t pixelX = 0;
    std::uint32_t pixelY = 0;
    std::uint32_t cellRow = 0;
    std::uint32_t cellColumn = 0;
    std::uint32_t cellIndex = 0;
    float weights[3] = {0.0f, 0.0f, 0.0f};
    float barycentric[3] = {0.0f, 0.0f, 0.0f};
    float uv[2] = {0.0f, 0.0f};
    float texelCoordinate[2] = {0.0f, 0.0f}; // continuous, before wrap/floor
    std::uint32_t texelX = 0;
    std::uint32_t texelY = 0;
    float edgeMarginPixels = 0.0f;
    float texelBoundaryMargin = 0.0f;
    std::uint8_t expected[4] = {0, 0, 0, 0};

    // Which triangle the prediction came from, and why. `coveringTriangles` is how many
    // kept triangles cover this pixel, so a value above one is the record that this
    // sample sits in an overlap and was resolved by depth rather than by draw order.
    std::uint32_t triangle = 0;
    std::uint32_t group = 0;
    std::uint32_t coveringTriangles = 1;
    float ndcDepth = 0.0f; // interpolated clip.z / clip.w at the sample point
    float depthGap = 3.4e38f; // next nearest ndc depth minus this one; +inf when unique
};

enum class SelectStatus
{
    Ok = 0,
    NoTexture,
    DegenerateTriangle,
    BehindEye,
    NonFinite,
    NoCoverage,
    TooFewSamples,
    TooFewCells,
    NoTriangles,
    TooManyTriangles,
    GroupUnderSampled,
    // A triangle has corners on both sides of the eye plane. The device clips such a triangle
    // against the near plane; this oracle does not model clipping, so it refuses the whole
    // declaration instead of dropping the triangle and predicting a surface that is not the one
    // the device draws.
    ClippingUnsupported,
};

inline const char* SelectStatusName(SelectStatus status)
{
    switch (status)
    {
    case SelectStatus::Ok: return "Ok";
    case SelectStatus::NoTexture: return "NoTexture";
    case SelectStatus::DegenerateTriangle: return "DegenerateTriangle";
    case SelectStatus::BehindEye: return "BehindEye";
    case SelectStatus::NonFinite: return "NonFinite";
    case SelectStatus::NoCoverage: return "NoCoverage";
    case SelectStatus::TooFewSamples: return "TooFewSamples";
    case SelectStatus::TooFewCells: return "TooFewCells";
    case SelectStatus::NoTriangles: return "NoTriangles";
    case SelectStatus::TooManyTriangles: return "TooManyTriangles";
    case SelectStatus::GroupUnderSampled: return "GroupUnderSampled";
    case SelectStatus::ClippingUnsupported: return "ClippingUnsupported";
    }
    return "Unknown";
}

struct Selection
{
    SelectStatus status = SelectStatus::NoCoverage;
    std::string message;
    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    std::uint32_t cellCount = 0;
    // Geometry bounding box in pixels, inclusive: minX, minY, maxX, maxY.
    std::int32_t boundingBox[4] = {0, 0, 0, 0};
    std::uint32_t occupiedCells = 0;
    std::uint32_t consideredPixels = 0;
    std::uint32_t eligiblePixels = 0;
    std::vector<Sample> samples;

    // --- multi-triangle census ---------------------------------------------
    // Counted, not assumed: a triangle that leaves the frustum, folds to a line on screen
    // or is removed by the cull mode is reported under its own name rather than being
    // folded into "no coverage".
    std::uint32_t triangleCount = 0;
    std::uint32_t triangleLimit = kMaxTriangles;
    std::uint32_t trianglesKept = 0;
    std::uint32_t trianglesCulled = 0;
    // Wholly behind the eye: every corner has clip w <= 0. Such a triangle is culled by the
    // device too, so dropping it is correct -- but it is counted here by name rather than being
    // folded into "no coverage".
    std::uint32_t trianglesBehindEye = 0;
    std::uint32_t trianglesDegenerate = 0;
    std::uint32_t trianglesNonFinite = 0;
    // Straddling the eye plane: some corner has clip w <= 0 and some has clip w > 0. The device
    // clips these; this oracle does not, so a non-zero count here means the selection was
    // refused with ClippingUnsupported and every other field is a census, not a prediction.
    std::uint32_t trianglesStraddlingEye = 0;

    // Pixels covered by two or more kept triangles, and among those the ones whose
    // colour depends on draw order when the depth test is off: the first-drawn and the
    // last-drawn covering triangle differ, and both cover the point with margin. That
    // set is the a-priori reason a reversed-order, depth-disabled frame MUST differ; if
    // it is empty the replay must not claim a depth-ordering result.
    //
    // It is necessary but NOT sufficient, and the two counts below are why: a pixel whose
    // nearest triangle is also the first-drawn one, or whose two triangles predict the same
    // colour, has a different owner without a different image. Any claim about an image
    // difference has to be made from orderContrastPixels, not from this count.
    std::uint32_t multiCoveredPixels = 0;
    std::uint32_t orderSensitivePixels = 0;
    std::int32_t orderSensitiveBox[4] = {0, 0, 0, 0};
    // The pixels above where the two draw-order winners additionally predict colours that differ
    // by more than kMaxChannelDelta -- the comparison's own acceptance rule. This is the
    // preregistered contrast: a forward-order depth-disabled frame and a reversed-order
    // depth-disabled frame must differ on at least these pixels, because each shows the other
    // winner's colour there.
    std::uint32_t orderContrastPixels = 0;
    std::int32_t orderContrastBox[4] = {0, 0, 0, 0};
    // Covered pixels where the two nearest depths are within kDepthTieEpsilon of each
    // other. A non-zero count weakens any order claim: there the depth test cannot
    // arbitrate, so a reversed frame may legitimately differ.
    std::uint32_t depthTiePixels = 0;

    std::vector<GroupStats> groups;

    bool Ok() const { return status == SelectStatus::Ok; }

    const GroupStats* FindGroup(std::uint32_t group) const
    {
        for (const GroupStats& stats : groups)
            if (stats.group == group)
                return &stats;
        return nullptr;
    }
};

// One 8-bit level, declared above the selection code so that the order-contrast census there
// and the readback comparison below cannot disagree about what counts as a visible difference.
//
// kMaxChannelDelta is predetermined, not tuned against a measurement: both the shader inputs
// (UNORM texels and D3DCOLOR vertex colours) and the destination (A8R8G8B8) are 8-bit, and the
// pixel program carries partial-precision hints, so a float32 reference and the hardware can
// differ by at most one quantization step anywhere on this path.
constexpr int kMaxChannelDelta = 1;

// Two NDC depths closer than this are treated as a tie. It is a threshold on a value in
// [0, 1] produced by float arithmetic on the same two clip positions, not a tolerance
// chosen against a measurement: the surfaces this oracle is used on are hundreds of world
// units apart, which is four to five orders of magnitude above this.
constexpr float kDepthTieEpsilon = 1e-6f;

// Screen position of a vertex, from its clip position.
//
// D3D's viewport transform maps NDC x in [-1, 1] to [0, width] and NDC y to
// [height, 0]: y grows downward, so pixel row 0 is the top of the render target.
inline bool ClipToScreen(const float clip[4], std::uint32_t width, std::uint32_t height,
    float screen[2], float ndc[2])
{
    if (!(clip[3] > 0.0f) || !IsFinite(clip[3]))
        return false;
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    screen[0] = (ndc[0] + 1.0f) * 0.5f * static_cast<float>(width);
    screen[1] = (1.0f - ndc[1]) * 0.5f * static_cast<float>(height);
    return IsFinite(screen[0]) && IsFinite(screen[1]);
}

// Barycentric coordinates of a point in a screen triangle. The formulas below already
// yield weights that sum to one for either winding, and a point is inside exactly when
// all three are non-negative, so no sign normalisation is applied -- flipping them for a
// negatively wound triangle would break the sum and reject the interior. `doubleArea` is
// the shared denominator, kept so edge distances can be recovered from the weights.
inline void ScreenBarycentric(const float screen[3][2], float px, float py, float out[3],
    float* doubleArea)
{
    const float area = (screen[1][1] - screen[2][1]) * (screen[0][0] - screen[2][0])
        + (screen[2][0] - screen[1][0]) * (screen[0][1] - screen[2][1]);
    *doubleArea = area;

    out[0] = ((screen[1][1] - screen[2][1]) * (px - screen[2][0])
                 + (screen[2][0] - screen[1][0]) * (py - screen[2][1]))
        / area;
    out[1] = ((screen[2][1] - screen[0][1]) * (px - screen[2][0])
                 + (screen[0][0] - screen[2][0]) * (py - screen[2][1]))
        / area;
    out[2] = 1.0f - out[0] - out[1];
}

// ---------------------------------------------------------------------------
// Bounded multi-triangle selection.
//
// Which triangle colours a pixel on this device is decided one way: among the triangles
// the cull mode keeps, the one that covers the pixel and has the smallest interpolated
// clip.z / clip.w. That rule is what the loop below reproduces, so no prediction is ever
// "the first triangle in the list" or "the largest one".
//
// Depth is interpolated with the SCREEN barycentrics, not with the perspective-corrected
// weights. A rasterizer interpolates z/w linearly in window space while u, v, colour and
// fog are interpolated after division by w; using one set of weights for both gives a
// depth order that is right for a triangle seen face-on and wrong as soon as two planes
// meet at an angle -- which is the case this bound exists for.
//
// Pure function of its inputs: nothing here reads a rendered image, and no tolerance is
// fitted to one.
// ---------------------------------------------------------------------------
inline Selection SelectSamplesBounded(const std::vector<TriangleInput>& triangles,
    const Mip0View& mip, const PixelConstants& constants, const OracleConfig& config)
{
    Selection selection;
    selection.viewportWidth = config.viewportWidth;
    selection.viewportHeight = config.viewportHeight;
    selection.cellCount = config.cellCount;
    selection.triangleCount = static_cast<std::uint32_t>(triangles.size());
    selection.triangleLimit = kMaxTriangles;

    if (!mip.Valid())
    {
        selection.status = SelectStatus::NoTexture;
        selection.message = "no mip0 texels: the oracle cannot predict a sampled colour";
        return selection;
    }
    if (config.cellCount == 0)
    {
        selection.status = SelectStatus::NoCoverage;
        selection.message = "cellCount is zero";
        return selection;
    }
    if (triangles.empty())
    {
        selection.status = SelectStatus::NoTriangles;
        selection.message = "no triangles: there is nothing to predict";
        return selection;
    }
    if (triangles.size() > kMaxTriangles)
    {
        selection.status = SelectStatus::TooManyTriangles;
        selection.message = std::to_string(triangles.size()) + " triangles exceed the bound of "
            + std::to_string(kMaxTriangles) + "; the array is refused rather than truncated, "
            "because predicting part of a surface is not predicting the surface";
        return selection;
    }

    struct Prepared
    {
        std::uint32_t source = 0;
        std::uint32_t group = 0;
        float screen[3][2];
        float ndcZ[3];
        std::int32_t box[4] = {0, 0, 0, 0}; // clamped pixel box, inclusive
    };

    const std::int32_t viewportMaxX = static_cast<std::int32_t>(config.viewportWidth) - 1;
    const std::int32_t viewportMaxY = static_cast<std::int32_t>(config.viewportHeight) - 1;

    std::vector<Prepared> prepared;
    prepared.reserve(triangles.size());

    for (std::size_t source = 0; source < triangles.size(); ++source)
    {
        const TriangleInput& triangle = triangles[source];
        Prepared candidate{};
        candidate.source = static_cast<std::uint32_t>(source);
        candidate.group = triangle.group;

        bool projected = true;
        bool nonFinite = false;
        int cornersAtOrBehindEye = 0;
        for (int corner = 0; corner < 3 && projected; ++corner)
        {
            const float* clip = triangle.output[corner].clip;
            if (!IsFinite(clip[0]) || !IsFinite(clip[1]) || !IsFinite(clip[2])
                || !IsFinite(clip[3]))
            {
                nonFinite = true;
                projected = false;
                break;
            }
            if (!(clip[3] > 0.0f))
            {
                // A single corner behind the eye says nothing yet; the triangle is classified
                // after the loop on how many of its corners are at or behind the eye plane.
                ++cornersAtOrBehindEye;
                continue;
            }
            float ndc[2];
            if (!ClipToScreen(clip, config.viewportWidth, config.viewportHeight,
                    candidate.screen[corner], ndc))
            {
                nonFinite = true;
                projected = false;
                break;
            }
            candidate.ndcZ[corner] = clip[2] / clip[3];
            if (!IsFinite(candidate.ndcZ[corner]))
            {
                nonFinite = true;
                projected = false;
            }
        }
        if (!projected)
        {
            if (nonFinite)
                ++selection.trianglesNonFinite;
            else
                ++selection.trianglesBehindEye;
            continue;
        }
        if (cornersAtOrBehindEye > 0)
        {
            if (cornersAtOrBehindEye < 3)
            {
                // Mixed signs: the triangle passes through the eye plane. The device clips it
                // against the near plane and still draws part of it, so dropping it here would
                // predict a surface that is not the one the device draws. Refusing the whole
                // declaration is the only honest answer this oracle can give.
                ++selection.trianglesStraddlingEye;
                selection.status = SelectStatus::ClippingUnsupported;
                selection.message = "triangle " + std::to_string(source)
                    + " straddles the eye plane; its corners have clip w = ("
                    + std::to_string(triangle.output[0].clip[3]) + ", "
                    + std::to_string(triangle.output[1].clip[3]) + ", "
                    + std::to_string(triangle.output[2].clip[3]) + "). Near-plane clipping is not "
                    "modelled here, so the declaration is refused rather than predicting a surface "
                    "the device would clip";
                return selection;
            }
            // Wholly behind the eye: the device culls these too, so dropping it is correct. It is
            // counted and named in the census instead of vanishing into "no coverage".
            ++selection.trianglesBehindEye;
            continue;
        }

        const float area = (candidate.screen[1][1] - candidate.screen[2][1])
                * (candidate.screen[0][0] - candidate.screen[2][0])
            + (candidate.screen[2][0] - candidate.screen[1][0])
                * (candidate.screen[0][1] - candidate.screen[2][1]);
        if (!IsFinite(area) || std::fabs(area) < 1e-6f)
        {
            ++selection.trianglesDegenerate;
            continue;
        }

        // The cull decision is in screen space and uses the same twice-signed area the
        // barycentrics are built from: positive means clockwise on the pixel grid, which is
        // D3D's front face.
        bool kept = true;
        switch (config.cull)
        {
        case CullMode::None:
            kept = true;
            break;
        case CullMode::ClockwiseOnly:
            kept = area > 0.0f;
            break;
        case CullMode::CounterClockwiseOnly:
            kept = area < 0.0f;
            break;
        }
        if (!kept)
        {
            ++selection.trianglesCulled;
            continue;
        }
        ++selection.trianglesKept;

        float minX = candidate.screen[0][0];
        float maxX = candidate.screen[0][0];
        float minY = candidate.screen[0][1];
        float maxY = candidate.screen[0][1];
        for (int corner = 1; corner < 3; ++corner)
        {
            minX = std::min(minX, candidate.screen[corner][0]);
            maxX = std::max(maxX, candidate.screen[corner][0]);
            minY = std::min(minY, candidate.screen[corner][1]);
            maxY = std::max(maxY, candidate.screen[corner][1]);
        }
        candidate.box[0] = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(minX)));
        candidate.box[1] = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(minY)));
        candidate.box[2] = std::min(viewportMaxX, static_cast<std::int32_t>(std::ceil(maxX)));
        candidate.box[3] = std::min(viewportMaxY, static_cast<std::int32_t>(std::ceil(maxY)));
        if (candidate.box[2] < candidate.box[0] || candidate.box[3] < candidate.box[1])
            continue; // kept by the device too, just entirely off the target

        prepared.push_back(candidate);
    }

    // Group table, built from every input triangle so a group that lost all of its
    // triangles is still named with zero kept triangles rather than disappearing.
    std::vector<std::uint32_t> groupIds;
    for (const TriangleInput& triangle : triangles)
        if (std::find(groupIds.begin(), groupIds.end(), triangle.group) == groupIds.end())
            groupIds.push_back(triangle.group);
    std::sort(groupIds.begin(), groupIds.end());

    selection.groups.resize(groupIds.size());
    for (std::size_t index = 0; index < groupIds.size(); ++index)
        selection.groups[index].group = groupIds[index];
    for (const Prepared& triangle : prepared)
        for (std::size_t index = 0; index < groupIds.size(); ++index)
            if (groupIds[index] == triangle.group)
                ++selection.groups[index].trianglesKept;

    if (prepared.empty())
    {
        if (selection.trianglesBehindEye == selection.triangleCount)
        {
            selection.status = SelectStatus::BehindEye;
            selection.message = "every triangle is wholly behind the eye (all three corners have "
                "clip w <= 0), so none is in front of it";
        }
        else if (selection.trianglesNonFinite == selection.triangleCount)
        {
            selection.status = SelectStatus::NonFinite;
            selection.message = "every triangle has a non-finite clip position";
        }
        else if (selection.trianglesDegenerate == selection.triangleCount)
        {
            selection.status = SelectStatus::DegenerateTriangle;
            selection.message = "every triangle covers no screen area";
        }
        else
        {
            selection.status = SelectStatus::NoCoverage;
            selection.message = "no triangle survived: " + std::to_string(selection.trianglesCulled)
                + " culled, " + std::to_string(selection.trianglesBehindEye) + " behind the eye, "
                + std::to_string(selection.trianglesDegenerate) + " degenerate, "
                + std::to_string(selection.trianglesNonFinite) + " non-finite, "
                + std::to_string(selection.trianglesStraddlingEye) + " straddling the eye plane, of "
                + std::to_string(selection.triangleCount);
        }
        return selection;
    }

    std::int32_t boxMinX = viewportMaxX;
    std::int32_t boxMaxX = 0;
    std::int32_t boxMinY = viewportMaxY;
    std::int32_t boxMaxY = 0;
    for (const Prepared& triangle : prepared)
    {
        boxMinX = std::min(boxMinX, triangle.box[0]);
        boxMaxX = std::max(boxMaxX, triangle.box[2]);
        boxMinY = std::min(boxMinY, triangle.box[1]);
        boxMaxY = std::max(boxMaxY, triangle.box[3]);
    }

    selection.boundingBox[0] = boxMinX;
    selection.boundingBox[1] = boxMinY;
    selection.boundingBox[2] = boxMaxX;
    selection.boundingBox[3] = boxMaxY;

    // Cell extents, in pixels. A degenerate extent is widened to one pixel so a sliver
    // still stratifies instead of dividing by zero. One grid covers the union of every
    // kept triangle, so a cell names the same screen area for every group.
    const std::uint32_t cells = config.cellCount;
    const std::uint32_t cellsSquared = cells * cells;
    const float boxWidth = static_cast<float>(boxMaxX - boxMinX + 1);
    const float boxHeight = static_cast<float>(boxMaxY - boxMinY + 1);
    const float cellWidth = std::max(1.0f, boxWidth / static_cast<float>(cells));
    const float cellHeight = std::max(1.0f, boxHeight / static_cast<float>(cells));

    // Candidate pixels, bucketed by (group, cell) and kept in row-major visit order.
    // Collecting first and choosing afterwards keeps the ordering rule in one place.
    struct Candidate
    {
        std::uint32_t pixelX = 0;
        std::uint32_t pixelY = 0;
        std::uint32_t triangle = 0;
        std::uint32_t groupSlot = 0;
        std::uint32_t coveringTriangles = 1;
        float barycentric[3] = {0.0f, 0.0f, 0.0f};
        float weights[3] = {0.0f, 0.0f, 0.0f};
        float uv[2] = {0.0f, 0.0f};
        float texelCoordinate[2] = {0.0f, 0.0f};
        float edgeMargin = 0.0f;
        float boundaryMargin = 0.0f;
        float ndcDepth = 0.0f;
        float depthGap = 3.4e38f;
    };
    std::vector<std::vector<Candidate>> cellCandidates(groupIds.size() * cellsSquared);

    // Predicted colour of one prepared triangle at one pixel: the same interpolation and the same
    // audited pixel program a sample prediction uses. False means the point is not inside that
    // triangle or the program produced a non-finite value.
    //
    // This exists for the order-contrast census below, which has to know whether the two
    // draw-order winners would paint different pixels -- not merely whether they are different
    // triangles.
    const auto evaluateColour = [&](std::size_t index, float px, float py,
                                        std::uint8_t out[4]) -> bool {
        const Prepared& triangle = prepared[index];
        float barycentric[3];
        float doubleArea = 0.0f;
        ScreenBarycentric(triangle.screen, px, py, barycentric, &doubleArea);
        if (!(barycentric[0] >= 0.0f) || !(barycentric[1] >= 0.0f) || !(barycentric[2] >= 0.0f))
            return false;

        const TriangleInput& source = triangles[triangle.source];
        float clipW[3];
        for (int corner = 0; corner < 3; ++corner)
            clipW[corner] = source.output[corner].clip[3];
        float weights[3];
        PerspectiveWeights(barycentric, clipW, weights);

        float colour[4];
        if (!EvalPixel(weights, source.output, mip, constants, colour))
            return false;
        for (int channel = 0; channel < 4; ++channel)
            out[channel] = QuantizeUnorm8(colour[channel]);
        return true;
    };

    for (std::int32_t pixelY = boxMinY; pixelY <= boxMaxY; ++pixelY)
    {
        for (std::int32_t pixelX = boxMinX; pixelX <= boxMaxX; ++pixelX)
        {
            ++selection.consideredPixels;
            const float px = static_cast<float>(pixelX) + config.fragmentSampleOffset;
            const float py = static_cast<float>(pixelY) + config.fragmentSampleOffset;

            // Depth resolution for this pixel: the nearest kept triangle that covers it,
            // the farthest cover in draw order, and how many triangles cover it at all.
            // The bound is kMaxTriangles, so this inner loop is at most 256 iterations per
            // pixel of the union box.
            std::size_t nearest = prepared.size();
            std::size_t firstCovering = prepared.size();
            std::size_t lastCovering = prepared.size();
            std::uint32_t covering = 0;
            float nearestDepth = 0.0f;
            float secondDepth = 3.4e38f;
            float nearestBarycentric[3] = {0.0f, 0.0f, 0.0f};
            float firstMargin = 0.0f;
            float lastMargin = 0.0f;

            for (std::size_t index = 0; index < prepared.size(); ++index)
            {
                const Prepared& triangle = prepared[index];
                if (pixelX < triangle.box[0] || pixelX > triangle.box[2] || pixelY < triangle.box[1]
                    || pixelY > triangle.box[3])
                    continue;

                float barycentric[3];
                float doubleArea = 0.0f;
                ScreenBarycentric(triangle.screen, px, py, barycentric, &doubleArea);
                if (!(barycentric[0] >= 0.0f) || !(barycentric[1] >= 0.0f)
                    || !(barycentric[2] >= 0.0f))
                    continue;

                ++covering;

                // Pixel distance to the nearest edge, from the barycentric weight times the
                // twice-area over the edge length.
                const float absoluteArea = std::fabs(doubleArea);
                float margin = 1e30f;
                for (int corner = 0; corner < 3; ++corner)
                {
                    const int next = (corner + 1) % 3;
                    const int nextNext = (corner + 2) % 3;
                    const float dx = triangle.screen[nextNext][0] - triangle.screen[next][0];
                    const float dy = triangle.screen[nextNext][1] - triangle.screen[next][1];
                    const float length = std::sqrt(dx * dx + dy * dy);
                    if (!(length > 0.0f))
                    {
                        margin = 0.0f;
                        break;
                    }
                    margin = std::min(margin, barycentric[corner] * absoluteArea / length);
                }

                if (firstCovering == prepared.size())
                {
                    firstCovering = index;
                    firstMargin = margin;
                }
                lastCovering = index;
                lastMargin = margin;

                // z/w is affine in screen space, so the screening barycentrics are the
                // right weights here and the perspective-corrected ones are not.
                const float depth = barycentric[0] * triangle.ndcZ[0] + barycentric[1] * triangle.ndcZ[1]
                    + barycentric[2] * triangle.ndcZ[2];
                if (nearest == prepared.size())
                {
                    // First cover: there is no previous depth to demote to second place, so
                    // secondDepth keeps its +inf sentinel.
                    nearestDepth = depth;
                    nearest = index;
                    for (int corner = 0; corner < 3; ++corner)
                        nearestBarycentric[corner] = barycentric[corner];
                }
                else if (depth < nearestDepth)
                {
                    secondDepth = nearestDepth;
                    nearestDepth = depth;
                    nearest = index;
                    for (int corner = 0; corner < 3; ++corner)
                        nearestBarycentric[corner] = barycentric[corner];
                }
                else if (depth < secondDepth)
                {
                    secondDepth = depth;
                }
            }

            if (covering >= 2)
            {
                ++selection.multiCoveredPixels;
                if (secondDepth - nearestDepth < kDepthTieEpsilon)
                    ++selection.depthTiePixels;
                if (firstCovering != lastCovering
                    && firstMargin >= config.overlapEdgeMarginPixels
                    && lastMargin >= config.overlapEdgeMarginPixels)
                {
                    if (selection.orderSensitivePixels == 0)
                    {
                        selection.orderSensitiveBox[0] = pixelX;
                        selection.orderSensitiveBox[1] = pixelY;
                        selection.orderSensitiveBox[2] = pixelX;
                        selection.orderSensitiveBox[3] = pixelY;
                    }
                    else
                    {
                        selection.orderSensitiveBox[0] = std::min(selection.orderSensitiveBox[0], pixelX);
                        selection.orderSensitiveBox[1] = std::min(selection.orderSensitiveBox[1], pixelY);
                        selection.orderSensitiveBox[2] = std::max(selection.orderSensitiveBox[2], pixelX);
                        selection.orderSensitiveBox[3] = std::max(selection.orderSensitiveBox[3], pixelY);
                    }
                    ++selection.orderSensitivePixels;

                    // Owner change is not an image change. Compare what the two draw-order
                    // winners would actually paint, rounded the way the target rounds and scored
                    // with the comparison's own tolerance: only a pixel that survives this test
                    // can be evidence that a depth-disabled frame depends on draw order.
                    std::uint8_t firstColour[4] = {0, 0, 0, 0};
                    std::uint8_t lastColour[4] = {0, 0, 0, 0};
                    if (evaluateColour(firstCovering, px, py, firstColour)
                        && evaluateColour(lastCovering, px, py, lastColour))
                    {
                        int worst = 0;
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            const int delta = static_cast<int>(firstColour[channel])
                                - static_cast<int>(lastColour[channel]);
                            worst = std::max(worst, delta < 0 ? -delta : delta);
                        }
                        if (worst > kMaxChannelDelta)
                        {
                            if (selection.orderContrastPixels == 0)
                            {
                                selection.orderContrastBox[0] = pixelX;
                                selection.orderContrastBox[1] = pixelY;
                                selection.orderContrastBox[2] = pixelX;
                                selection.orderContrastBox[3] = pixelY;
                            }
                            else
                            {
                                selection.orderContrastBox[0] =
                                    std::min(selection.orderContrastBox[0], pixelX);
                                selection.orderContrastBox[1] =
                                    std::min(selection.orderContrastBox[1], pixelY);
                                selection.orderContrastBox[2] =
                                    std::max(selection.orderContrastBox[2], pixelX);
                                selection.orderContrastBox[3] =
                                    std::max(selection.orderContrastBox[3], pixelY);
                            }
                            ++selection.orderContrastPixels;
                        }
                    }
                }
            }

            if (nearest == prepared.size())
                continue; // no kept triangle covers this pixel

            const Prepared& visible = prepared[nearest];
            std::size_t groupSlot = 0;
            for (std::size_t index = 0; index < groupIds.size(); ++index)
                if (groupIds[index] == visible.group)
                    groupSlot = index;
            ++selection.groups[groupSlot].coveredPixels;

            // Strict interior of the triangle the prediction will read.
            float visibleMargin = 1e30f;
            {
                const float absoluteArea = std::fabs((visible.screen[1][1] - visible.screen[2][1])
                        * (visible.screen[0][0] - visible.screen[2][0])
                    + (visible.screen[2][0] - visible.screen[1][0])
                        * (visible.screen[0][1] - visible.screen[2][1]));
                for (int corner = 0; corner < 3; ++corner)
                {
                    const int next = (corner + 1) % 3;
                    const int nextNext = (corner + 2) % 3;
                    const float dx = visible.screen[nextNext][0] - visible.screen[next][0];
                    const float dy = visible.screen[nextNext][1] - visible.screen[next][1];
                    const float length = std::sqrt(dx * dx + dy * dy);
                    if (!(length > 0.0f))
                    {
                        visibleMargin = 0.0f;
                        break;
                    }
                    visibleMargin = std::min(
                        visibleMargin, nearestBarycentric[corner] * absoluteArea / length);
                }
            }
            if (!(visibleMargin >= config.minEdgeMarginPixels))
                continue;

            const float depthGap = covering >= 2 ? secondDepth - nearestDepth : 3.4e38f;
            if (depthGap < config.minDepthGap)
                continue;

            const float clipW[3] = {triangles[visible.source].output[0].clip[3],
                triangles[visible.source].output[1].clip[3],
                triangles[visible.source].output[2].clip[3]};
            float weights[3];
            PerspectiveWeights(nearestBarycentric, clipW, weights);

            float uv[2] = {0.0f, 0.0f};
            for (int corner = 0; corner < 3; ++corner)
            {
                uv[0] += weights[corner] * triangles[visible.source].output[corner].uv[0];
                uv[1] += weights[corner] * triangles[visible.source].output[corner].uv[1];
            }

            const float texelX = uv[0] * static_cast<float>(mip.width);
            const float texelY = uv[1] * static_cast<float>(mip.height);
            const float fractionX = texelX - std::floor(texelX);
            const float fractionY = texelY - std::floor(texelY);
            const float boundaryMargin = std::min(std::min(fractionX, 1.0f - fractionX),
                std::min(fractionY, 1.0f - fractionY));
            if (!(boundaryMargin >= config.minTexelBoundaryMargin))
                continue;

            ++selection.eligiblePixels;
            ++selection.groups[groupSlot].eligiblePixels;

            std::uint32_t cellColumn = static_cast<std::uint32_t>(
                std::max(0.0f, (px - static_cast<float>(boxMinX)) / cellWidth));
            std::uint32_t cellRow = static_cast<std::uint32_t>(
                std::max(0.0f, (py - static_cast<float>(boxMinY)) / cellHeight));
            if (cellColumn >= cells)
                cellColumn = cells - 1;
            if (cellRow >= cells)
                cellRow = cells - 1;

            Candidate entry;
            entry.pixelX = static_cast<std::uint32_t>(pixelX);
            entry.pixelY = static_cast<std::uint32_t>(pixelY);
            entry.triangle = visible.source;
            entry.groupSlot = static_cast<std::uint32_t>(groupSlot);
            entry.coveringTriangles = covering;
            for (int corner = 0; corner < 3; ++corner)
            {
                entry.barycentric[corner] = nearestBarycentric[corner];
                entry.weights[corner] = weights[corner];
            }
            entry.uv[0] = uv[0];
            entry.uv[1] = uv[1];
            entry.texelCoordinate[0] = texelX;
            entry.texelCoordinate[1] = texelY;
            entry.edgeMargin = visibleMargin;
            entry.boundaryMargin = boundaryMargin;
            entry.ndcDepth = nearestDepth;
            entry.depthGap = depthGap;

            cellCandidates[groupSlot * cellsSquared + static_cast<std::size_t>(cellRow) * cells + cellColumn]
                .push_back(entry);
        }
    }

    for (const std::vector<Candidate>& list : cellCandidates)
        if (!list.empty())
            ++selection.occupiedCells;

    if (selection.occupiedCells == 0)
    {
        selection.status = SelectStatus::NoCoverage;
        selection.message = "no pixel centre in the geometry bounding box is "
            + std::to_string(config.minEdgeMarginPixels) + " pixels inside its nearest visible "
            "triangle and " + std::to_string(config.minTexelBoundaryMargin)
            + " texels from a texel boundary (" + std::to_string(selection.trianglesKept)
            + " triangle(s) kept of " + std::to_string(selection.triangleCount) + ")";
        return selection;
    }

    // Three deterministic passes over the cells, cells in row-major order each time:
    // first candidate, last candidate, middle candidate. Taking the ends of each cell's
    // scanline run is what spreads the samples inside a cell; taking one candidate per
    // cell alone would leave a sliver's few usable cells under-sampled.
    //
    // A pass walks the cells and visits every group inside each cell. One group therefore
    // cannot consume the global sample budget before the others are reached, and the
    // per-group quota is what turns "each plane was sampled" into a check. For a
    // one-triangle geometry this reduces to the cell-major walk it has always been.
    const std::uint32_t perGroupCap = config.maxSamplesPerGroup != 0
        ? config.maxSamplesPerGroup
        : config.maxSamples;
    std::vector<std::uint32_t> perGroupCount(groupIds.size(), 0);

    std::vector<Sample> chosen;
    const std::size_t passes = 3;
    for (std::size_t pass = 0; pass < passes && chosen.size() < config.maxSamples; ++pass)
    {
        for (std::uint32_t cellIndex = 0; cellIndex < cellsSquared; ++cellIndex)
        {
            for (std::size_t groupSlot = 0; groupSlot < groupIds.size(); ++groupSlot)
            {
                if (chosen.size() >= config.maxSamples)
                    break;
                if (perGroupCount[groupSlot] >= perGroupCap)
                    continue;
                const std::vector<Candidate>& list = cellCandidates[groupSlot * cellsSquared + cellIndex];
                if (list.empty())
                    continue;

                std::size_t position = 0;
                if (pass == 1)
                    position = list.size() - 1;
                else if (pass == 2)
                    position = list.size() / 2;
                const Candidate& candidate = list[position];

                bool duplicate = false;
                for (const Sample& existing : chosen)
                    if (existing.pixelX == candidate.pixelX && existing.pixelY == candidate.pixelY)
                        duplicate = true;
                if (duplicate)
                    continue;

                Sample sample;
                sample.pixelX = candidate.pixelX;
                sample.pixelY = candidate.pixelY;
                sample.cellRow = cellIndex / cells;
                sample.cellColumn = cellIndex % cells;
                sample.cellIndex = cellIndex;
                for (int corner = 0; corner < 3; ++corner)
                {
                    sample.barycentric[corner] = candidate.barycentric[corner];
                    sample.weights[corner] = candidate.weights[corner];
                }
                sample.uv[0] = candidate.uv[0];
                sample.uv[1] = candidate.uv[1];
                sample.texelCoordinate[0] = candidate.texelCoordinate[0];
                sample.texelCoordinate[1] = candidate.texelCoordinate[1];
                sample.texelX = WrapTexelIndex(candidate.texelCoordinate[0], mip.width);
                sample.texelY = WrapTexelIndex(candidate.texelCoordinate[1], mip.height);
                sample.edgeMarginPixels = candidate.edgeMargin;
                sample.texelBoundaryMargin = candidate.boundaryMargin;
                sample.triangle = candidate.triangle;
                sample.group = groupIds[groupSlot];
                sample.coveringTriangles = candidate.coveringTriangles;
                sample.ndcDepth = candidate.ndcDepth;
                sample.depthGap = candidate.depthGap;
                chosen.push_back(sample);
                ++perGroupCount[groupSlot];
            }
        }
    }

    // Group first, then row-major over the target, so the report reads plane by plane and
    // two runs with the same inputs list their samples in the same order. With one group
    // this is exactly row-major order.
    std::sort(chosen.begin(), chosen.end(), [](const Sample& a, const Sample& b) {
        if (a.group != b.group)
            return a.group < b.group;
        if (a.pixelY != b.pixelY)
            return a.pixelY < b.pixelY;
        return a.pixelX < b.pixelX;
    });

    std::uint32_t distinctCells = 0;
    for (std::uint32_t cellIndex = 0; cellIndex < cellsSquared; ++cellIndex)
    {
        for (const Sample& sample : chosen)
        {
            if (sample.cellIndex == cellIndex)
            {
                ++distinctCells;
                break;
            }
        }
    }

    for (std::size_t groupSlot = 0; groupSlot < groupIds.size(); ++groupSlot)
        selection.groups[groupSlot].samples = perGroupCount[groupSlot];

    // A group with pixels available has to be represented. Checked before the aggregate
    // counts so the report names the plane that was missed instead of saying "8 of 8".
    if (config.minSamplesPerGroup > 0)
    {
        for (const GroupStats& group : selection.groups)
        {
            if (!group.PixelsAvailable() || group.samples >= config.minSamplesPerGroup)
                continue;
            selection.status = SelectStatus::GroupUnderSampled;
            selection.message = "group " + std::to_string(group.group) + " covers "
                + std::to_string(group.coveredPixels) + " pixel(s) but supplied only "
                + std::to_string(group.samples) + " sample(s) of the "
                + std::to_string(config.minSamplesPerGroup) + " required";
            selection.samples = chosen;
            return selection;
        }
    }

    if (chosen.size() < config.minSamples)
    {
        selection.status = SelectStatus::TooFewSamples;
        selection.message = "only " + std::to_string(chosen.size()) + " sample(s) met the "
            + std::to_string(config.minEdgeMarginPixels) + "-pixel interior and "
            + std::to_string(config.minTexelBoundaryMargin) + "-texel boundary margins; "
            + std::to_string(config.minSamples) + " are required";
        selection.samples = chosen;
        return selection;
    }
    if (distinctCells < config.minOccupiedCells)
    {
        selection.status = SelectStatus::TooFewCells;
        selection.message = std::to_string(chosen.size()) + " sample(s) span only "
            + std::to_string(distinctCells) + " of the " + std::to_string(cells) + "x"
            + std::to_string(cells) + " grid cells; " + std::to_string(config.minOccupiedCells)
            + " are required";
        selection.samples = chosen;
        return selection;
    }

    // Predictions are filled last, and only through the full pixel program: no shortcut
    // such as "the answer is the texel" is used to produce them, because a shortcut
    // would agree with a wrong shader transcription on exactly the fixture under test.
    for (Sample& sample : chosen)
    {
        float out[4];
        if (!EvalPixel(sample.weights, triangles[sample.triangle].output, mip, constants, out))
        {
            selection.status = SelectStatus::NonFinite;
            selection.message = "the pixel program produced a non-finite value at pixel ("
                + std::to_string(sample.pixelX) + ", " + std::to_string(sample.pixelY) + ")";
            selection.samples = chosen;
            return selection;
        }
        for (int channel = 0; channel < 4; ++channel)
            sample.expected[channel] = QuantizeUnorm8(out[channel]);
    }

    selection.samples = chosen;
    selection.status = SelectStatus::Ok;
    return selection;
}

// One triangle: the same computation over a single-element geometry, with the per-group
// quota neutral. Every multi-triangle entry point goes through the function above, so the
// single-triangle case is a test of the general code rather than a parallel implementation
// that could drift from it.
inline Selection SelectSamples(const VertexOutput outputs[3], const Mip0View& mip,
    const PixelConstants& constants, const OracleConfig& config)
{
    std::vector<TriangleInput> geometry(1);
    for (int corner = 0; corner < 3; ++corner)
    {
        geometry[0].output[corner] = outputs[corner];
        geometry[0].index[corner] = static_cast<std::uint32_t>(corner);
    }
    geometry[0].group = 0;
    return SelectSamplesBounded(geometry, mip, constants, config);
}

// ---------------------------------------------------------------------------
// Comparison against a readback.
//
// kMaxChannelDelta is declared above the selection code, because the order-contrast census
// there is scored with the same rule this comparison applies.
// ---------------------------------------------------------------------------

struct SampleResult
{
    std::uint32_t pixelX = 0;
    std::uint32_t pixelY = 0;
    std::uint32_t texelX = 0;
    std::uint32_t texelY = 0;
    std::uint8_t expected[4] = {0, 0, 0, 0};
    std::uint8_t actual[4] = {0, 0, 0, 0};
    int delta[4] = {0, 0, 0, 0};
    bool matched = false;
};

struct Comparison
{
    std::vector<SampleResult> results;
    std::uint32_t matched = 0;
    std::uint32_t mismatched = 0;
    int worstDelta = 0;

    bool Ok() const { return mismatched == 0 && matched > 0; }
};

// `rgba` is width * height * 4 bytes, row-major, row 0 at the top of the target, which
// is the order the SYSTEMMEM readback produces and the order RenderTargetData copies.
inline Comparison CompareSamples(const std::vector<Sample>& samples, const std::uint8_t* rgba,
    std::uint32_t width, std::uint32_t height, int tolerance)
{
    Comparison comparison;
    if (rgba == nullptr)
        return comparison;

    for (const Sample& sample : samples)
    {
        SampleResult result;
        result.pixelX = sample.pixelX;
        result.pixelY = sample.pixelY;
        result.texelX = sample.texelX;
        result.texelY = sample.texelY;

        bool inRange = sample.pixelX < width && sample.pixelY < height;
        for (int channel = 0; channel < 4; ++channel)
        {
            result.expected[channel] = sample.expected[channel];
            result.actual[channel] = 0;
        }
        if (inRange)
        {
            const std::size_t offset = (static_cast<std::size_t>(sample.pixelY) * width
                                           + sample.pixelX)
                * 4u;
            for (int channel = 0; channel < 4; ++channel)
                result.actual[channel] = rgba[offset + static_cast<std::size_t>(channel)];
        }

        result.matched = inRange;
        for (int channel = 0; channel < 4; ++channel)
        {
            result.delta[channel] = static_cast<int>(result.actual[channel])
                - static_cast<int>(result.expected[channel]);
            const int magnitude = result.delta[channel] < 0 ? -result.delta[channel]
                                                            : result.delta[channel];
            if (magnitude > comparison.worstDelta)
                comparison.worstDelta = magnitude;
            if (magnitude > tolerance)
                result.matched = false;
        }

        if (result.matched)
            ++comparison.matched;
        else
            ++comparison.mismatched;
        comparison.results.push_back(result);
    }
    return comparison;
}

// ---------------------------------------------------------------------------
// The audited baseline gate.
//
// The reduction in this file was derived for one fixture: all-white vertex colours,
// material colour (1,1,1,0), fog colour zero, fog vector (0,1,0,0). Each of those is a
// real shader input, so a run that changes one is outside the audited case and must be
// refused rather than predicted loosely. The checks are numerical and reported, not
// assumed.
// ---------------------------------------------------------------------------
struct BaselineFacts
{
    bool materialWhiteOpaqueFactor = false; // PS c1 == (1,1,1,0)
    bool fogColorZero = false;              // PS c0 == (0,0,0,0)
    bool fogVectorDisabled = false;         // VS c21 == (0,1,0,0)
    bool vertexColorsWhite = false;         // all three D3DCOLOR are 0xFFFFFFFF
    int whiteVertexCount = 0;

    bool Ok() const
    {
        return materialWhiteOpaqueFactor && fogColorZero && fogVectorDisabled
            && vertexColorsWhite;
    }

    std::string Describe() const
    {
        std::string text;
        text += materialWhiteOpaqueFactor ? "material=(1,1,1,0) " : "material NOT (1,1,1,0) ";
        text += fogColorZero ? "fog_color=0 " : "fog_color NOT zero ";
        text += fogVectorDisabled ? "fog_vector=(0,1,0,0)" : "fog_vector NOT (0,1,0,0)";
        text += ", white vertex colours " + std::to_string(whiteVertexCount) + "/3";
        return text;
    }
};

inline bool NearlyEqual(float a, float b)
{
    return std::fabs(a - b) <= 1e-6f;
}

inline BaselineFacts CheckBaseline(const VertexInput vertices[3], const PixelConstants& constants,
    const VertexConstants& vertexConstants)
{
    BaselineFacts facts;
    facts.materialWhiteOpaqueFactor = NearlyEqual(constants.material[0], 1.0f)
        && NearlyEqual(constants.material[1], 1.0f) && NearlyEqual(constants.material[2], 1.0f)
        && NearlyEqual(constants.material[3], 0.0f);
    facts.fogColorZero = NearlyEqual(constants.fogColor[0], 0.0f)
        && NearlyEqual(constants.fogColor[1], 0.0f) && NearlyEqual(constants.fogColor[2], 0.0f)
        && NearlyEqual(constants.fogColor[3], 0.0f);
    facts.fogVectorDisabled = NearlyEqual(vertexConstants.fog[0], 0.0f)
        && NearlyEqual(vertexConstants.fog[1], 1.0f) && NearlyEqual(vertexConstants.fog[2], 0.0f)
        && NearlyEqual(vertexConstants.fog[3], 0.0f);
    for (int corner = 0; corner < 3; ++corner)
        if (vertices[corner].color == 0xFFFFFFFFu)
            ++facts.whiteVertexCount;
    facts.vertexColorsWhite = facts.whiteVertexCount == 3;
    return facts;
}

} // namespace world_surface_oracle

#endif // WORLD_SURFACE_ORACLE_H
