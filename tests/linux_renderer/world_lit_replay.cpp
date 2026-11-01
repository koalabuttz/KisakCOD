// tests/linux_renderer/world_lit_replay.cpp
//
// Standalone replay of ONE exported static world surface's TECHNIQUE_LIT pass through
// DXVK Native D3D9, with a CPU prediction of the same pixels.
//
// What this program is
// --------------------
// It reads one schema-1 lit bundle (world_lit_bundle.h), builds the D3D9 objects the bundle
// describes, draws the surface's 42 surface-local indices once into a fixed 512x512
// offscreen A8R8G8B8 target, reads the target back through a SYSTEMMEM surface, and
// compares at least eight predeclared interior pixels against a CPU evaluation of the same
// two shader programs. Samples are integer pixel coordinates on the ordinary viewport mapping,
// the same convention the unlit oracle uses; the report states the measured texel travel at
// those coordinates rather than claiming a margin the numbers do not support.
//
// What this program is NOT
// ------------------------
// It is not a renderer and it is not a frame replay:
//   * the camera is synthetic. There is no captured refdef. The eye is derived from the
//     referenced triangles' own extent and area-weighted normal, and that distance is used as it
//     is: no option rescales it and no measurement moves it. --camera-eye/--camera-target replace
//     the derivation with the caller's numbers. Neither is a recorded view;
//   * the lightmap orientation is not flipped. The capture is uploaded exactly as the fast
//     file stored it, because the shader reads the atlas's two vertical halves and a flip
//     would swap the upper sample with the lower one;
//   * the base texture is the prepared IWI decoded by the repository's own decoder.
//
// Fidelity rules that shape the code
// ----------------------------------
//   * The state block is decoded from the bundle's two recorded words with the engine's own
//     bit layout (src/gfx_d3d/r_state.h). A state this file does not model stops the run.
//   * The sampler DWORDs are produced by R_DecodeSamplerState. The filter entry half of that
//     DWORD is built at renderer init from device caps and cvars the bundle does not carry,
//     so the r_texFilterDisable=1 branch, which r_state.cpp pins completely, is used and the
//     report says so. s0 (raw 19) therefore wraps on all three axes; the lightmap (raw 0x62)
//     clamps U and V and wraps W. Those are different states and are not merged.
//   * The lit vertex declaration is transcribed independently here as position FLOAT4 at 0,
//     colour D3DCOLOR at 16, texcoord FLOAT4 at 20, normal UBYTE4 at 36, stride 44, and
//     static_asserted against the loader's pins so the two cannot drift.
//   * The device is admitted by a Vulkan preflight, not by D3D9's adapter string, which DXVK
//     spoofs: a CPU or llvmpipe device is refused before any D3D9 object exists, and software
//     vertex processing is not a fallback.
//   * This profile uploads codes 60, 76, 41 and 42 and nothing else. It has no code-40
//     argument: the lit pass takes its colour from the bound colour map times the captured
//     lightmap, and PS c1 is a shader-local def in this program. The upload function names
//     the four registers it writes and never writes PS c1.
//
// The CPU prediction
// ------------------
// The prediction is an audited equivalent of the two packaged programs, taken from the
// completed shader audit of this exact pair (VS FNV e1989ed410dbcfd6, 179 DWORDs; PS FNV
// 3551ae4bda0872e4, 148 DWORDs):
//
//   VS  eyeRelative = dp4(vertex.xyz1, c4..c7)      (c4..c7 = code 60, transposed world)
//       clip        = dp4(eyeRelative, c0..c3)      (c0..c3 = code 76, transposed view-proj)
//       normal      = (byte.xyz * 1/127 - 1) * (byte.w * 1/255 + 192/255)
//       normalWorld = dp3(normal, c4..c6)
//       fog         = saturate(exp2((rcp(rsq(|eyeRel|^2)) * c21.z + c21.w) * log2(e)))
//       outputs     = COLOR0=v1, TEXCOORD0=v2 (all four), TEXCOORD1=(normalWorld, fog)
//
//   PS  A = s3.POINT_CLAMP(UV.lmU, UV.lmV * 0.5)
//       B = s3.POINT_CLAMP(UV.lmU, UV.lmV * 0.5 + 0.5)
//       x = A.a * 4.0783... - 2.0796...  y = B.a * 4.0645... - 2.0645...
//       w = saturate(rsqrt(x*x + y*y + 1))
//       L = B.rgb * w + A.rgb
//       D = s0.POINT_WRAP(UV.uv).rgb * COLOR0.rgb
//       out.rgb = fog * (D * L - F) + F       out.a = 1
//
// There is no lightmap exponent, no per-pixel dot product with the normal, no guessed
// brightness multiplier and no code-40 material colour. The normal reaches TEXCOORD1 and the
// pixel program declares only its .w, so mutating normal bytes provably cannot change this
// image; that is stated rather than offered as a control.
//
// Build and run: see README.md next to this file. The DXVK native headers define
// __stdcall/WINAPI as empty, so this uses the default Linux ABI throughout; never add
// WINAPI or __stdcall here.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <d3d9.h>
#include <windows.h>
#include <wsi/native_sdl3.h>

#include "world_lit_bundle.h"
#include "world_surface_bundle.h"
#include "world_surface_context.h"

// The lit profile pins two engine tables twice: once in the loader, once in the numeric
// context. These make a disagreement a compile error instead of an upload to the wrong
// register.
static_assert(world_lit_bundle::kCodeFog == world_surface_context::kCodeFog,
    "code 41 disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kCodeFogColor == world_surface_context::kCodeFogColor,
    "code 42 disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kCodeTransposeWorld == world_surface_context::kCodeTransposeWorldMatrix,
    "code 60 disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kCodeTransposeViewProjection
        == world_surface_context::kCodeTransposeViewProjectionMatrix,
    "code 76 disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kVsDestTransposeViewProjection
        == world_surface_context::kVsDestTransposeViewProjection,
    "the code-76 destination disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kVsDestTransposeWorld == world_surface_context::kVsDestTransposeWorld,
    "the code-60 destination disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kVsDestFog == world_surface_context::kVsDestFog,
    "the code-41 destination disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kPsDestFogColor == world_surface_context::kPsDestFogColor,
    "the code-42 destination disagrees between the lit profile and the context");
static_assert(world_lit_bundle::kCodeMaterialColor == world_surface_context::kCodeMaterialColor,
    "code 40 disagrees between the lit profile and the context");

// The replay links the repository's existing IWI decoder directly
// (src/linux/client/window/iwi_assets.cpp): no intermediate image container, no second decode
// path, and nothing that re-encodes the base texture before it reaches the GPU.
#include "iwi_assets.h"

// The decoder objects call the engine's assert hook, which for Linux lives in
// src/linux/sys_assert.cpp and needs the common runtime. This binary does not link the
// engine runtime, so the hook is defined here with the engine's exact signature and the
// behaviour that matters: report, then stop.
void MyAssertHandler(const char* filename, int line, int type, const char* fmt, ...)
{
    std::fprintf(stderr, "[assert] %s:%d type=%d: ", filename ? filename : "?", line, type);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt ? fmt : "", args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

namespace {

// ---------------------------------------------------------------------------
// Fixed render target. A size that depended on the desktop would not be comparable
// between runs.
//
// 512 rather than 256: the sample floor below is eight interior pixels, and this surface's
// colour map is a wire pattern whose texel boundaries are dense along the surface's long axis.
// Doubling the linear resolution doubles the number of pixel centres per texel, which is what
// makes eight unambiguous points reachable without loosening the texel margin. The size is a
// constant in this file, not an option, so it cannot be tuned per run.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kRenderWidth = 512;
constexpr std::uint32_t kRenderHeight = 512;

// Clear colour. Magenta is a colour this material cannot produce, which keeps "the draw
// produced black" and "the draw produced nothing" distinguishable in the readback.
constexpr std::uint8_t kClearRed = 255;
constexpr std::uint8_t kClearGreen = 0;
constexpr std::uint8_t kClearBlue = 255;

// A pass needs the surface to occupy enough of the target to be measurable.
constexpr std::size_t kMinCoveredPixels = 256;

// Predeclared comparison points. Interior pixels only: a pixel whose centre is within this
// many pixels of a triangle edge is skipped, so fill-rule and edge-rule differences cannot
// be mistaken for a shading disagreement.
constexpr float kInteriorMarginPixels = 2.0f;
constexpr std::size_t kMaxSamplePoints = 16;
// The floor is a floor. A run that can only prove one pixel does not describe the pass, and
// lowering this to whatever a camera happened to produce would be the same failure written
// differently. If the derived camera cannot place eight pixels this far inside the geometry
// and this far from a texel boundary, the run fails and says so.
constexpr std::size_t kRequiredSamplePoints = 8;
// One count of 255, fixed. The prediction is evaluated at the same integer pixel centre the
// rasteriser evaluates, with the same point-sampling rule, so the only remaining difference is
// the 8-bit rounding of the write. A wider bound would absorb a real disagreement.
constexpr int kPerChannelTolerance = 1;

enum ExitCode
{
    ExitPass = 0,
    ExitFailure = 1,
    ExitUsage = 2,
    ExitBundleInvalid = 3,
    ExitStateUnsupported = 4,
    ExitDeviceUnavailable = 5,
    ExitOracleMismatch = 6,
    ExitNotObservable = 7,
    ExitTextureInvalid = 8,
};

#if defined(__GNUC__)
#define LIT_PRINTF_FORMAT(first, second) __attribute__((format(printf, first, second)))
#else
#define LIT_PRINTF_FORMAT(first, second)
#endif

std::string format(const char* fmt, ...) LIT_PRINTF_FORMAT(1, 2);

std::string format(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return std::string(buffer);
}

std::string hrText(HRESULT hr)
{
    return format("0x%08X", static_cast<unsigned>(hr));
}

// A float with exactly the bits the packaged program's def instruction carries. The audit
// transcribed the def operands as DWORDs, and re-deriving them from a decimal literal would
// give a different float in the last place, so they are bit-cast here.
float FloatBits(std::uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// VS def c8 = bits(3c010204, 3b808081, bf800000, 3f40c0c1): the packed-normal decode.
constexpr std::uint32_t kNormalScaleXBits = 0x3c010204u; // 1/127
constexpr std::uint32_t kNormalScaleWBits = 0x3b808081u; // 1/255
constexpr std::uint32_t kNormalBiasWBits = 0x3f40c0c1u;  // 192/255

// VS def c9.z = log2(e), used by the fog exponent.
constexpr std::uint32_t kLog2EBits = 0x3fb8aa3bu;

// PS def c2 = bits(40828f5c, 40821084, c0051eb8, c0042108): the lightmap alpha decode.
constexpr std::uint32_t kAlphaScale0Bits = 0x40828f5cu;
constexpr std::uint32_t kAlphaScale1Bits = 0x40821084u;
constexpr std::uint32_t kAlphaBias0Bits = 0xc0051eb8u;
constexpr std::uint32_t kAlphaBias1Bits = 0xc0042108u;

// PS def c1 = (1, 0.5, 0, 0): the lightmap coordinate scale and the alpha-1 constant.
constexpr float kLightmapVScale = 0.5f;

// ---------------------------------------------------------------------------
// D3D9 state block decode.
//
// Bit layout copied from src/gfx_d3d/r_state.h (GFXS0_* / GFXS1_*) and the lookup tables in
// src/gfx_d3d/r_state.cpp (s_cullTable_30, s_depthTestTable_30). Every field is decoded or
// the block is reported unsupported: no field is filled with a plausible default.
// ---------------------------------------------------------------------------
struct DecodedState
{
    bool supported = true;
    std::string unsupportedReason;

    D3DCULL cull;
    D3DCMPFUNC depthFunction;
    bool depthTestEnabled;
    bool depthWriteEnabled;
    bool alphaTestEnabled;
    bool blendEnabled;
    DWORD colorWriteMask;
    D3DFILLMODE fillMode;
    bool stencilEnabled;
    DWORD polygonOffsetFactor;
};

const D3DCMPFUNC kDepthTestTable[4] = {
    D3DCMP_ALWAYS, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL};

// s_cullTable_30 = {0, 1, 3, 2}. Slot 0 holds 0, which is not a D3DCULL, so a block selecting
// it is refused rather than mapped to a neighbouring mode.
const D3DCULL kCullTable[4] = {
    D3DCULL_NONE /* slot 0 is invalid, see the check below */, D3DCULL_NONE, D3DCULL_CCW,
    D3DCULL_CW};

bool DecodeCull(std::uint32_t cullBits, D3DCULL* cull)
{
    if (cullBits >= 4u || cullBits == 0u)
        return false;
    *cull = kCullTable[cullBits];
    return true;
}

DecodedState DecodeStateBlock(std::uint32_t stateBits0, std::uint32_t stateBits1)
{
    DecodedState decoded{};

    auto unsupported = [&decoded](const std::string& reason) {
        if (decoded.supported)
        {
            decoded.supported = false;
            decoded.unsupportedReason = reason;
        }
    };

    // --- cull (stateBits0 bits 14-15) --------------------------------------
    {
        const std::uint32_t cullBits = (stateBits0 & 0xC000u) >> 14;
        if (!DecodeCull(cullBits, &decoded.cull))
            unsupported("GFXS0_CULL 0 indexes slot 0 of s_cullTable_30, which is not a D3DCULL");
    }

    // --- alpha test (stateBits0 bit 11) ------------------------------------
    decoded.alphaTestEnabled = (stateBits0 & 0x800u) == 0;
    if (decoded.alphaTestEnabled)
        unsupported("alpha test enabled: R_SetAlphaTestFunction's reference-alpha pairing is "
                    "not modelled here");

    // --- blend (stateBits0 bits 8-10) --------------------------------------
    decoded.blendEnabled = (stateBits0 & 0x700u) != 0;
    if (decoded.blendEnabled)
        unsupported("blending enabled: R_HW_SetBlend's factor and op tables are not modelled here");
    else if ((stateBits0 & 0x7000000u) != 0)
        unsupported("alpha blend op is set while the RGB blend op is disabled");

    // --- colour write (stateBits0 bits 27-28) ------------------------------
    decoded.colorWriteMask = 0;
    if ((stateBits0 & 0x8000000u) != 0)
        decoded.colorWriteMask |= D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
            | D3DCOLORWRITEENABLE_BLUE;
    if ((stateBits0 & 0x10000000u) != 0)
        decoded.colorWriteMask |= D3DCOLORWRITEENABLE_ALPHA;

    // --- fill mode (stateBits0 bit 31) -------------------------------------
    decoded.fillMode = (stateBits0 & 0x80000000u) != 0 ? D3DFILL_WIREFRAME : D3DFILL_SOLID;

    // --- depth (stateBits1 bits 0-3) ---------------------------------------
    decoded.depthWriteEnabled = (stateBits1 & 0x1u) != 0;
    decoded.depthTestEnabled = (stateBits1 & 0x2u) == 0;
    decoded.depthFunction = kDepthTestTable[(stateBits1 & 0xCu) >> 2];

    // --- polygon offset (stateBits1 bits 4-5) ------------------------------
    {
        const std::uint32_t offsetBits = (stateBits1 & 0x30u) >> 4;
        if (offsetBits == 3)
            unsupported("GFXS1_POLYGON_OFFSET_SHADOWMAP is not modelled here");
        else
            decoded.polygonOffsetFactor = offsetBits;
    }

    // --- stencil (stateBits1 bits 6-7) -------------------------------------
    decoded.stencilEnabled = (stateBits1 & 0xC0u) != 0;
    if (decoded.stencilEnabled)
        unsupported("stencil enabled: R_HW_EnableStencil's front/back op decode is not modelled here");

    return decoded;
}

// ---------------------------------------------------------------------------
// Sampler decode. R_DecodeSamplerState (src/gfx_d3d/r_state.cpp):
//
//   tableIndex = samplerState & 0x1F;
//   return s_decodeSamplerFilterState[tableIndex] | 0x1500000
//        | ((samplerState & 0x20) << 16) | ((samplerState & 0x40) << 17)
//        | ((samplerState & 0x80) << 18);
//
// s_decodeSamplerFilterState is filled by R_InitSamplerFilterState from r_texFilterMipMode,
// r_texFilterDisable and the device's anisotropy limits, none of which the bundle carries.
// r_texFilterDisable=1 pins every one of those inputs in r_state.cpp, so that branch is used
// and stated. The low byte of the decoded DWORD is D3DSAMP_MAXANISOTROPY, not a filter.
// ---------------------------------------------------------------------------
struct FilterTableInputs
{
    std::uint32_t mipFilterMode = 3;
    std::uint32_t anisotropicFilter = 4352u; // 0x1100
    std::uint32_t aniso2x = 1u;
    std::uint32_t aniso4x = 1u;
    std::uint32_t linearMippedAnisotropy = 1u;
    std::uint32_t linearMippedFilter = 4352u;
    std::uint32_t linearNonMippedFilter = 4352u;
};

const DWORD kMipFilterTable[4][3] = {
    {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR},
    {D3DTEXF_NONE, D3DTEXF_LINEAR, D3DTEXF_LINEAR},
    {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_POINT},
    {D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_NONE},
};

constexpr std::uint32_t kMaxSamplerTableIndex = 24;
constexpr std::uint32_t kSamplerAddressWrapAll = 0x1500000u;

DWORD BuildFilterTableEntry(std::uint32_t entryIndex, const FilterTableInputs& inputs)
{
    const std::uint32_t texFilter = entryIndex & 7u;
    const DWORD mipBits = kMipFilterTable[inputs.mipFilterMode & 3u][(entryIndex & 0x18u) >> 3] << 16;

    switch (texFilter)
    {
    case 2:
        return mipBits != 0 ? (mipBits | inputs.linearMippedAnisotropy | inputs.linearMippedFilter)
                            : (inputs.linearNonMippedFilter | 1u);
    case 3:
        return mipBits | inputs.aniso2x | inputs.anisotropicFilter;
    case 4:
        return mipBits | inputs.aniso4x | inputs.anisotropicFilter;
    default:
        return mipBits | 0x1101u;
    }
}

struct SamplerStates
{
    DWORD minFilter;
    DWORD magFilter;
    DWORD mipFilter;
    DWORD maxAnisotropy;
    DWORD addressU;
    DWORD addressV;
    DWORD addressW;
    std::uint32_t decodedDword;
};

bool DecodeSamplerState(std::uint8_t samplerState, const FilterTableInputs& inputs,
    SamplerStates* states, std::string* error)
{
    const std::uint32_t tableIndex = samplerState & 0x1Fu;
    if (tableIndex >= kMaxSamplerTableIndex)
    {
        *error = format("raw sampler 0x%02X selects filter-table index %u, outside the %u entries "
                        "R_InitSamplerFilterState fills",
            static_cast<unsigned>(samplerState), tableIndex, kMaxSamplerTableIndex);
        return false;
    }

    const std::uint32_t decoded = BuildFilterTableEntry(tableIndex, inputs) | kSamplerAddressWrapAll
        | (static_cast<std::uint32_t>(samplerState & 0x20u) << 16)
        | (static_cast<std::uint32_t>(samplerState & 0x40u) << 17)
        | (static_cast<std::uint32_t>(samplerState & 0x80u) << 18);

    states->decodedDword = decoded;
    states->maxAnisotropy = decoded & 0xFFu;
    states->minFilter = (decoded & 0xF00u) >> 8;
    states->magFilter = (decoded & 0xF000u) >> 12;
    states->mipFilter = (decoded & 0xF0000u) >> 16;
    states->addressU = (decoded & 0x300000u) >> 20;
    states->addressV = (decoded & 0xC00000u) >> 22;
    states->addressW = (decoded & 0x3000000u) >> 24;
    return true;
}

// ---------------------------------------------------------------------------
// The lit vertex declaration, transcribed independently of the loader.
//
// D3DDECLTYPE 3 = FLOAT4, 4 = D3DCOLOR, 5 = UBYTE4. The normal is UBYTE4 and NOT UBYTE4N: the
// shader multiplies the raw bytes by 1/127 and subtracts 1, so the declaration must hand it
// the 0..255 bytes, not a normalised 0..1 value.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kLitVertexStride = 44;

D3DVERTEXELEMENT9 MakeDeclarationElement(WORD stream, WORD offset, BYTE type, BYTE usage,
    BYTE usageIndex)
{
    D3DVERTEXELEMENT9 element;
    element.Stream = stream;
    element.Offset = offset;
    element.Type = type;
    element.Method = D3DDECLMETHOD_DEFAULT;
    element.Usage = usage;
    element.UsageIndex = usageIndex;
    return element;
}

// ---------------------------------------------------------------------------
// One lit vertex, as the CPU oracle reads it out of the exported slice.
// ---------------------------------------------------------------------------
struct LitVertex
{
    float position[4];
    float color[4]; // BGRA in memory, converted the way D3DDECLTYPE_D3DCOLOR converts it
    float texcoord[4];
    unsigned char normalBytes[4];
};

bool ReadLitVertex(const std::vector<std::uint8_t>& vertices, std::uint32_t index, LitVertex* out,
    std::string* error)
{
    const std::size_t offset = static_cast<std::size_t>(index) * kLitVertexStride;
    if (offset + kLitVertexStride > vertices.size())
    {
        *error = "vertex " + std::to_string(index) + " is outside the exported slice";
        return false;
    }
    const std::uint8_t* base = vertices.data() + offset;

    std::memcpy(out->position, base + 0, sizeof(out->position));
    std::memcpy(out->texcoord, base + 20, sizeof(out->texcoord));
    std::memcpy(out->normalBytes, base + 36, 4);

    // D3DDECLTYPE_D3DCOLOR: memory order B, G, R, A becomes R, G, B, A divided by 255.
    out->color[0] = static_cast<float>(base[18]) / 255.0f;
    out->color[1] = static_cast<float>(base[17]) / 255.0f;
    out->color[2] = static_cast<float>(base[16]) / 255.0f;
    out->color[3] = static_cast<float>(base[19]) / 255.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Image and texture containers.
// ---------------------------------------------------------------------------
struct Image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;  // width * height * 3
    std::vector<std::uint8_t> rgba; // width * height * 4
};

// A CPU-side RGBA8 image the oracle samples: the base colour map and the lightmap, in the
// orientation they are uploaded in.
struct CpuImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // row-major, top row first, width * height * 4

    const std::uint8_t* Texel(std::uint32_t x, std::uint32_t y) const
    {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
    }
};

bool WritePpm(const std::string& path, const Image& image, std::string* error)
{
    // The default output path is a fresh directory under /tmp, so create it rather than failing a
    // run whose only problem is that nobody made the folder yet.
    std::error_code ignored;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ignored);

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
        *error = "cannot open '" + path + "' for writing";
        return false;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
    const std::size_t written = std::fwrite(image.rgb.data(), 1, image.rgb.size(), file);
    const bool closed = std::fclose(file) == 0;
    if (written != image.rgb.size() || !closed)
    {
        *error = "short write to '" + path + "'";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Camera derivation from the referenced triangles.
//
// The exported slice is 8083 vertices but the surface uses 24 of them, so nothing may be
// derived from the whole slice: the bounds, the centre and the normal all come from the
// vertices the index buffer actually references.
// ---------------------------------------------------------------------------
struct Vec3
{
    float value[3];
};

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return Vec3{{a.value[0] - b.value[0], a.value[1] - b.value[1], a.value[2] - b.value[2]}};
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return Vec3{{a.value[1] * b.value[2] - a.value[2] * b.value[1],
        a.value[2] * b.value[0] - a.value[0] * b.value[2],
        a.value[0] * b.value[1] - a.value[1] * b.value[0]}};
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.value[0] * b.value[0] + a.value[1] * b.value[1] + a.value[2] * b.value[2];
}

float Length(const Vec3& value)
{
    return std::sqrt(Dot(value, value));
}

bool Normalize(const Vec3& value, Vec3* normalized)
{
    const float length = Length(value);
    if (!(length > 0.0f))
        return false;
    normalized->value[0] = value.value[0] / length;
    normalized->value[1] = value.value[1] / length;
    normalized->value[2] = value.value[2] / length;
    return true;
}

float Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

std::vector<std::uint16_t> ReadIndices(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint16_t> indices(bytes.size() / 2u);
    if (!indices.empty())
        std::memcpy(indices.data(), bytes.data(), indices.size() * sizeof(std::uint16_t));
    return indices;
}

// Camera basis in the engine's CoD convention (forward = axis[0], left = axis[1],
// up = axis[2], left == up x forward), normalised and re-orthogonalised so the context
// builder receives a basis that is unit length and orthogonal to within float rounding.
bool BuildCameraBasis(const Vec3& forwardIn, world_surface_context::CameraInputs* camera,
    std::string* error)
{
    Vec3 forward{};
    if (!Normalize(forwardIn, &forward))
    {
        *error = "the derived view direction has zero length";
        return false;
    }

    Vec3 referenceUp{{0.0f, 0.0f, 1.0f}};
    if (std::fabs(Dot(forward, referenceUp)) > 0.999f)
        referenceUp = Vec3{{0.0f, 1.0f, 0.0f}};

    Vec3 left{};
    if (!Normalize(Cross(referenceUp, forward), &left))
    {
        *error = "cannot build a left vector from the derived view direction";
        return false;
    }

    Vec3 up = Cross(forward, left);
    if (!Normalize(up, &up))
    {
        *error = "cannot build an up vector from the derived view direction";
        return false;
    }

    left = Cross(up, forward);
    if (!Normalize(left, &left))
    {
        *error = "cannot re-derive the left vector";
        return false;
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        camera->forward[axis] = forward.value[axis];
        camera->left[axis] = left.value[axis];
        camera->up[axis] = up.value[axis];
    }
    return true;
}

struct CameraDerivation
{
    Vec3 eye{};
    Vec3 target{};
    Vec3 center{};       // centre of the referenced vertices
    Vec3 normal{};       // area-weighted normal of the referenced triangles
    float distance = 0.0f;
    float fitDistance = 0.0f;      // distance that fits the whole referenced set in the frame
    float texelsPerPixel = 0.0f;   // densest triangle's colour-map texels per pixel at `distance`
    float zNear = 1.0f;
    bool derivedFromGeometry = true;
    float referencedRadius = 0.0f;
};

// Signed screen area of one projected triangle, in the convention
// (b - a) x (c - a): positive means clockwise on a target whose rows run down. Defined below,
// together with the projection it is used with.

// Places the eye along the referenced triangles' area-weighted normal, far enough that the
// whole referenced set fits the frustum.
bool DeriveCamera(const world_lit_bundle::Bundle& bundle, float tanHalfFov,
    std::uint32_t baseWidth, std::uint32_t baseHeight, CameraDerivation* out, std::string* error)
{
    const std::vector<std::uint16_t> indices = ReadIndices(bundle.indices);
    if (indices.empty())
    {
        *error = "the exported index slice is empty";
        return false;
    }

    Vec3 minimum{{1e30f, 1e30f, 1e30f}};
    Vec3 maximum{{-1e30f, -1e30f, -1e30f}};
    Vec3 normalSum{{0.0f, 0.0f, 0.0f}};
    std::size_t referenced = 0;

    std::vector<LitVertex> vertices(indices.size());
    for (std::size_t index = 0; index < indices.size(); ++index)
    {
        if (!ReadLitVertex(bundle.vertices, indices[index], &vertices[index], error))
            return false;
        const LitVertex& vertex = vertices[index];
        for (int axis = 0; axis < 3; ++axis)
        {
            minimum.value[axis] = std::min(minimum.value[axis], vertex.position[axis]);
            maximum.value[axis] = std::max(maximum.value[axis], vertex.position[axis]);
        }
    }

    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
    {
        const Vec3 a{{vertices[triangle + 0].position[0], vertices[triangle + 0].position[1],
            vertices[triangle + 0].position[2]}};
        const Vec3 b{{vertices[triangle + 1].position[0], vertices[triangle + 1].position[1],
            vertices[triangle + 1].position[2]}};
        const Vec3 c{{vertices[triangle + 2].position[0], vertices[triangle + 2].position[1],
            vertices[triangle + 2].position[2]}};
        // Area-weighted: the cross product's length is twice the triangle's area, so a large
        // triangle contributes its own weight without a second pass.
        const Vec3 cross = Cross(Subtract(b, a), Subtract(c, a));
        normalSum.value[0] += cross.value[0];
        normalSum.value[1] += cross.value[1];
        normalSum.value[2] += cross.value[2];
        ++referenced;
    }

    if (referenced == 0)
    {
        *error = "the exported index slice holds no complete triangle";
        return false;
    }

    Vec3 center{{(minimum.value[0] + maximum.value[0]) * 0.5f,
        (minimum.value[1] + maximum.value[1]) * 0.5f,
        (minimum.value[2] + maximum.value[2]) * 0.5f}};

    float radius = 0.0f;
    for (const LitVertex& vertex : vertices)
    {
        const Vec3 point{{vertex.position[0], vertex.position[1], vertex.position[2]}};
        radius = std::max(radius, Length(Subtract(point, center)));
    }
    if (!(radius > 0.0f))
    {
        *error = "the referenced vertices have zero radius";
        return false;
    }

    Vec3 normal{};
    if (!Normalize(normalSum, &normal))
    {
        *error = "the referenced triangles' area-weighted normal has zero length";
        return false;
    }

    // Fit the referenced vertices as they project, not as a bounding sphere. A sphere fit wastes
    // the frame on a surface that is long in one direction and thin in the other, which is what a
    // wire lattice is, and a surface that fills a tenth of the target has too few interior pixels
    // to meet the sample floor. The extents below are the referenced vertices' half-width along
    // the camera's left and up axes and their half-depth along the view direction, so the eye
    // distance is whatever puts them at 90% of the frame.
    world_surface_context::CameraInputs basis{};
    if (!BuildCameraBasis(normal, &basis, error))
        return false;
    const Vec3 forward{{basis.forward[0], basis.forward[1], basis.forward[2]}};
    const Vec3 left{{basis.left[0], basis.left[1], basis.left[2]}};
    const Vec3 up{{basis.up[0], basis.up[1], basis.up[2]}};

    const float frameFill = 0.9f;
    const float effectiveTan = tanHalfFov * frameFill;
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    float halfDepth = 0.0f;
    for (const LitVertex& vertex : vertices)
    {
        const Vec3 point{{vertex.position[0], vertex.position[1], vertex.position[2]}};
        const Vec3 offset = Subtract(point, center);
        halfWidth = std::max(halfWidth, std::fabs(Dot(offset, left)));
        halfHeight = std::max(halfHeight, std::fabs(Dot(offset, up)));
        halfDepth = std::max(halfDepth, std::fabs(Dot(offset, forward)));
    }
    if (!(halfWidth > 0.0f) && !(halfHeight > 0.0f))
    {
        *error = "the referenced vertices have no screen extent";
        return false;
    }

    const float fitDistance = std::max(halfWidth, halfHeight) / effectiveTan + halfDepth;

    // Keep the geometry-derived fit distance fixed. Texture travel is reported
    // later as a diagnostic and does not adjust this camera.
    const float distance = fitDistance;
    (void)baseWidth;
    (void)baseHeight;

    out->target = center;
    out->center = center;
    out->normal = normal;
    out->distance = distance;
    out->fitDistance = fitDistance;
    out->zNear = std::max(radius * 0.05f, 1.0f);
    out->derivedFromGeometry = true;
    out->referencedRadius = radius;
    return true;
}

// ---------------------------------------------------------------------------
// The CPU lit evaluator. This is the audited equivalent of the packaged pair; see the file
// header for the formula and where each term comes from.
// ---------------------------------------------------------------------------
struct LitConstants
{
    // c0..c3 and c4..c7 are the transposed matrices exactly as uploaded.
    float viewProjection[4][4];
    float world[4][4];
    float fog[4];
    float fogColor[4];
};

struct VsOutput
{
    float clip[4];
    float color[4];
    float texcoord[4];
    float normalWorld[3];
    float fog;
    float eyeDistance;
};

VsOutput EvaluateVertex(const LitVertex& vertex, const LitConstants& constants)
{
    VsOutput out{};

    // mad r0, v0.xyzx, c9.xxxy, c9.yyyx -> (x, y, z, 1): position.w is discarded.
    const float q[4] = {vertex.position[0], vertex.position[1], vertex.position[2], 1.0f};

    // dp4 r1.x..w, r0, c4..c7: register c4+j holds row j of the transposed world matrix, so
    // each output component is the dot product of the input with that whole register.
    float eyeRelative[4];
    for (int component = 0; component < 4; ++component)
    {
        float sum = 0.0f;
        for (int i = 0; i < 4; ++i)
            sum += q[i] * constants.world[component][i];
        eyeRelative[component] = sum;
    }

    // dp4 o0.x..w, r1, c0..c3, same convention.
    for (int component = 0; component < 4; ++component)
    {
        float sum = 0.0f;
        for (int i = 0; i < 4; ++i)
            sum += eyeRelative[i] * constants.viewProjection[component][i];
        out.clip[component] = sum;
    }

    // dp3 r0.x, r1, r1 then rsq and rcp: rcp(rsq(d2)) is the eye distance.
    const float distanceSquared = eyeRelative[0] * eyeRelative[0] + eyeRelative[1] * eyeRelative[1]
        + eyeRelative[2] * eyeRelative[2];
    float rsq = 0.0f;
    if (distanceSquared > 0.0f)
        rsq = 1.0f / std::sqrt(distanceSquared);
    float distance = 0.0f;
    if (rsq > 0.0f)
        distance = 1.0f / rsq;
    out.eyeDistance = distance;

    // Fog: mad r0.x, r0.x, c21.z, c21.w; mul by c9.z (log2 e); exp_sat.
    const float fogExponent = (distance * constants.fog[2] + constants.fog[3]) * FloatBits(kLog2EBits);
    out.fog = Clamp(std::exp2(fogExponent), 0.0f, 1.0f);

    // Packed normal: t.xyz = byte * (1/127) - 1; t.w = byte.w * (1/255) + 192/255; then
    // n = t.xyz * t.w, and the world normal is n dotted with the world matrix rows.
    const float scaleX = FloatBits(kNormalScaleXBits);
    const float scaleW = FloatBits(kNormalScaleWBits);
    const float biasW = FloatBits(kNormalBiasWBits);
    float packed[3];
    for (int axis = 0; axis < 3; ++axis)
        packed[axis] = static_cast<float>(vertex.normalBytes[axis]) * scaleX - 1.0f;
    const float packedW =
        static_cast<float>(vertex.normalBytes[3]) * scaleW + biasW;

    const float n[3] = {packed[0] * packedW, packed[1] * packedW, packed[2] * packedW};
    for (int component = 0; component < 3; ++component)
        out.normalWorld[component] = n[0] * constants.world[component][0]
            + n[1] * constants.world[component][1] + n[2] * constants.world[component][2];

    // mov o1, v1; mov o2, v2.
    for (int channel = 0; channel < 4; ++channel)
    {
        out.color[channel] = vertex.color[channel];
        out.texcoord[channel] = vertex.texcoord[channel];
    }
    return out;
}

// Point sampling with the address mode the decoded sampler DWORD says. The lightmap's U and V
// clamp; the base map wraps on all three axes.
std::uint32_t WrapIndex(int value, std::uint32_t size)
{
    if (size == 0)
        return 0;
    const int sizeSigned = static_cast<int>(size);
    int wrapped = value % sizeSigned;
    if (wrapped < 0)
        wrapped += sizeSigned;
    return static_cast<std::uint32_t>(wrapped);
}

std::uint32_t ClampIndex(int value, std::uint32_t size)
{
    if (size == 0)
        return 0;
    if (value < 0)
        return 0;
    const std::uint32_t unsignedValue = static_cast<std::uint32_t>(value);
    return unsignedValue >= size ? size - 1u : unsignedValue;
}

const std::uint8_t* SamplePointWrap(const CpuImage& image, float u, float v, int offsetU = 0,
    int offsetV = 0)
{
    const std::uint32_t x = WrapIndex(static_cast<int>(std::floor(u * static_cast<float>(image.width)))
            + offsetU,
        image.width);
    const std::uint32_t y = WrapIndex(static_cast<int>(std::floor(v * static_cast<float>(image.height)))
            + offsetV,
        image.height);
    return image.Texel(x, y);
}

const std::uint8_t* SamplePointClamp(const CpuImage& image, float u, float v, int offsetU = 0,
    int offsetV = 0)
{
    const std::uint32_t x = ClampIndex(static_cast<int>(std::floor(u * static_cast<float>(image.width)))
            + offsetU,
        image.width);
    const std::uint32_t y = ClampIndex(static_cast<int>(std::floor(v * static_cast<float>(image.height)))
            + offsetV,
        image.height);
    return image.Texel(x, y);
}

struct PixelPrediction
{
    float rgb[3];
    float alpha;
    // Intermediate terms kept so the report can show what the lightmap contributed.
    float lightmapRgb[3];
    float baseRgb[3];
    float sampleA[3]; // the lower lightmap sample, before the weight
    float sampleB[3]; // the upper lightmap sample, before the weight
};

// Evaluates the pixel program at one shading point. The two texel offsets exist for diagnosis
// only: they let a mismatch report ask whether the disagreement is one texel of sampling position
// rather than a difference in the arithmetic, and they are zero everywhere in the prediction.
PixelPrediction EvaluatePixel(const float texcoord[4], const float color[4], float fog,
    const CpuImage& base, const CpuImage& lightmap, const LitConstants& constants,
    int baseOffsetU = 0, int baseOffsetV = 0, int lightmapOffsetU = 0, int lightmapOffsetV = 0)
{
    PixelPrediction out{};

    // mul r0.xy, c1, v1.zwzw  -> (lmU, 0.5 * lmV); texld_pp r0, r0, s3
    const float aU = texcoord[2];
    const float aV = texcoord[3] * kLightmapVScale;
    const std::uint8_t* sampleA =
        SamplePointClamp(lightmap, aU, aV, lightmapOffsetU, lightmapOffsetV);

    // mad r1.zw, v1, c1.xyxy, c1.xyzy -> (lmU, 0.5 * lmV + 0.5); texld_pp r2, r1.zwzw, s3
    const float bU = texcoord[2];
    const float bV = texcoord[3] * kLightmapVScale + kLightmapVScale;
    const std::uint8_t* sampleB =
        SamplePointClamp(lightmap, bU, bV, lightmapOffsetU, lightmapOffsetV);

    // The CpuImage already holds RGBA: LightmapCpuImage did the payload's BGRA-to-RGBA conversion
    // once, and there is no gamma or exponent decode anywhere.
    const float aRgb[3] = {static_cast<float>(sampleA[0]) / 255.0f,
        static_cast<float>(sampleA[1]) / 255.0f, static_cast<float>(sampleA[2]) / 255.0f};
    const float bRgb[3] = {static_cast<float>(sampleB[0]) / 255.0f,
        static_cast<float>(sampleB[1]) / 255.0f, static_cast<float>(sampleB[2]) / 255.0f};
    const float aAlpha = static_cast<float>(sampleA[3]) / 255.0f;
    const float bAlpha = static_cast<float>(sampleB[3]) / 255.0f;

    // mad_pp r1.xy, r1, c2, c2.zwzw; dp2add r0.w, r1, r1, c1.x; rsq_sat.
    const float x = aAlpha * FloatBits(kAlphaScale0Bits) + FloatBits(kAlphaBias0Bits);
    const float y = bAlpha * FloatBits(kAlphaScale1Bits) + FloatBits(kAlphaBias1Bits);
    const float sum = x * x + y * y + 1.0f;
    const float weight = sum > 0.0f ? Clamp(1.0f / std::sqrt(sum), 0.0f, 1.0f) : 0.0f;

    // mad_pp r0.xyz, r2, r0.wwww, r0 -> B.rgb * weight + A.rgb, deliberately NOT saturated.
    for (int channel = 0; channel < 3; ++channel)
    {
        out.lightmapRgb[channel] = bRgb[channel] * weight + aRgb[channel];
        out.sampleA[channel] = aRgb[channel];
        out.sampleB[channel] = bRgb[channel];
    }

    // texld_pp r1, v1, s0 ; mul_pp r1.xyz, r1, v0.
    // The IWI decoder hands back RGBA, unlike the lightmap payload, so these bytes are read in
    // the order the decoder produced them.
    const std::uint8_t* baseTexel =
        SamplePointWrap(base, texcoord[0], texcoord[1], baseOffsetU, baseOffsetV);
    const float baseRgb[3] = {static_cast<float>(baseTexel[0]) / 255.0f,
        static_cast<float>(baseTexel[1]) / 255.0f, static_cast<float>(baseTexel[2]) / 255.0f};
    for (int channel = 0; channel < 3; ++channel)
        out.baseRgb[channel] = baseRgb[channel] * color[channel];

    // mad r0.xyz, r1, r0, -c0 ; mad oC0.xyz, v2.wwww, r0, c0.
    for (int channel = 0; channel < 3; ++channel)
    {
        const float reflected =
            out.baseRgb[channel] * out.lightmapRgb[channel] - constants.fogColor[channel];
        out.rgb[channel] = fog * reflected + constants.fogColor[channel];
    }

    // mov oC0.w, c1.x: output alpha is exactly 1, independent of the base texel and COLOR0.
    out.alpha = 1.0f;
    return out;
}

std::uint8_t QuantizeUnorm8(float value)
{
    const float clamped = Clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

// ---------------------------------------------------------------------------
// Rasterisation for the CPU prediction.
//
// The comparison uses predeclared interior pixels, so the oracle only has to answer "what
// does the program compute at this pixel centre", not "what is the whole image".
// ---------------------------------------------------------------------------
struct ProjectedVertex
{
    float screenX;
    float screenY;
    float reciprocalW;
    float clipZ; // in [0, w]; the depth the rasteriser compares
    bool visible;
};

bool ProjectVertex(const VsOutput& vertex, std::uint32_t width, std::uint32_t height,
    ProjectedVertex* out)
{
    if (!(vertex.clip[3] > 0.0f))
    {
        out->visible = false;
        return false;
    }
    // Ordinary D3D9 viewport transform, no half-pixel term: the same mapping the unlit oracle
    // uses (world_surface_oracle.h, fragmentSampleOffset = 0). An integer coordinate is a pixel
    // index, and the sample points below are integers, so the pixel the CPU evaluates is named by
    // the same number the readback is indexed with. Adding or subtracting half a pixel here does
    // not move the GPU by that amount; it only moves the CPU's evaluation point away from the
    // pixel it is compared against, which is a convention error rather than a correction.
    if (!world_lit_bundle::sampling::ProjectToScreen(vertex.clip, width, height, &out->screenX,
            &out->screenY))
    {
        out->visible = false;
        return false;
    }
    out->reciprocalW = 1.0f / vertex.clip[3];
    out->clipZ = vertex.clip[2];
    out->visible = true;
    return true;
}

// Signed screen area of one projected triangle: positive means clockwise on a target whose
// rows run down, which is the winding this profile's recorded state block draws.
float ScreenArea(const ProjectedVertex& a, const ProjectedVertex& b, const ProjectedVertex& c)
{
    return (b.screenX - a.screenX) * (c.screenY - a.screenY)
        - (b.screenY - a.screenY) * (c.screenX - a.screenX);
}

LitConstants MakeConstants(const world_surface_context::Context& context)
{
    LitConstants constants{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            constants.viewProjection[row][column] = context.vsTransposeViewProjection.rows[row][column];
            constants.world[row][column] = context.vsTransposeWorld.rows[row][column];
        }
    }
    for (int channel = 0; channel < 4; ++channel)
    {
        constants.fog[channel] = context.vsFog[channel];
        constants.fogColor[channel] = context.psFogColor[channel];
    }
    return constants;
}

// How many of the surface's triangles would be drawn from this camera: all three corners in
// front of the eye and the screen winding on the side the recorded cull mode keeps.
std::size_t CountFrontFacing(const world_lit_bundle::Bundle& bundle, const LitConstants& constants,
    float frontSign)
{
    const std::vector<std::uint16_t> indices = ReadIndices(bundle.indices);
    std::size_t count = 0;
    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
    {
        ProjectedVertex projected[3];
        bool usable = true;
        for (int corner = 0; corner < 3; ++corner)
        {
            LitVertex vertex{};
            std::string ignored;
            if (!ReadLitVertex(bundle.vertices, indices[triangle + corner], &vertex, &ignored))
                return count;
            const VsOutput evaluated = EvaluateVertex(vertex, constants);
            if (!ProjectVertex(evaluated, kRenderWidth, kRenderHeight, &projected[corner]))
            {
                usable = false;
                break;
            }
        }
        if (!usable)
            continue;
        const float area = ScreenArea(projected[0], projected[1], projected[2]);
        if (area * frontSign > 0.0f)
            ++count;
    }
    return count;
}

// Point sampling is only unambiguous away from a texel boundary. The base map is a wire pattern
// whose neighbouring texels differ sharply, so its coordinate needs a wide margin; the lightmap
// is smooth, so it only needs to be off the edge far enough to exclude a rounding tie.
constexpr float kLightmapTexelMargin = 0.02f;

// The one base-texel margin this file uses. A candidate whose interpolated coordinate is within
// this fraction of a texel boundary can floor to either neighbour, and this material's colour map
// is a wire pattern, so those candidates are excluded rather than compared. The margin is fixed
// before the run: there is no ladder of attempts and no search for the loosest value that still
// produces a match, because that search would be a tolerance chosen by its own result.
constexpr float kBaseTexelMargin = 0.35f;
// Per-triangle candidate cap. Larger than the sample cap so the round-robin still has room on each
// triangle, small enough that the collection pass stays bounded on a dense surface.
constexpr std::size_t kMaxCandidatesPerTriangle = 64;

struct SamplePoint
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t triangle = 0;
    std::uint8_t predicted[4] = {0, 0, 0, 0};
    float predictedRgb[3] = {0.0f, 0.0f, 0.0f};
    // The two lightmap samples and the base texel the prediction used, so a disagreement can be
    // read as "which term differs" instead of "the images differ".
    float sampleA[3] = {0.0f, 0.0f, 0.0f};
    float sampleB[3] = {0.0f, 0.0f, 0.0f};
    float baseTexel[3] = {0.0f, 0.0f, 0.0f};
    float uv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float fog = 1.0f;
    // Measured, not assumed: the worst-direction texel travel of one pixel at this point and the
    // smallest distance from the sampled coordinates to a texel boundary. Their ratio is the
    // statement about whether the sample can be reproduced at all, and it is reported whether or
    // not it is comfortable.
    float texelTravel = 0.0f;
    float texelMargin = 0.0f;
    bool baseNonZero = false;
    bool lightmapNonZero = false;
};

// Every referenced triangle's projection, computed once so a candidate pixel can be tested
// against all of them. Coverage by more than one triangle is not an error in the bundle: a wire
// surface's quads overlap on screen, and the depth test decides which one is drawn. It does mean
// the pixel cannot be predicted from one triangle's interpolants, so it is not used.
struct ProjectedTriangle
{
    bool usable = false;
    float screen[3][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    ProjectedVertex vertex[3];
};

bool ProjectAllTriangles(const world_lit_bundle::Bundle& bundle, const LitConstants& constants,
    std::vector<ProjectedTriangle>* triangles, std::string* error)
{
    const std::vector<std::uint16_t> indices = ReadIndices(bundle.indices);
    const std::size_t triangleCount = indices.size() / 3u;
    triangles->assign(triangleCount, {});

    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        ProjectedTriangle& out = (*triangles)[triangle];
        bool usable = true;
        for (int corner = 0; corner < 3; ++corner)
        {
            LitVertex vertex{};
            if (!ReadLitVertex(bundle.vertices, indices[triangle * 3u + static_cast<std::size_t>(corner)],
                    &vertex, error))
                return false;
            const VsOutput evaluated = EvaluateVertex(vertex, constants);
            if (!ProjectVertex(evaluated, kRenderWidth, kRenderHeight, &out.vertex[corner]))
            {
                usable = false;
                break;
            }
            out.screen[corner][0] = out.vertex[corner].screenX;
            out.screen[corner][1] = out.vertex[corner].screenY;
        }
        out.usable = usable;
    }
    return true;
}

// True when this pixel can only have come from one triangle, and it is this one. `margin` keeps
// the point away from the triangle's own edges, so the fill rule and a rounding difference in the
// projection cannot decide coverage; the uniqueness test keeps it away from every other
// triangle's interior, where the depth test would decide which interpolants the shader saw. That
// is stricter than modelling the depth test, and deliberately so: a lattice's triangles overlap,
// and a pixel covered by two of them is exactly the pixel whose prediction would depend on the
// rasteriser's tie-breaking rather than on the programs being compared.
bool UniquelyInside(const std::vector<ProjectedTriangle>& triangles, std::size_t own, float px,
    float py, float margin)
{
    if (!triangles[own].usable || !world_lit_bundle::sampling::InsideTriangle(triangles[own].screen, px, py, margin))
        return false;

    for (std::size_t other = 0; other < triangles.size(); ++other)
    {
        if (other == own || !triangles[other].usable)
            continue;
        if (world_lit_bundle::sampling::InsideTriangle(triangles[other].screen, px, py, 0.0f))
            return false;
    }
    return true;
}

// Picks interior pixels whose prediction is usable, grouped by triangle so the selection can
// spread across the surface instead of exhausting the first triangle it meets.
bool CollectCandidates(const world_lit_bundle::Bundle& bundle, const LitConstants& constants,
    const CpuImage& base, const CpuImage& lightmap,
    std::vector<std::vector<SamplePoint>>* perTriangle, std::string* error)
{
    const std::vector<std::uint16_t> indices = ReadIndices(bundle.indices);
    const std::size_t triangleCount = indices.size() / 3u;

    std::vector<ProjectedTriangle> allTriangles;
    if (!ProjectAllTriangles(bundle, constants, &allTriangles, error))
        return false;

    perTriangle->assign(triangleCount, {});
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        VsOutput outputs[3];
        ProjectedVertex projected[3];
        if (!allTriangles[triangle].usable)
            continue;
        for (int corner = 0; corner < 3; ++corner)
        {
            LitVertex vertex{};
            if (!ReadLitVertex(bundle.vertices, indices[triangle * 3u + corner], &vertex, error))
                return false;
            outputs[corner] = EvaluateVertex(vertex, constants);
            projected[corner] = allTriangles[triangle].vertex[corner];
        }

        float screen[3][2];
        for (int corner = 0; corner < 3; ++corner)
        {
            screen[corner][0] = projected[corner].screenX;
            screen[corner][1] = projected[corner].screenY;
        }

        const float minimumX = std::min({screen[0][0], screen[1][0], screen[2][0]});
        const float maximumX = std::max({screen[0][0], screen[1][0], screen[2][0]});
        const float minimumY = std::min({screen[0][1], screen[1][1], screen[2][1]});
        const float maximumY = std::max({screen[0][1], screen[1][1], screen[2][1]});

        const int startX = std::max(0, static_cast<int>(std::floor(minimumX)));
        const int endX = std::min(static_cast<int>(kRenderWidth) - 1, static_cast<int>(std::ceil(maximumX)));
        const int startY = std::max(0, static_cast<int>(std::floor(minimumY)));
        const int endY = std::min(static_cast<int>(kRenderHeight) - 1, static_cast<int>(std::ceil(maximumY)));

        std::vector<SamplePoint>& candidates = (*perTriangle)[triangle];
        for (int y = startY; y <= endY && candidates.size() < kMaxCandidatesPerTriangle; ++y)
        {
            for (int x = startX; x <= endX && candidates.size() < kMaxCandidatesPerTriangle; ++x)
            {
                // Integer pixel indices. The centre of pixel x is the integer x in the
                // projection's coordinates, so this evaluates where the rasteriser evaluates and
                // names the same index the readback is sampled at.
                const float px = static_cast<float>(x);
                const float py = static_cast<float>(y);
                if (!UniquelyInside(allTriangles, triangle, px, py, kInteriorMarginPixels))
                    continue;

                // Barycentrics in screen space, then perspective correction through the
                // vertices' 1/w, which is what D3D9 interpolates.
                float barycentric[3];
                {
                    const float area = (screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1])
                        - (screen[2][0] - screen[0][0]) * (screen[1][1] - screen[0][1]);
                    if (std::fabs(area) < 1.0e-6f)
                        continue;
                    const float weight1 = ((px - screen[0][0]) * (screen[2][1] - screen[0][1])
                            - (screen[2][0] - screen[0][0]) * (py - screen[0][1]))
                        / area;
                    const float weight2 = ((screen[1][0] - screen[0][0]) * (py - screen[0][1])
                            - (px - screen[0][0]) * (screen[1][1] - screen[0][1]))
                        / area;
                    barycentric[1] = weight1;
                    barycentric[2] = weight2;
                    barycentric[0] = 1.0f - weight1 - weight2;
                }

                float sum = 0.0f;
                float scaled[3];
                for (int corner = 0; corner < 3; ++corner)
                {
                    scaled[corner] = barycentric[corner] * projected[corner].reciprocalW;
                    sum += scaled[corner];
                }
                if (!(sum > 0.0f))
                    continue;

                float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                float texcoord[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                float fog = 0.0f;
                for (int corner = 0; corner < 3; ++corner)
                {
                    const float weight = scaled[corner] / sum;
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        color[channel] += outputs[corner].color[channel] * weight;
                        texcoord[channel] += outputs[corner].texcoord[channel] * weight;
                    }
                    fog += outputs[corner].fog * weight;
                }

                const PixelPrediction prediction =
                    EvaluatePixel(texcoord, color, fog, base, lightmap, constants);

                // Skip candidates whose point-sampled coordinates sit near a texel edge. The
                // margins are constants of this file: one fixed set, applied to every run.
                if (world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[0], base.width)
                        < kBaseTexelMargin
                    || world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[1], base.height)
                        < kBaseTexelMargin
                    || world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[2], lightmap.width)
                        < kLightmapTexelMargin
                    || world_lit_bundle::sampling::TexelBoundaryMargin(
                           texcoord[3] * kLightmapVScale, lightmap.height)
                        < kLightmapTexelMargin
                    || world_lit_bundle::sampling::TexelBoundaryMargin(
                           texcoord[3] * kLightmapVScale + kLightmapVScale, lightmap.height)
                        < kLightmapTexelMargin)
                    continue;

                SamplePoint point{};
                point.x = static_cast<std::uint32_t>(x);
                point.y = static_cast<std::uint32_t>(y);
                point.triangle = static_cast<std::uint32_t>(triangle);

                // The measured travel of the base coordinate, in texels per pixel, from the same
                // perspective interpolation the prediction uses.
                {
                    float texelU[3];
                    float texelV[3];
                    float reciprocalW[3];
                    for (int corner = 0; corner < 3; ++corner)
                    {
                        texelU[corner] = outputs[corner].texcoord[0]
                            * static_cast<float>(base.width);
                        texelV[corner] = outputs[corner].texcoord[1]
                            * static_cast<float>(base.height);
                        reciprocalW[corner] = projected[corner].reciprocalW;
                    }
                    float travel = 0.0f;
                    if (world_lit_bundle::sampling::MaxTexelTravelPerPixel(screen, reciprocalW,
                            texelU, texelV, px, py, &travel))
                        point.texelTravel = travel;
                }
                point.texelMargin =
                    std::min({world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[0], base.width),
                        world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[1], base.height),
                        world_lit_bundle::sampling::TexelBoundaryMargin(texcoord[2], lightmap.width),
                        world_lit_bundle::sampling::TexelBoundaryMargin(
                            texcoord[3] * kLightmapVScale, lightmap.height),
                        world_lit_bundle::sampling::TexelBoundaryMargin(
                            texcoord[3] * kLightmapVScale + kLightmapVScale, lightmap.height)});
                for (int channel = 0; channel < 4; ++channel)
                {
                    point.uv[channel] = texcoord[channel];
                    point.color[channel] = color[channel];
                }
                point.fog = fog;
                for (int channel = 0; channel < 3; ++channel)
                {
                    point.predictedRgb[channel] = prediction.rgb[channel];
                    point.predicted[channel] = QuantizeUnorm8(prediction.rgb[channel]);
                    point.sampleA[channel] = prediction.sampleA[channel];
                    point.sampleB[channel] = prediction.sampleB[channel];
                    point.baseTexel[channel] = prediction.baseRgb[channel];
                    point.baseNonZero = point.baseNonZero || prediction.baseRgb[channel] > 0.0f;
                    point.lightmapNonZero =
                        point.lightmapNonZero || prediction.lightmapRgb[channel] > 0.0f;
                }
                point.predicted[3] = QuantizeUnorm8(prediction.alpha);
                candidates.push_back(point);
            }
        }
    }

    for (const std::vector<SamplePoint>& candidates : *perTriangle)
        if (!candidates.empty())
            return true;
    *error = "no interior pixel of the referenced triangles could be selected: the camera does "
             "not place the surface inside the target, or every candidate was closer than the "
             "interior margin to an edge or than the texel margin to a texel boundary";
    return false;
}

// Chooses the reported points by walking the triangles in order and taking one candidate from
// each in turn, so the set spans the surface rather than the first triangle that had room. The
// choice is a function of the bundle, the camera and the two images only: nothing about the
// readback influences which pixels are compared.
bool SelectSamplePoints(const world_lit_bundle::Bundle& bundle, const LitConstants& constants,
    const CpuImage& base, const CpuImage& lightmap, std::vector<SamplePoint>* points,
    std::string* error)
{
    std::vector<std::vector<SamplePoint>> perTriangle;
    if (!CollectCandidates(bundle, constants, base, lightmap, &perTriangle, error))
        return false;

    std::vector<std::size_t> taken(perTriangle.size(), 0);
    bool progressed = true;
    while (points->size() < kMaxSamplePoints && progressed)
    {
        progressed = false;
        for (std::size_t triangle = 0; triangle < perTriangle.size(); ++triangle)
        {
            if (points->size() >= kMaxSamplePoints)
                break;
            if (taken[triangle] >= perTriangle[triangle].size())
                continue;
            points->push_back(perTriangle[triangle][taken[triangle]]);
            ++taken[triangle];
            progressed = true;
        }
    }

    if (points->size() < kRequiredSamplePoints)
    {
        std::size_t trianglesWithCandidates = 0;
        for (const std::vector<SamplePoint>& candidates : perTriangle)
            if (!candidates.empty())
                ++trianglesWithCandidates;
        *error = "only " + std::to_string(points->size())
            + " interior pixel(s) at least " + format("%.2f", kBaseTexelMargin)
            + " of a texel from a boundary were found across " + std::to_string(trianglesWithCandidates)
            + " triangle(s); this profile compares at least "
            + std::to_string(kRequiredSamplePoints)
            + " and does not lower that floor to whatever the camera produced. Move the camera with "
              "--camera-eye/--camera-target so more of the surface faces it";
        return false;
    }
    return true;
}


// ---------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------
struct Options
{
    std::string bundlePath;
    std::string outputPath = "/tmp/kisak-world-lit-replay/world-lit.ppm";
    std::string reportPath;

    bool textureSupplied = false;
    std::string texturePath;

    bool cameraEyeSupplied = false;
    Vec3 cameraEye{};
    bool cameraTargetSupplied = false;
    Vec3 cameraTarget{};
    float tanHalfFov = 0.57735026f; // tan(30 deg)

    // Code 42, the previous fog colour. The bundle carries no scene value, and with fog
    // disabled the term cancels arithmetically (fog == 1) but not bit-exactly, so the caller
    // states it instead of this file inventing one.
    float fogColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool fogColorSupplied = false;

    // Controls. Both perturb the bytes that are uploaded, so the CPU prediction sees the same
    // perturbation and the comparison stays a comparison.
    int lightmapPerturbation = 32;
    int basePerturbation = 32;
    bool cpuOnly = false;

    // Optional GPU name filter, applied to the enumerated Vulkan devices. Empty selects the first
    // non-CPU device. There is no default name: a filter nobody set would be a claim this file
    // made up about the host.
    std::string expectGpu;
};

void printUsage(std::FILE* out)
{
    std::fprintf(out,
        "usage: world_lit_replay --bundle PATH --texture PATH --fog-color R,G,B,A [options]\n"
        "\n"
        "  --bundle PATH            schema-1 lit world surface bundle (required)\n"
        "  --texture PATH           the prepared IWI the pass binds to sampler 0 (required).\n"
        "                           Decoded in this process by the repository's own decoder.\n"
        "  --fog-color R,G,B,A      value of constant code 42, the previous fog colour\n"
        "                           (required). The bundle does not carry it.\n"
        "  --output PATH            PPM P6 of the base run (default %s)\n"
        "  --report PATH            also write the text report here\n"
        "  --camera-eye X,Y,Z       explicit eye, with --camera-target\n"
        "  --camera-target X,Y,Z    explicit look-at point, with --camera-eye\n"
        "  --tan-half-fov VALUE     projection half-angle tangent (default 0.57735026)\n"
        "  --lightmap-perturb N     delta added to the lightmap's RGB bytes for the lightmap\n"
        "                           control (default 32)\n"
        "  --base-perturb N         delta added to the base texture's RGB bytes for the base\n"
        "                           control (default 32)\n"
        "  --cpu-only               run the loader and the CPU prediction without a device\n"
        "  --expect-gpu NAME        require the enumerated Vulkan device name to contain NAME\n"
        "\n"
        "There is no option that skips the controls. A run that cannot show the image\n"
        "responding to the lightmap and to the base texture, and matching the CPU prediction\n"
        "at every predeclared point, is not a pass.\n"
        "\n"
        "Constant 40 is absent by contract: the lit pass has no material-colour argument.\n"
        "\n"
        "Exit codes: 0 pass, 1 failure, 2 usage, 3 bundle invalid, 4 state unsupported,\n"
        "            5 device unavailable, 6 prediction mismatch, 7 not observable,\n"
        "            8 texture invalid.\n",
        "/tmp/kisak-world-lit-replay/world-lit.ppm");
}

bool parseInt(const char* text, int* value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 0);
    if (!end || *end != '\0')
        return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool parseFloat(const char* text, float* value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (!end || *end != '\0')
        return false;
    *value = static_cast<float>(parsed);
    return true;
}

// Four comma-separated numbers, field by field, so "1,2,3" and "1,2,3,4,5" are refused rather
// than truncated.
bool parseVec4(const char* text, float out[4])
{
    if (!text || !*text)
        return false;
    const char* cursor = text;
    for (int component = 0; component < 4; ++component)
    {
        char* end = nullptr;
        const double parsed = std::strtod(cursor, &end);
        if (!end || end == cursor || !std::isfinite(parsed))
            return false;
        out[component] = static_cast<float>(parsed);
        if (component < 3)
        {
            if (*end != ',')
                return false;
            cursor = end + 1;
        }
        else if (*end != '\0')
        {
            return false;
        }
    }
    return true;
}

bool parseVec3(const char* text, Vec3* value)
{
    if (!text || !*text)
        return false;
    const char* cursor = text;
    for (int component = 0; component < 3; ++component)
    {
        char* end = nullptr;
        const double parsed = std::strtod(cursor, &end);
        if (!end || end == cursor || !std::isfinite(parsed))
            return false;
        value->value[component] = static_cast<float>(parsed);
        if (component < 2)
        {
            if (*end != ',')
                return false;
            cursor = end + 1;
        }
        else if (*end != '\0')
        {
            return false;
        }
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& options, bool* helpRequested, std::string* error)
{
    *helpRequested = false;

    for (int index = 1; index < argc; ++index)
    {
        std::string arg = argv[index];
        std::string value;
        const std::string::size_type equals = arg.find('=');
        if (equals != std::string::npos)
        {
            value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }

        if (arg == "--help" || arg == "-h")
        {
            *helpRequested = true;
            return true;
        }
        if (arg == "--skip-controls")
        {
            *error = "--skip-controls does not exist: a control set that can be switched off "
                     "would be switched off, and the run would still print PASS";
            return false;
        }

        if (value.empty() && arg != "--cpu-only")
        {
            if (index + 1 >= argc)
            {
                *error = "option '" + arg + "' needs a value";
                return false;
            }
            value = argv[++index];
        }

        if (arg == "--bundle")
            options.bundlePath = value;
        else if (arg == "--output")
            options.outputPath = value;
        else if (arg == "--report")
            options.reportPath = value;
        else if (arg == "--texture")
        {
            options.texturePath = value;
            options.textureSupplied = true;
        }
        else if (arg == "--fog-color")
        {
            if (!parseVec4(value.c_str(), options.fogColor))
            {
                *error = "--fog-color is not four comma-separated numbers";
                return false;
            }
            options.fogColorSupplied = true;
        }
        else if (arg == "--camera-eye")
        {
            if (!parseVec3(value.c_str(), &options.cameraEye))
            {
                *error = "--camera-eye is not three comma-separated numbers";
                return false;
            }
            options.cameraEyeSupplied = true;
        }
        else if (arg == "--camera-target")
        {
            if (!parseVec3(value.c_str(), &options.cameraTarget))
            {
                *error = "--camera-target is not three comma-separated numbers";
                return false;
            }
            options.cameraTargetSupplied = true;
        }
        else if (arg == "--tan-half-fov")
        {
            if (!parseFloat(value.c_str(), &options.tanHalfFov))
            {
                *error = "--tan-half-fov is not a number";
                return false;
            }
        }
        else if (arg == "--lightmap-perturb")
        {
            if (!parseInt(value.c_str(), &options.lightmapPerturbation))
            {
                *error = "--lightmap-perturb is not an integer";
                return false;
            }
        }
        else if (arg == "--base-perturb")
        {
            if (!parseInt(value.c_str(), &options.basePerturbation))
            {
                *error = "--base-perturb is not an integer";
                return false;
            }
        }
        else if (arg == "--cpu-only")
            options.cpuOnly = true;
        else if (arg == "--expect-gpu")
        {
            if (value.empty())
            {
                *error = "--expect-gpu needs a device name substring";
                return false;
            }
            options.expectGpu = value;
        }
        else
        {
            *error = "unknown option '" + arg + "'";
            return false;
        }
    }

    if (options.cameraEyeSupplied != options.cameraTargetSupplied)
    {
        *error = "--camera-eye and --camera-target are used together or not at all";
        return false;
    }
    if (options.bundlePath.empty())
    {
        *error = "--bundle is required";
        return false;
    }
    if (!options.fogColorSupplied)
    {
        *error = "--fog-color is required: code 42 is scene state the bundle does not carry, and "
                 "this file will not invent it";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Device and resources.
// ---------------------------------------------------------------------------
struct Resources
{
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DIndexBuffer9* indexBuffer = nullptr;
    IDirect3DTexture9* baseTexture = nullptr;
    IDirect3DTexture9* perturbedBaseTexture = nullptr;
    IDirect3DTexture9* lightmapTexture = nullptr;
    IDirect3DTexture9* perturbedLightmapTexture = nullptr;
    IDirect3DSurface9* renderTarget = nullptr;
    IDirect3DSurface9* depthStencil = nullptr;
};

// Releases every device object this replay owns, in either direction. Used before a device Reset,
// which invalidates every D3DPOOL_DEFAULT object, and again at exit.
void ReleaseResources(Resources* resources)
{
    if (resources->baseTexture)
        resources->baseTexture->Release();
    if (resources->perturbedBaseTexture)
        resources->perturbedBaseTexture->Release();
    if (resources->lightmapTexture)
        resources->lightmapTexture->Release();
    if (resources->perturbedLightmapTexture)
        resources->perturbedLightmapTexture->Release();
    if (resources->renderTarget)
        resources->renderTarget->Release();
    if (resources->depthStencil)
        resources->depthStencil->Release();
    if (resources->vertexBuffer)
        resources->vertexBuffer->Release();
    if (resources->indexBuffer)
        resources->indexBuffer->Release();
    if (resources->declaration)
        resources->declaration->Release();
    if (resources->vertexShader)
        resources->vertexShader->Release();
    if (resources->pixelShader)
        resources->pixelShader->Release();
    *resources = Resources{};
}

// ---------------------------------------------------------------------------
// Device preflight.
//
// D3D9's adapter identity through DXVK is a spoof: the log of a passing run names an AMD Radeon
// RX 6700 XT while the device actually created is an Intel integrated GPU. So the adapter string
// is printed for the record but never trusted, and the statement "this ran on a GPU" is made by
// enumerating the Vulkan physical devices and refusing a CPU device — the same rule the standalone
// d3d9_sdl3_smoke preflight applies, reduced here to the part this replay needs: enumerate, skip
// VK_PHYSICAL_DEVICE_TYPE_CPU, take the first survivor, and pin DXVK to that device by name so the
// translator cannot quietly choose the llvmpipe device the enumeration refused.
//
// There is no Wine or HAL fallback on the other side of this check: D3DDEVTYPE_REF and software
// vertex processing are both refused after creation, because a software rasteriser reproducing the
// prediction would say nothing about the GPU path this bundle is exported for.
// ---------------------------------------------------------------------------
struct VulkanDeviceInfo
{
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
};

const char* PhysicalDeviceTypeName(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    default: return "unknown";
    }
}

struct VulkanProbe
{
    bool loaderReady = false;
    std::vector<VulkanDeviceInfo> devices;
    std::string error;
};

// Creates a throwaway Vulkan instance through the loader SDL already opened, enumerates the
// physical devices, and destroys the instance before DXVK creates its own.
VulkanProbe ProbeVulkanDevices()
{
    VulkanProbe probe;

    const SDL_FunctionPointer rawGetProc = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!rawGetProc)
    {
        probe.error = "SDL_Vulkan_GetVkGetInstanceProcAddr returned null";
        return probe;
    }

    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(rawGetProc);
    const auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getProc(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance)
    {
        probe.error = "vkCreateInstance is not exported by the Vulkan loader";
        return probe;
    }

    Uint32 extensionCount = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!extensions)
    {
        probe.error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
        return probe;
    }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "kisak-world-lit-replay";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    applicationInfo.pEngineName = "none";
    applicationInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = extensionCount;
    createInfo.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&createInfo, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE)
    {
        probe.error = "vkCreateInstance failed";
        return probe;
    }
    probe.loaderReady = true;

    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        getProc(instance, "vkEnumeratePhysicalDevices"));
    const auto getProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getProc(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(getProc(instance, "vkDestroyInstance"));

    if (enumerate && getProperties)
    {
        std::uint32_t deviceCount = 0;
        if (enumerate(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0)
        {
            std::vector<VkPhysicalDevice> handles(deviceCount);
            if (enumerate(instance, &deviceCount, handles.data()) == VK_SUCCESS)
            {
                for (std::uint32_t index = 0; index < deviceCount; ++index)
                {
                    VkPhysicalDeviceProperties properties{};
                    getProperties(handles[index], &properties);
                    VulkanDeviceInfo info;
                    info.name = std::string(properties.deviceName);
                    info.type = properties.deviceType;
                    probe.devices.push_back(info);
                }
            }
        }
    }

    if (destroyInstance)
        destroyInstance(instance, nullptr);

    if (probe.devices.empty())
        probe.error = "the Vulkan loader enumerated no physical devices";
    return probe;
}

// A pure selection over the enumerated devices: the first non-CPU device, or the first non-CPU
// device whose name matches the requested filter. A list holding only CPU devices selects nothing,
// which is the whole point of the function.
bool SelectVulkanDevice(const std::vector<VulkanDeviceInfo>& devices, const std::string& wanted,
    std::size_t* selected, std::string* reason)
{
    *selected = devices.size();
    bool sawCpu = false;
    for (std::size_t index = 0; index < devices.size(); ++index)
    {
        if (devices[index].type == VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            sawCpu = true;
            continue;
        }
        if (!wanted.empty() && devices[index].name.find(wanted) == std::string::npos)
            continue;
        *selected = index;
        return true;
    }

    if (!wanted.empty())
        *reason = "no non-CPU Vulkan device matches '" + wanted + "'";
    else if (sawCpu)
        *reason = "every enumerated Vulkan device is VK_PHYSICAL_DEVICE_TYPE_CPU; a software "
                  "rasteriser is not a GPU and this run is not a pass";
    else
        *reason = "no Vulkan physical device was enumerated";
    return false;
}


bool CreateDevice(HWND hwnd, IDirect3D9** d3dOut, IDirect3DDevice9** deviceOut,
    D3DPRESENT_PARAMETERS* paramsOut, std::string* error)
{
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        *error = "Direct3DCreate9 returned null";
        return false;
    }

    D3DPRESENT_PARAMETERS params;
    std::memset(&params, 0, sizeof(params));
    params.BackBufferWidth = kRenderWidth;
    params.BackBufferHeight = kRenderHeight;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = hwnd;
    params.Windowed = TRUE;
    params.EnableAutoDepthStencil = FALSE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
    if (FAILED(hr) || !device)
    {
        if (device)
        {
            device->Release();
            device = nullptr;
        }
        *error = "CreateDevice(HAL, hardware vertex processing) failed, hr=" + hrText(hr)
            + ". This profile does not retry with software vertex processing or D3DDEVTYPE_REF: a "
              "run whose shaders were executed by a substitute pipeline is not evidence about the "
              "GPU";
        d3d->Release();
        return false;
    }

    // Creation succeeded, so now prove what was created: the device must report the HAL type and
    // hardware vertex processing, not a software device that accepted the request.
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation)) || creation.DeviceType != D3DDEVTYPE_HAL
        || (creation.BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) == 0)
    {
        *error = "the created device reports type " + std::to_string(static_cast<int>(creation.DeviceType))
            + " with behavior flags 0x" + format("%08X", static_cast<unsigned>(creation.BehaviorFlags))
            + "; HAL with hardware vertex processing is required";
        device->Release();
        d3d->Release();
        return false;
    }

    *d3dOut = d3d;
    *deviceOut = device;
    *paramsOut = params;
    return true;
}

bool CreateShaderObjects(const world_lit_bundle::Bundle& bundle, IDirect3DDevice9* device,
    Resources* resources, std::string* error)
{
    HRESULT hr = device->CreateVertexShader(
        reinterpret_cast<const DWORD*>(bundle.vertexShader.payload.data()), &resources->vertexShader);
    if (FAILED(hr) || !resources->vertexShader)
    {
        *error = "CreateVertexShader failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->CreatePixelShader(
        reinterpret_cast<const DWORD*>(bundle.pixelShader.payload.data()), &resources->pixelShader);
    if (FAILED(hr) || !resources->pixelShader)
    {
        *error = "CreatePixelShader failed, hr=" + hrText(hr);
        return false;
    }

    // The lit declaration: four elements, the normal UBYTE4 at 36 is the one the unlit profile
    // does not have.
    D3DVERTEXELEMENT9 elements[5];
    elements[0] = MakeDeclarationElement(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    elements[1] = MakeDeclarationElement(0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
    elements[2] = MakeDeclarationElement(0, 20, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, 0);
    elements[3] = MakeDeclarationElement(0, 36, D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_NORMAL, 0);
    elements[4] = D3DDECL_END();

    hr = device->CreateVertexDeclaration(elements, &resources->declaration);
    if (FAILED(hr) || !resources->declaration)
    {
        *error = "CreateVertexDeclaration failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

bool CreateGeometry(const world_lit_bundle::Bundle& bundle, IDirect3DDevice9* device,
    Resources* resources, std::string* error)
{
    HRESULT hr = device->CreateVertexBuffer(static_cast<UINT>(bundle.vertices.size()),
        D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &resources->vertexBuffer, nullptr);
    if (FAILED(hr) || !resources->vertexBuffer)
    {
        *error = "CreateVertexBuffer failed, hr=" + hrText(hr);
        return false;
    }
    void* locked = nullptr;
    hr = resources->vertexBuffer->Lock(0, static_cast<UINT>(bundle.vertices.size()), &locked,
        D3DLOCK_DISCARD);
    if (FAILED(hr) || !locked)
    {
        *error = "vertex buffer Lock failed, hr=" + hrText(hr);
        return false;
    }
    std::memcpy(locked, bundle.vertices.data(), bundle.vertices.size());
    resources->vertexBuffer->Unlock();

    hr = device->CreateIndexBuffer(static_cast<UINT>(bundle.indices.size()), D3DUSAGE_DYNAMIC,
        D3DFMT_INDEX16, D3DPOOL_DEFAULT, &resources->indexBuffer, nullptr);
    if (FAILED(hr) || !resources->indexBuffer)
    {
        *error = "CreateIndexBuffer failed, hr=" + hrText(hr);
        return false;
    }
    locked = nullptr;
    hr = resources->indexBuffer->Lock(0, static_cast<UINT>(bundle.indices.size()), &locked,
        D3DLOCK_DISCARD);
    if (FAILED(hr) || !locked)
    {
        *error = "index buffer Lock failed, hr=" + hrText(hr);
        return false;
    }
    std::memcpy(locked, bundle.indices.data(), bundle.indices.size());
    resources->indexBuffer->Unlock();
    return true;
}

// Uploads the decoded IWI mip chain to sampler 0. The decoder's pixels are RGBA; the texture
// is A8R8G8B8, whose memory order is B, G, R, A, so each texel is written as
// (a << 24) | (r << 16) | (g << 8) | b and the rows are written at the surface's own pitch.
bool UploadBaseTexture(const kisak::window::IwiMipChain& chain, IDirect3DDevice9* device,
    IDirect3DTexture9** textureOut, std::uint32_t* widthOut, std::uint32_t* heightOut,
    std::string* error)
{
    if (chain.empty())
    {
        *error = "the decoder returned an empty mip chain";
        return false;
    }

    const kisak::window::PixelExtent baseExtent = chain.front().Extent();
    const std::uint32_t width = static_cast<std::uint32_t>(baseExtent.width);
    const std::uint32_t height = static_cast<std::uint32_t>(baseExtent.height);
    const std::uint32_t levels = static_cast<std::uint32_t>(chain.size());

    HRESULT hr = device->CreateTexture(width, height, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        textureOut, nullptr);
    if (FAILED(hr) || !*textureOut)
    {
        *error = "CreateTexture(base) failed, hr=" + hrText(hr);
        return false;
    }

    for (std::uint32_t level = 0; level < levels; ++level)
    {
        const kisak::window::PixelBufferView view = chain[level].View();
        const std::uint32_t levelWidth = static_cast<std::uint32_t>(view.width);
        const std::uint32_t levelHeight = static_cast<std::uint32_t>(view.height);

        D3DLOCKED_RECT locked{};
        hr = (*textureOut)->LockRect(level, &locked, nullptr, 0);
        if (FAILED(hr))
        {
            *error = "base texture LockRect(level " + std::to_string(level) + ") failed, hr="
                + hrText(hr);
            return false;
        }

        for (std::uint32_t row = 0; row < levelHeight; ++row)
        {
            auto* destination = reinterpret_cast<std::uint32_t*>(
                static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch);
            const std::uint8_t* source = view.bytes.data()
                + static_cast<std::size_t>(row) * levelWidth * 4u;
            for (std::uint32_t column = 0; column < levelWidth; ++column)
            {
                const std::uint32_t red = source[column * 4u + 0u];
                const std::uint32_t green = source[column * 4u + 1u];
                const std::uint32_t blue = source[column * 4u + 2u];
                const std::uint32_t alpha = source[column * 4u + 3u];
                destination[column] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        (*textureOut)->UnlockRect(level);
    }

    *widthOut = width;
    *heightOut = height;
    return true;
}

// Uploads the captured lightmap: one level, A8R8G8B8, no flip, no mip generation, no
// re-encode. The bytes are the fast file's payload in the order the shader's two vertical
// samples expect.
bool UploadLightmap(const std::vector<std::uint8_t>& payload, std::uint32_t width,
    std::uint32_t height, IDirect3DDevice9* device, IDirect3DTexture9** textureOut,
    std::string* error)
{
    HRESULT hr = device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        textureOut, nullptr);
    if (FAILED(hr) || !*textureOut)
    {
        *error = "CreateTexture(lightmap) failed, hr=" + hrText(hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = (*textureOut)->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr))
    {
        *error = "lightmap LockRect failed, hr=" + hrText(hr);
        return false;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    for (std::uint32_t row = 0; row < height; ++row)
    {
        std::memcpy(static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch,
            payload.data() + static_cast<std::size_t>(row) * rowBytes, rowBytes);
    }
    (*textureOut)->UnlockRect(0);
    return true;
}

bool CreateTargets(IDirect3DDevice9* device, Resources* resources, std::string* error)
{
    HRESULT hr = device->CreateRenderTarget(kRenderWidth, kRenderHeight, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &resources->renderTarget, nullptr);
    if (FAILED(hr) || !resources->renderTarget)
    {
        *error = "CreateRenderTarget failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->CreateDepthStencilSurface(kRenderWidth, kRenderHeight, D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &resources->depthStencil, nullptr);
    if (FAILED(hr) || !resources->depthStencil)
    {
        *error = "CreateDepthStencilSurface failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

bool ApplyStateBlock(IDirect3DDevice9* device, const DecodedState& state, std::string* error)
{
    struct RenderState
    {
        D3DRENDERSTATETYPE state;
        DWORD value;
    };

    const RenderState states[] = {
        {D3DRS_CULLMODE, static_cast<DWORD>(state.cull)},
        {D3DRS_ZENABLE, state.depthTestEnabled ? D3DZB_TRUE : D3DZB_FALSE},
        {D3DRS_ZFUNC, static_cast<DWORD>(state.depthFunction)},
        {D3DRS_ZWRITEENABLE, state.depthWriteEnabled ? DWORD(TRUE) : DWORD(FALSE)},
        {D3DRS_ALPHATESTENABLE, state.alphaTestEnabled ? DWORD(TRUE) : DWORD(FALSE)},
        {D3DRS_ALPHABLENDENABLE, state.blendEnabled ? DWORD(TRUE) : DWORD(FALSE)},
        {D3DRS_COLORWRITEENABLE, state.colorWriteMask},
        {D3DRS_FILLMODE, static_cast<DWORD>(state.fillMode)},
        {D3DRS_STENCILENABLE, state.stencilEnabled ? DWORD(TRUE) : DWORD(FALSE)},
        {D3DRS_LIGHTING, FALSE},
    };

    for (const RenderState& entry : states)
    {
        const HRESULT hr = device->SetRenderState(entry.state, entry.value);
        if (FAILED(hr))
        {
            *error = "SetRenderState(" + std::to_string(static_cast<int>(entry.state)) + ", "
                + std::to_string(entry.value) + ") failed, hr=" + hrText(hr);
            return false;
        }
    }

    if (state.polygonOffsetFactor != 0)
    {
        *error = "polygon offset preset " + std::to_string(state.polygonOffsetFactor)
            + " is decoded but not applied: the engine's preset bias values live in the "
              "renderer, not in the recorded block";
        return false;
    }
    return true;
}

bool ApplySampler(IDirect3DDevice9* device, DWORD samplerIndex, const SamplerStates& states,
    std::string* error)
{
    const struct
    {
        D3DSAMPLERSTATETYPE state;
        DWORD value;
    } entries[] = {
        {D3DSAMP_MINFILTER, states.minFilter},
        {D3DSAMP_MAGFILTER, states.magFilter},
        {D3DSAMP_MIPFILTER, states.mipFilter},
        {D3DSAMP_MAXANISOTROPY, states.maxAnisotropy},
        {D3DSAMP_ADDRESSU, states.addressU},
        {D3DSAMP_ADDRESSV, states.addressV},
        {D3DSAMP_ADDRESSW, states.addressW},
    };

    for (const auto& entry : entries)
    {
        const HRESULT hr = device->SetSamplerState(samplerIndex, entry.state, entry.value);
        if (FAILED(hr))
        {
            *error = "SetSamplerState(" + std::to_string(samplerIndex) + ", "
                + std::to_string(static_cast<int>(entry.state)) + ") failed, hr=" + hrText(hr);
            return false;
        }
    }

    // sRGB conversion is stated test context: neither state has a bit in the engine's recorded
    // words, and a sampler with SRGBTEXTURE set would convert a texel before the shader saw
    // it, contradicting every value the CPU oracle predicts.
    HRESULT hr = device->SetSamplerState(samplerIndex, D3DSAMP_SRGBTEXTURE, 0);
    if (FAILED(hr))
    {
        *error = "SetSamplerState(SRGBTEXTURE) failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

// Uploads the four constants this profile supplies. Code 40 is named here only to say that it
// is not written: PS c1 is a shader-local def in this program, and the loader refused any
// bundle that asked for a material-colour argument.
bool UploadConstants(IDirect3DDevice9* device, const world_surface_context::Context& context,
    std::string* error)
{
    HRESULT hr = device->SetVertexShaderConstantF(world_lit_bundle::kVsDestTransposeViewProjection,
        &context.vsTransposeViewProjection.rows[0][0], 4);
    if (FAILED(hr))
    {
        *error = "SetVertexShaderConstantF(c0..c3) failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->SetVertexShaderConstantF(world_lit_bundle::kVsDestTransposeWorld,
        &context.vsTransposeWorld.rows[0][0], 4);
    if (FAILED(hr))
    {
        *error = "SetVertexShaderConstantF(c4..c7) failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->SetVertexShaderConstantF(world_lit_bundle::kVsDestFog, context.vsFog, 1);
    if (FAILED(hr))
    {
        *error = "SetVertexShaderConstantF(c21) failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->SetPixelShaderConstantF(world_lit_bundle::kPsDestFogColor, context.psFogColor, 1);
    if (FAILED(hr))
    {
        *error = "SetPixelShaderConstantF(c0) failed, hr=" + hrText(hr);
        return false;
    }

    // Deliberately absent: SetPixelShaderConstantF(world_lit_bundle::kPsDestMaterialColor, ...).
    // The lit pass has no code-40 argument, and writing c1 here would override the program's
    // own def with a value this file invented.
    return true;
}

struct FrameInputs
{
    bool issueDraw = true;
    IDirect3DTexture9* lightmap = nullptr;
    IDirect3DTexture9* baseTexture = nullptr;
};

bool RenderOnce(IDirect3DDevice9* device, const world_lit_bundle::Bundle& bundle,
    const world_surface_context::Context& context, Resources* resources,
    const SamplerStates& baseSampler, const SamplerStates& lightmapSampler,
    const FrameInputs& inputs, Image* image, std::string* error)
{
    HRESULT hr = device->SetRenderTarget(0, resources->renderTarget);
    if (FAILED(hr))
    {
        *error = "SetRenderTarget failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetDepthStencilSurface(resources->depthStencil);
    if (FAILED(hr) || FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                             D3DCOLOR_ARGB(255, kClearRed, kClearGreen, kClearBlue), 1.0f, 0)))
    {
        *error = "Clear failed or the depth surface could not be bound";
        return false;
    }

    hr = device->BeginScene();
    if (FAILED(hr))
    {
        *error = "BeginScene failed, hr=" + hrText(hr);
        return false;
    }

    if (FAILED(device->SetVertexDeclaration(resources->declaration))
        || FAILED(device->SetStreamSource(0, resources->vertexBuffer, 0, kLitVertexStride))
        || FAILED(device->SetIndices(resources->indexBuffer))
        || FAILED(device->SetVertexShader(resources->vertexShader))
        || FAILED(device->SetPixelShader(resources->pixelShader)))
    {
        device->EndScene();
        *error = "binding the pipeline objects failed";
        return false;
    }

    if (!ApplyStateBlock(device, DecodeStateBlock(bundle.stateBits0, bundle.stateBits1), error))
    {
        device->EndScene();
        return false;
    }
    if (!ApplySampler(device, 0, baseSampler, error)
        || !ApplySampler(device, world_lit_bundle::kLightmapStage, lightmapSampler, error))
    {
        device->EndScene();
        return false;
    }
    if (!UploadConstants(device, context, error))
    {
        device->EndScene();
        return false;
    }

    hr = device->SetTexture(0, inputs.baseTexture);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetTexture(0) failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetTexture(world_lit_bundle::kLightmapStage, inputs.lightmap);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetTexture(3) failed, hr=" + hrText(hr);
        return false;
    }

    // issueDraw=false is the skip-draw control: every binding, state and constant above is
    // identical and only the draw call is missing, so the readback must be the clear colour.
    if (inputs.issueDraw)
    {
        hr = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, bundle.vertexCount, 0,
            bundle.triCount);
        if (FAILED(hr))
        {
            device->EndScene();
            *error = "DrawIndexedPrimitive failed, hr=" + hrText(hr);
            return false;
        }
    }

    hr = device->EndScene();
    if (FAILED(hr))
    {
        *error = "EndScene failed, hr=" + hrText(hr);
        return false;
    }

    // Readback through a SYSTEMMEM offscreen surface: the render target itself is not lockable,
    // and this path also proves the target holds the drawn frame rather than an untouched clear.
    IDirect3DSurface9* systemSurface = nullptr;
    hr = device->CreateOffscreenPlainSurface(kRenderWidth, kRenderHeight, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &systemSurface, nullptr);
    if (FAILED(hr) || !systemSurface)
    {
        *error = "CreateOffscreenPlainSurface failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->GetRenderTargetData(resources->renderTarget, systemSurface);
    if (FAILED(hr))
    {
        systemSurface->Release();
        *error = "GetRenderTargetData failed, hr=" + hrText(hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = systemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        systemSurface->Release();
        *error = "surface LockRect failed, hr=" + hrText(hr);
        return false;
    }

    image->width = kRenderWidth;
    image->height = kRenderHeight;
    image->rgb.assign(static_cast<std::size_t>(kRenderWidth) * kRenderHeight * 3u, 0);
    image->rgba.assign(static_cast<std::size_t>(kRenderWidth) * kRenderHeight * 4u, 0);
    for (std::uint32_t row = 0; row < kRenderHeight; ++row)
    {
        const auto* source = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch);
        for (std::uint32_t column = 0; column < kRenderWidth; ++column)
        {
            const std::uint32_t texel = source[column];
            const std::size_t rgbOffset = (static_cast<std::size_t>(row) * kRenderWidth + column) * 3u;
            image->rgb[rgbOffset + 0] = static_cast<std::uint8_t>((texel >> 16) & 0xFFu);
            image->rgb[rgbOffset + 1] = static_cast<std::uint8_t>((texel >> 8) & 0xFFu);
            image->rgb[rgbOffset + 2] = static_cast<std::uint8_t>(texel & 0xFFu);

            const std::size_t rgbaOffset = (static_cast<std::size_t>(row) * kRenderWidth + column) * 4u;
            image->rgba[rgbaOffset + 0] = image->rgb[rgbOffset + 0];
            image->rgba[rgbaOffset + 1] = image->rgb[rgbOffset + 1];
            image->rgba[rgbaOffset + 2] = image->rgb[rgbOffset + 2];
            image->rgba[rgbaOffset + 3] = static_cast<std::uint8_t>((texel >> 24) & 0xFFu);
        }
    }

    systemSurface->UnlockRect();
    systemSurface->Release();
    return true;
}

// ---------------------------------------------------------------------------
// Measurements.
// ---------------------------------------------------------------------------
struct ImageStats
{
    std::size_t covered = 0;   // pixels that are not the clear colour
    std::size_t nonBlack = 0;  // covered pixels with any channel above zero
    std::size_t opaque = 0;    // covered pixels with alpha 255
    std::uint8_t maximumRed = 0;
    std::uint8_t maximumGreen = 0;
    std::uint8_t maximumBlue = 0;
};

bool IsClearPixel(const Image& image, std::size_t offset)
{
    return image.rgb[offset + 0] == kClearRed && image.rgb[offset + 1] == kClearGreen
        && image.rgb[offset + 2] == kClearBlue;
}

ImageStats Measure(const Image& image)
{
    ImageStats stats{};
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(image.width) * image.height; ++pixel)
    {
        const std::size_t rgbOffset = pixel * 3u;
        if (IsClearPixel(image, rgbOffset))
            continue;
        ++stats.covered;
        const std::uint8_t red = image.rgb[rgbOffset + 0];
        const std::uint8_t green = image.rgb[rgbOffset + 1];
        const std::uint8_t blue = image.rgb[rgbOffset + 2];
        if (red != 0 || green != 0 || blue != 0)
            ++stats.nonBlack;
        if (image.rgba[pixel * 4u + 3u] == 255)
            ++stats.opaque;
        stats.maximumRed = std::max(stats.maximumRed, red);
        stats.maximumGreen = std::max(stats.maximumGreen, green);
        stats.maximumBlue = std::max(stats.maximumBlue, blue);
    }
    return stats;
}

// Reads level 0 of the uploaded base texture back and compares it with the source rows, row by
// row. The prediction samples the source bytes; the GPU samples whatever the upload actually
// wrote. If those two differ by even one row, the comparison is between two different textures and
// no amount of arithmetic will reconcile them — so the upload is checked rather than assumed.
struct UploadCheck
{
    bool readable = false;
    std::size_t identicalRows = 0;
    std::size_t rows = 0;
    int matchingShift = 0; // source row that reproduces destination row 0
    bool anyShiftMatchesAll = false;
    std::string detail;
};

UploadCheck CheckBaseUpload(IDirect3DTexture9* texture,
    const kisak::window::PixelBufferView& source)
{
    UploadCheck check;
    if (!texture)
    {
        check.detail = "no texture";
        return check;
    }

    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)))
    {
        check.detail = "LockRect(0) failed";
        return check;
    }
    check.readable = true;

    const int width = source.width;
    const int height = source.height;
    check.rows = static_cast<std::size_t>(height);

    auto expected = [&](int row, int column) {
        const std::uint8_t* texel =
            source.bytes.data() + (static_cast<std::size_t>(row) * width + column) * 4u;
        return (static_cast<std::uint32_t>(texel[3]) << 24)
            | (static_cast<std::uint32_t>(texel[0]) << 16)
            | (static_cast<std::uint32_t>(texel[1]) << 8) | static_cast<std::uint32_t>(texel[2]);
    };
    auto actual = [&](int row, int column) {
        const auto* pixels = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch);
        return pixels[column];
    };

    for (int row = 0; row < height; ++row)
    {
        bool same = true;
        for (int column = 0; column < width && same; ++column)
            same = expected(row, column) == actual(row, column);
        if (same)
            ++check.identicalRows;
    }

    // Which single source-row offset reproduces every destination row, if any. This is the
    // difference between "the upload is right and the GPU samples elsewhere" and "the texture the
    // GPU has is not the texture the prediction was built from".
    for (int shift = -2; shift <= 2 && !check.anyShiftMatchesAll; ++shift)
    {
        bool all = true;
        for (int row = 0; row < height && all; ++row)
        {
            const int sourceRow = row + shift;
            if (sourceRow < 0 || sourceRow >= height)
            {
                all = false;
                break;
            }
            for (int column = 0; column < width; ++column)
            {
                if (expected(sourceRow, column) != actual(row, column))
                {
                    all = false;
                    break;
                }
            }
        }
        if (all)
        {
            check.anyShiftMatchesAll = true;
            check.matchingShift = shift;
        }
    }

    texture->UnlockRect(0);
    check.detail = std::to_string(check.identicalRows) + " of " + std::to_string(check.rows)
        + " rows identical"
        + (check.anyShiftMatchesAll
                ? ", all rows reproduced with a source offset of " + std::to_string(check.matchingShift)
                : "");
    return check;
}


// Runs the prediction under single-texel shifts of the base sample and of the two lightmap
// samples, and names the shifts (if any) that reproduce the readback at this pixel. A match here
// says the arithmetic is right and the sampled texel is wrong, which is a different defect from a
// formula that disagrees; the report prints it next to the mismatch instead of leaving the reader
// to guess which of the two it is.
std::uint8_t SampleChannel(const Image& image, std::uint32_t column, std::uint32_t row, int channel);

// Counts, over all sample points, how many are reproduced by the prediction as written and by the
// same prediction under a single-texel shift of the base sample. Printed as one line so a
// systematic sampling offset is visible as a number instead of being inferred from ten mismatches.
struct ShiftTally
{
    std::size_t unshifted = 0;
    std::size_t vMinusOne = 0;
    std::size_t uMinusOne = 0;
    std::size_t uPlusOne = 0;
};

ShiftTally TallyBaseShifts(const std::vector<SamplePoint>& points, const CpuImage& base,
    const CpuImage& lightmap, const LitConstants& constants, const Image& image)
{
    ShiftTally tally{};
    for (const SamplePoint& point : points)
    {
        auto reproduces = [&](int du, int dv) {
            const PixelPrediction prediction = EvaluatePixel(point.uv, point.color, point.fog, base,
                lightmap, constants, du, dv, 0, 0);
            for (int channel = 0; channel < 3; ++channel)
                if (std::abs(static_cast<int>(SampleChannel(image, point.x, point.y, channel))
                        - static_cast<int>(QuantizeUnorm8(prediction.rgb[channel])))
                    > kPerChannelTolerance)
                    return false;
            return true;
        };
        if (reproduces(0, 0))
            ++tally.unshifted;
        if (reproduces(0, -1))
            ++tally.vMinusOne;
        if (reproduces(-1, 0))
            ++tally.uMinusOne;
        if (reproduces(1, 0))
            ++tally.uPlusOne;
    }
    return tally;
}

std::string DiagnoseTexelShift(const SamplePoint& point, const CpuImage& base,
    const CpuImage& lightmap, const LitConstants& constants, const Image& image)
{
    std::string found;
    auto record = [&](const char* what, int du, int dv) {
        if (!found.empty())
            found += ", ";
        found += format("%s(%+d,%+d)", what, du, dv);
    };

    for (int du = -1; du <= 1; ++du)
    {
        for (int dv = -1; dv <= 1; ++dv)
        {
            if (du == 0 && dv == 0)
                continue;
            const PixelPrediction shifted = EvaluatePixel(point.uv, point.color, point.fog, base,
                lightmap, constants, du, dv, 0, 0);
            bool ok = true;
            for (int channel = 0; channel < 3; ++channel)
                if (std::abs(static_cast<int>(SampleChannel(image, point.x, point.y, channel))
                        - static_cast<int>(QuantizeUnorm8(shifted.rgb[channel])))
                    > kPerChannelTolerance)
                    ok = false;
            if (ok)
                record("base", du, dv);

            const PixelPrediction shiftedLightmap = EvaluatePixel(point.uv, point.color, point.fog,
                base, lightmap, constants, 0, 0, du, dv);
            ok = true;
            for (int channel = 0; channel < 3; ++channel)
                if (std::abs(static_cast<int>(SampleChannel(image, point.x, point.y, channel))
                        - static_cast<int>(QuantizeUnorm8(shiftedLightmap.rgb[channel])))
                    > kPerChannelTolerance)
                    ok = false;
            if (ok)
                record("lightmap", du, dv);
        }
    }
    return found.empty() ? "no single-texel shift reproduces the readback" : found;
}


// Compares the CPU's own coverage mask with the readback. This is the geometric half of the
// prediction: if the projection is off by even a fraction of a pixel, pixels appear here as
// "ours but clear" or "theirs but outside every triangle", and no amount of shading work would
// explain the mismatch. Counted over the whole target, not just the sample points.
struct CoverageStats
{
    std::size_t ours = 0;
    std::size_t theirs = 0;
    std::size_t oursButClear = 0;
    std::size_t theirsButOutside = 0;
};

CoverageStats CompareCoverage(const Image& image, const std::vector<ProjectedTriangle>& triangles)
{
    CoverageStats stats{};
    for (std::uint32_t row = 0; row < image.height; ++row)
    {
        for (std::uint32_t column = 0; column < image.width; ++column)
        {
            bool inside = false;
            for (const ProjectedTriangle& triangle : triangles)
            {
                if (!triangle.usable)
                    continue;
                if (world_lit_bundle::sampling::InsideTriangle(triangle.screen, static_cast<float>(column),
                        static_cast<float>(row), 0.0f))
                {
                    inside = true;
                    break;
                }
            }
            const std::size_t offset = (static_cast<std::size_t>(row) * image.width + column) * 3u;
            const bool covered = !IsClearPixel(image, offset);
            if (inside)
                ++stats.ours;
            if (covered)
                ++stats.theirs;
            if (inside && !covered)
                ++stats.oursButClear;
            if (!inside && covered)
                ++stats.theirsButOutside;
        }
    }
    return stats;
}

std::size_t CountDifferingPixels(const Image& a, const Image& b)
{
    if (a.rgb.size() != b.rgb.size())
        return a.rgb.size();
    std::size_t differing = 0;
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(a.width) * a.height; ++pixel)
    {
        const std::size_t offset = pixel * 3u;
        if (a.rgb[offset + 0] != b.rgb[offset + 0] || a.rgb[offset + 1] != b.rgb[offset + 1]
            || a.rgb[offset + 2] != b.rgb[offset + 2])
            ++differing;
    }
    return differing;
}

std::uint8_t SampleChannel(const Image& image, std::uint32_t column, std::uint32_t row, int channel)
{
    const std::size_t offset =
        (static_cast<std::size_t>(row) * image.width + column) * 4u + static_cast<std::size_t>(channel);
    return image.rgba[offset];
}

// ---------------------------------------------------------------------------
// Byte perturbations used by the controls.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> PerturbLightmap(const std::vector<std::uint8_t>& payload, int delta)
{
    std::vector<std::uint8_t> perturbed = payload;
    // A8R8G8B8 in memory is B, G, R, A: bytes 0..2 carry colour and byte 3 is the alpha the
    // shader uses for its two-term weight, so alpha is left alone.
    for (std::size_t offset = 0; offset + 3 < perturbed.size(); offset += 4)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const int value = static_cast<int>(perturbed[offset + static_cast<std::size_t>(channel)])
                + delta;
            perturbed[offset + static_cast<std::size_t>(channel)] =
                static_cast<std::uint8_t>(std::min(255, std::max(0, value)));
        }
    }
    return perturbed;
}

// Copies a decoded mip chain with delta added to every level's R, G and B bytes, leaving alpha
// alone. The base map is RGBA in memory (the decoder's own order), so the three colour bytes are
// 0, 1 and 2 — no channel is swapped here, and the single conversion to A8R8G8B8 happens in
// UploadBaseTexture exactly as it does for the unperturbed chain.
bool PerturbBaseChain(const kisak::window::IwiMipChain& chain, int delta,
    kisak::window::IwiMipChain* out, std::string* error)
{
    out->clear();
    out->reserve(chain.size());
    for (const kisak::window::PixelBuffer& level : chain)
    {
        const kisak::window::PixelBufferView view = level.View();
        kisak::window::PixelBuffer copy;
        if (!copy.Resize(view.width, view.height))
        {
            *error = "cannot allocate a " + std::to_string(view.width) + "x"
                + std::to_string(view.height) + " copy of the base texture";
            return false;
        }
        const std::span<std::uint8_t> destination = copy.Bytes();
        for (std::size_t offset = 0; offset < view.bytes.size(); ++offset)
        {
            const std::size_t channel = offset % 4u;
            const int value = static_cast<int>(view.bytes[offset]) + (channel < 3u ? delta : 0);
            destination[offset] = static_cast<std::uint8_t>(std::min(255, std::max(0, value)));
        }
        out->push_back(std::move(copy));
    }
    return true;
}

// Builds the CPU-side RGBA view of the lightmap the oracle samples: BGRA in memory becomes
// RGBA, divided by 255, with no gamma or exponent decode.
CpuImage LightmapCpuImage(const std::vector<std::uint8_t>& payload, std::uint32_t width,
    std::uint32_t height)
{
    CpuImage image;
    image.width = width;
    image.height = height;
    image.rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    for (std::size_t texel = 0; texel < static_cast<std::size_t>(width) * height; ++texel)
    {
        const std::uint8_t* source = payload.data() + texel * 4u;
        image.rgba[texel * 4u + 0u] = source[2];
        image.rgba[texel * 4u + 1u] = source[1];
        image.rgba[texel * 4u + 2u] = source[0];
        image.rgba[texel * 4u + 3u] = source[3];
    }
    return image;
}

// Builds the CPU-side RGBA view of the base texture from the decoder's own pixels, so the
// oracle samples the same bytes the upload writes.
CpuImage BaseCpuImage(const kisak::window::IwiMipChain& chain)
{
    CpuImage image;
    if (chain.empty())
        return image;
    const kisak::window::PixelBufferView view = chain.front().View();
    image.width = static_cast<std::uint32_t>(view.width);
    image.height = static_cast<std::uint32_t>(view.height);
    image.rgba.assign(view.bytes.begin(), view.bytes.end());

    // TEMPORARY DIAGNOSTIC: row shift for the base image, to test whether the readback is
    // explained by the CPU image being one row out of step with the uploaded texture.
    const char* shiftText = std::getenv("LIT_BASE_ROW_SHIFT");
    if (shiftText && std::atoi(shiftText) != 0)
    {
        const int shift = std::atoi(shiftText);
        std::vector<std::uint8_t> shifted(image.rgba.size(), 0);
        for (std::uint32_t row = 0; row < image.height; ++row)
        {
            const int source = static_cast<int>(row) + shift;
            if (source < 0 || source >= static_cast<int>(image.height))
                continue;
            std::memcpy(shifted.data() + static_cast<std::size_t>(row) * image.width * 4u,
                image.rgba.data() + static_cast<std::size_t>(source) * image.width * 4u,
                image.width * 4u);
        }
        image.rgba = shifted;
    }
    return image;
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------
struct Report
{
    std::string text;
    bool failed = false;

    void line(const std::string& value) { text += value; text += '\n'; }

    void check(const char* what, bool ok, const std::string& detail)
    {
        text += std::string("[") + (ok ? "ok  " : "FAIL") + "] " + what;
        if (!detail.empty())
            text += ": " + detail;
        text += '\n';
        if (!ok)
            failed = true;
    }
};

} // namespace

int main(int argc, char** argv)
{
    Options options;
    bool helpRequested = false;
    std::string error;

    if (!parseArgs(argc, argv, options, &helpRequested, &error))
    {
        std::fprintf(stderr, "world_lit_replay: %s\n\n", error.c_str());
        printUsage(stderr);
        return ExitUsage;
    }
    if (helpRequested)
    {
        printUsage(stdout);
        return ExitPass;
    }

    const std::string& reportPath = options.reportPath;

    world_lit_bundle::Bundle bundle;
    if (!world_lit_bundle::Load(options.bundlePath, &bundle, &error))
    {
        std::fprintf(stderr, "world_lit_replay: bundle rejected: %s\n", error.c_str());
        return ExitBundleInvalid;
    }
    std::printf("%s", world_lit_bundle::Describe(bundle).c_str());

    // --- base texture --------------------------------------------------------
    // Decode the genuine base texture before computing CPU predictions. The
    // camera is fitted to geometry, independently of these texture samples.
    kisak::window::IwiMipChain chain;
    if (options.cpuOnly)
    {
        if (options.textureSupplied)
        {
            try
            {
                chain = kisak::window::LoadIwiMipChainFile(options.texturePath);
            }
            catch (const std::exception& exception)
            {
                std::fprintf(stderr, "world_lit_replay: %s\n", exception.what());
                return ExitTextureInvalid;
            }
        }
    }
    else
    {
        if (!options.textureSupplied)
        {
            std::fprintf(stderr,
                "world_lit_replay: --texture is required for a draw: the material samples a "
                "colour map, and a 1x1 substitute would make the image respond to a texture the "
                "bundle never named\n");
            return ExitUsage;
        }
        try
        {
            chain = kisak::window::LoadIwiMipChainFile(options.texturePath);
        }
        catch (const std::exception& exception)
        {
            std::fprintf(stderr, "world_lit_replay: %s\n", exception.what());
            return ExitTextureInvalid;
        }
    }

    if (chain.empty())
    {
        std::fprintf(stderr,
            "world_lit_replay: no base texture was decoded, so the CPU prediction has no colour "
            "map to sample. Pass --texture (the prepared me_wire_black IWI).\n");
        return ExitTextureInvalid;
    }


    // --- camera and context ------------------------------------------------
    // The camera and the cull rule are one decision: a world surface is drawn from the side its
    // winding is front-facing on, and the recorded state block says which winding that is. The
    // derivation therefore tests both sides of the referenced triangles' plane and keeps the one
    // that is actually drawn, instead of assuming the area-weighted normal points outward.
    const DecodedState decodedState = DecodeStateBlock(bundle.stateBits0, bundle.stateBits1);
    if (!decodedState.supported)
    {
        std::fprintf(stderr, "world_lit_replay: state block unsupported: %s\n",
            decodedState.unsupportedReason.c_str());
        return ExitStateUnsupported;
    }
    // D3DCULL_CCW keeps clockwise screen winding, D3DCULL_CW the opposite; with no culling both
    // sides are kept.
    const float frontSign = decodedState.cull == D3DCULL_CW ? -1.0f : 1.0f;

    auto buildContext = [&](const Vec3& eye, const Vec3& target, float zNear,
                            world_surface_context::Context* out, std::string* buildError) {
        world_surface_context::Inputs inputs{};
        inputs.camera.origin[0] = eye.value[0];
        inputs.camera.origin[1] = eye.value[1];
        inputs.camera.origin[2] = eye.value[2];
        inputs.camera.tanHalfFovX = options.tanHalfFov;
        inputs.camera.tanHalfFovY = options.tanHalfFov;
        inputs.camera.zNear = zNear;
        if (!BuildCameraBasis(Subtract(target, eye), &inputs.camera, buildError))
            return false;

        // Code 40 is not part of this profile. The context builder still requires the field, so
        // it is filled with zeros and never uploaded: UploadConstants names the four registers it
        // writes and PS c1 is not among them.
        for (int channel = 0; channel < 4; ++channel)
            inputs.materialColor.rgba[channel] = 0.0f;

        inputs.fog.enabled = false;
        inputs.fog.density = 0.0f;
        inputs.fog.fogStart = 0.0f;
        for (int channel = 0; channel < 4; ++channel)
        {
            inputs.fog.colorBgra[channel] = 0;
            inputs.fog.previousColor[channel] = options.fogColor[channel];
        }

        world_surface_context::Error contextError = world_surface_context::Error::None;
        if (!world_surface_context::Build(inputs, out, &contextError))
        {
            *buildError = std::string("the context builder refused the camera: ")
                + world_surface_context::ErrorName(contextError);
            return false;
        }
        return true;
    };

    CameraDerivation camera{};
    world_surface_context::Context context{};
    if (options.cameraEyeSupplied)
    {
        camera.eye = options.cameraEye;
        camera.target = options.cameraTarget;
        camera.derivedFromGeometry = false;
        const float length = Length(Subtract(camera.target, camera.eye));
        if (!(length > 0.0f))
        {
            std::fprintf(stderr, "world_lit_replay: --camera-eye and --camera-target coincide\n");
            return ExitUsage;
        }
        camera.zNear = std::max(length * 0.01f, 0.01f);
        if (!buildContext(camera.eye, camera.target, camera.zNear, &context, &error))
        {
            std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
            return ExitFailure;
        }
    }
    else
    {
        const kisak::window::PixelExtent baseExtent = chain.front().Extent();
        const std::uint32_t baseWidth = static_cast<std::uint32_t>(baseExtent.width);
        const std::uint32_t baseHeight = static_cast<std::uint32_t>(baseExtent.height);
        if (!DeriveCamera(bundle, options.tanHalfFov, baseWidth, baseHeight, &camera, &error))
        {
            std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
            return ExitFailure;
        }

        // The camera is the derived fit and is not moved afterwards. There is no option that
        // rescales it and no loop that pulls it in until a measurement improves: a camera chosen by
        // what it makes the comparison say is not evidence about the pass. The report states the
        // distance and the measured texel travel at the sample points so a reader can see both.
        float distance = camera.distance;
        float density = 0.0f;
        (void)density;
        camera.distance = distance;
        camera.texelsPerPixel = 0.0f;

        std::size_t bestFrontFacing = 0;
        bool haveCandidate = false;
        for (int side = 1; side >= -1; side -= 2)
        {
            Vec3 candidateEye{{camera.center.value[0] + camera.normal.value[0] * camera.distance * side,
                camera.center.value[1] + camera.normal.value[1] * camera.distance * side,
                camera.center.value[2] + camera.normal.value[2] * camera.distance * side}};
            world_surface_context::Context candidate{};
            if (!buildContext(candidateEye, camera.target, camera.zNear, &candidate, &error))
            {
                std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
                return ExitFailure;
            }
            const std::size_t frontFacing =
                CountFrontFacing(bundle, MakeConstants(candidate), frontSign);
            if (!haveCandidate || frontFacing > bestFrontFacing)
            {
                haveCandidate = true;
                bestFrontFacing = frontFacing;
                camera.eye = candidateEye;
                context = candidate;
            }
        }

        if (!haveCandidate || bestFrontFacing == 0)
        {
            std::fprintf(stderr,
                "world_lit_replay: neither side of the referenced triangles is front-facing under "
                "the recorded cull mode, so no camera can draw this surface\n");
            return ExitFailure;
        }
        std::printf("[camera] eye placed on the %s side of the referenced plane: %zu of %u "
                    "triangles front-facing\n",
            frontSign > 0.0f ? "clockwise-winding" : "counter-clockwise-winding", bestFrontFacing,
            bundle.triCount);
    }

    const LitConstants constants = MakeConstants(context);

    // --- sampler states ------------------------------------------------------
    const FilterTableInputs filterInputs{};
    SamplerStates baseSampler{};
    SamplerStates lightmapSampler{};
    const std::uint32_t baseSamplerRaw = bundle.textureArguments.front().samplerState;
    if (!DecodeSamplerState(static_cast<std::uint8_t>(baseSamplerRaw), filterInputs, &baseSampler,
            &error))
    {
        std::fprintf(stderr, "world_lit_replay: sampler 0: %s\n", error.c_str());
        return ExitStateUnsupported;
    }
    if (!DecodeSamplerState(static_cast<std::uint8_t>(world_lit_bundle::kLightmapSamplerState),
            filterInputs, &lightmapSampler, &error))
    {
        std::fprintf(stderr, "world_lit_replay: lightmap sampler: %s\n", error.c_str());
        return ExitStateUnsupported;
    }

    // --- CPU prediction for the base run -------------------------------------
    const CpuImage baseImage = BaseCpuImage(chain);
    const CpuImage lightmapImage = LightmapCpuImage(bundle.capture.payload, bundle.capture.width,
        bundle.capture.height);

    std::vector<SamplePoint> basePoints;
    if (!SelectSamplePoints(bundle, constants, baseImage, lightmapImage, &basePoints, &error))
    {
        std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
        return ExitFailure;
    }

    std::uint32_t earlyMax[3] = {0, 0, 0};
    std::size_t baseNonZeroCount = 0;
    std::size_t lightmapNonZeroCount = 0;
    for (const SamplePoint& point : basePoints)
    {
        for (int channel = 0; channel < 3; ++channel)
            earlyMax[channel] = std::max(earlyMax[channel], static_cast<std::uint32_t>(point.predicted[channel]));
        if (point.baseNonZero)
            ++baseNonZeroCount;
        if (point.lightmapNonZero)
            ++lightmapNonZeroCount;
    }

    // The lightmap control's CPU side: perturbing the payload must move the prediction at the
    // same pixels, or the lightmap is not observable through this image.
    const std::vector<std::uint8_t> perturbedLightmapBytes =
        PerturbLightmap(bundle.capture.payload, options.lightmapPerturbation);
    const CpuImage perturbedLightmap = LightmapCpuImage(perturbedLightmapBytes, bundle.capture.width,
        bundle.capture.height);

    std::vector<SamplePoint> lightmapPoints = basePoints;
    for (SamplePoint& point : lightmapPoints)
    {
        const PixelPrediction prediction = EvaluatePixel(point.uv, point.color, point.fog, baseImage,
            perturbedLightmap, constants);
        for (int channel = 0; channel < 3; ++channel)
            point.predicted[channel] = QuantizeUnorm8(prediction.rgb[channel]);
        point.predicted[3] = QuantizeUnorm8(prediction.alpha);
    }

    std::size_t lightmapChangedPoints = 0;
    for (std::size_t index = 0; index < basePoints.size() && index < lightmapPoints.size(); ++index)
        if (basePoints[index].predicted[0] != lightmapPoints[index].predicted[0]
            || basePoints[index].predicted[1] != lightmapPoints[index].predicted[1]
            || basePoints[index].predicted[2] != lightmapPoints[index].predicted[2])
            ++lightmapChangedPoints;

    // The base-texture control, built the same way: perturb the decoded bytes, predict from the
    // perturbed CPU image, and later upload exactly the chain that was predicted from. The encoder
    // is the decoder's own output order, so no channel is swapped and the perturbation is applied
    // once.
    kisak::window::IwiMipChain perturbedChain;
    if (!PerturbBaseChain(chain, options.basePerturbation, &perturbedChain, &error))
    {
        std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
        return ExitFailure;
    }
    const CpuImage perturbedBase = BaseCpuImage(perturbedChain);
    std::vector<SamplePoint> basePerturbedPoints = basePoints;
    for (SamplePoint& point : basePerturbedPoints)
    {
        const PixelPrediction prediction = EvaluatePixel(point.uv, point.color, point.fog,
            perturbedBase, lightmapImage, constants);
        for (int channel = 0; channel < 3; ++channel)
            point.predicted[channel] = QuantizeUnorm8(prediction.rgb[channel]);
        point.predicted[3] = QuantizeUnorm8(prediction.alpha);
    }

    std::size_t baseChangedPoints = 0;
    for (std::size_t index = 0; index < basePoints.size() && index < basePerturbedPoints.size(); ++index)
        if (basePoints[index].predicted[0] != basePerturbedPoints[index].predicted[0]
            || basePoints[index].predicted[1] != basePerturbedPoints[index].predicted[1]
            || basePoints[index].predicted[2] != basePerturbedPoints[index].predicted[2])
            ++baseChangedPoints;

    std::vector<ProjectedTriangle> allTriangles;
    if (!ProjectAllTriangles(bundle, constants, &allTriangles, &error))
    {
        std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
        return ExitFailure;
    }

    Report report;
    report.line("world_lit_replay: one TECHNIQUE_LIT pass of " + bundle.map + " surface "
        + std::to_string(bundle.surfaceIndex) + ", technique " + bundle.techniqueName);
    report.line("camera: eye " + format("%.4f,%.4f,%.4f", camera.eye.value[0], camera.eye.value[1],
                    camera.eye.value[2])
        + " target " + format("%.4f,%.4f,%.4f", camera.target.value[0], camera.target.value[1],
            camera.target.value[2])
        + (camera.derivedFromGeometry ? " (derived from the referenced triangles)"
                                      : " (explicit, supplied on the command line)"));
    report.line("target: " + std::to_string(kRenderWidth) + "x" + std::to_string(kRenderHeight)
        + " A8R8G8B8, fixed in this file");
    if (camera.derivedFromGeometry)
        report.line(format("camera: fit distance %.3f, eye distance %.3f, fixed by the referenced "
                           "extent and not adjusted by any measurement",
            camera.fitDistance, camera.distance));
    report.line(format("context: code 41 = (%.4f,%.4f,%.4f,%.4f) fog disabled, code 42 = "
                       "(%.4f,%.4f,%.4f,%.4f); code 40 is not uploaded",
        context.vsFog[0], context.vsFog[1], context.vsFog[2], context.vsFog[3], context.psFogColor[0],
        context.psFogColor[1], context.psFogColor[2], context.psFogColor[3]));
    report.line("programs: VS " + std::to_string(bundle.vertexShader.words) + " DWORDs, FNV-1a-64 "
        + world_lit_bundle::FnvHex(world_surface_bundle::Fnv1a64(
              bundle.vertexShader.payload.data(), bundle.vertexShader.payload.size()))
        + " (measured); PS " + std::to_string(bundle.pixelShader.words) + " DWORDs, FNV-1a-64 "
        + world_lit_bundle::FnvHex(world_surface_bundle::Fnv1a64(
              bundle.pixelShader.payload.data(), bundle.pixelShader.payload.size()))
        + " (measured); both equal the audited pair");
    report.line(format("sampler 0: raw %u -> 0x%08X (min %u mag %u mip %u aniso %u, U %u V %u W %u)",
        baseSamplerRaw, baseSampler.decodedDword, static_cast<unsigned>(baseSampler.minFilter),
        static_cast<unsigned>(baseSampler.magFilter), static_cast<unsigned>(baseSampler.mipFilter),
        static_cast<unsigned>(baseSampler.maxAnisotropy), static_cast<unsigned>(baseSampler.addressU),
        static_cast<unsigned>(baseSampler.addressV), static_cast<unsigned>(baseSampler.addressW)));
    report.line(format("sampler 3: raw %u -> 0x%08X (min %u mag %u mip %u aniso %u, U %u V %u W %u)",
        world_lit_bundle::kLightmapSamplerState, lightmapSampler.decodedDword,
        static_cast<unsigned>(lightmapSampler.minFilter),
        static_cast<unsigned>(lightmapSampler.magFilter),
        static_cast<unsigned>(lightmapSampler.mipFilter),
        static_cast<unsigned>(lightmapSampler.maxAnisotropy),
        static_cast<unsigned>(lightmapSampler.addressU),
        static_cast<unsigned>(lightmapSampler.addressV),
        static_cast<unsigned>(lightmapSampler.addressW)));
    report.line("base texture: " + std::to_string(baseImage.width) + "x"
        + std::to_string(baseImage.height) + " from the repository IWI decoder, "
        + std::to_string(chain.size()) + " mip level(s), RGB kept in the decoder's RGBA order");
    report.line("sample points: " + std::to_string(basePoints.size())
        + " interior pixels, integer coordinates on the ordinary viewport mapping, base-texel "
          "margin floor " + format("%.2f", kBaseTexelMargin) + " (fixed), predicted RGB maxima "
        + std::to_string(earlyMax[0]) + "," + std::to_string(earlyMax[1]) + ","
        + std::to_string(earlyMax[2]));
    {
        float worstTravel = 0.0f;
        float smallestMargin = 1.0f;
        std::size_t safe = 0;
        for (const SamplePoint& point : basePoints)
        {
            worstTravel = std::max(worstTravel, point.texelTravel);
            smallestMargin = std::min(smallestMargin, point.texelMargin);
            if (point.texelMargin > 0.5f * point.texelTravel)
                ++safe;
        }
        // What the numbers are and are not: they describe how fast the sampled coordinate moves
        // per pixel and how close the chosen coordinates sit to a texel boundary. They are not a
        // guarantee that a boundary cannot be crossed — at this camera the margin is often smaller
        // than the travel — and the run does not claim one. What the run claims is the readback
        // match at the declared tolerance, which is measured below.
        report.line(format("texel travel at the sample points: worst %.3f texels/pixel, smallest "
                           "boundary margin %.3f texels (%zu of %zu points further from a boundary "
                           "than half a pixel of travel)",
            worstTravel, smallestMargin, safe, basePoints.size()));
    }
    report.line("prediction: " + std::to_string(baseNonZeroCount)
        + " point(s) with a nonzero base-times-colour term, " + std::to_string(lightmapNonZeroCount)
        + " with a nonzero lightmap term, " + std::to_string(lightmapChangedPoints)
        + " changed by the lightmap perturbation, " + std::to_string(baseChangedPoints)
        + " by the base perturbation");

    // --- gates that precede any device --------------------------------------
    report.check("at least the required sample count",
        basePoints.size() >= kRequiredSamplePoints,
        std::to_string(basePoints.size()) + " points, floor is "
            + std::to_string(kRequiredSamplePoints));
    {
        std::size_t outOfBounds = 0;
        for (const SamplePoint& point : basePoints)
            if (point.x >= kRenderWidth || point.y >= kRenderHeight)
                ++outOfBounds;
        report.check("sample points inside the target", outOfBounds == 0,
            std::to_string(outOfBounds) + " outside");
    }
    report.check("percent-zero perturbations are refused",
        options.basePerturbation != 0 && options.lightmapPerturbation != 0,
        "base " + std::to_string(options.basePerturbation) + ", lightmap "
            + std::to_string(options.lightmapPerturbation)
            + "; a zero perturbation controls nothing and fails the run");
    report.check("base contribution is observable", baseNonZeroCount == basePoints.size(),
        std::to_string(baseNonZeroCount) + " of " + std::to_string(basePoints.size())
            + " points predict a nonzero base-times-colour product");
    report.check("lightmap control moves the prediction", lightmapChangedPoints > 0,
        std::to_string(lightmapChangedPoints) + " of " + std::to_string(basePoints.size())
            + " points changed by a " + std::to_string(options.lightmapPerturbation)
            + " byte perturbation");
    report.check("base control moves the prediction", baseChangedPoints > 0,
        std::to_string(baseChangedPoints) + " of " + std::to_string(basePoints.size())
            + " points changed by a " + std::to_string(options.basePerturbation)
            + " byte perturbation");

    if (options.cpuOnly)
    {
        report.line("cpu-only: no device was created and no draw was issued");
        std::printf("%s", report.text.c_str());
        if (!reportPath.empty())
        {
            std::FILE* file = std::fopen(reportPath.c_str(), "wb");
            if (file)
            {
                std::fwrite(report.text.data(), 1, report.text.size(), file);
                std::fclose(file);
            }
        }
        return report.failed ? ExitFailure : ExitPass;
    }

    // --- device preflight ----------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "world_lit_replay: SDL_Init failed: %s\n", SDL_GetError());
        return ExitDeviceUnavailable;
    }

    // DXVK Native picks its window-system integration from this variable; the SDL3 one is the
    // integration this build carries.
    if (SDL_getenv("DXVK_WSI_DRIVER") == nullptr)
        SDL_setenv_unsafe("DXVK_WSI_DRIVER", "SDL3", 0);

    // SDL's Vulkan loader has to be open before any of its Vulkan entry points exist; the probe
    // below runs through SDL_Vulkan_GetVkGetInstanceProcAddr for exactly the loader DXVK will use.
    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        std::fprintf(stderr, "world_lit_replay: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
        SDL_Quit();
        return ExitDeviceUnavailable;
    }

    const VulkanProbe probe = ProbeVulkanDevices();
    std::size_t deviceIndex = probe.devices.size();
    std::string deviceReason;
    const bool haveDevice =
        SelectVulkanDevice(probe.devices, options.expectGpu, &deviceIndex, &deviceReason);

    report.line("vulkan: " + std::to_string(probe.devices.size())
        + " physical device(s) enumerated by the loader in this process"
        + (probe.error.empty() ? "" : " (" + probe.error + ")"));
    for (std::size_t index = 0; index < probe.devices.size(); ++index)
        report.line("  device " + std::to_string(index) + ": \"" + probe.devices[index].name
            + "\" type=" + PhysicalDeviceTypeName(probe.devices[index].type)
            + (index == deviceIndex ? "  <- selected" : ""));

    if (!haveDevice)
    {
        report.check("a non-CPU Vulkan device is present", false, deviceReason);
        report.line("this run did not create a D3D9 device: a CPU rasteriser or a Wine HAL "
                    "substitution is not evidence about the GPU");
        std::printf("%s", report.text.c_str());
        if (!reportPath.empty())
        {
            std::FILE* file = std::fopen(reportPath.c_str(), "wb");
            if (file)
            {
                std::fwrite(report.text.data(), 1, report.text.size(), file);
                std::fclose(file);
            }
        }
        SDL_Quit();
        return ExitDeviceUnavailable;
    }

    const VulkanDeviceInfo& selectedDevice = probe.devices[deviceIndex];
    report.check("a non-CPU Vulkan device is present", true,
        "\"" + selectedDevice.name + "\" type=" + PhysicalDeviceTypeName(selectedDevice.type));

    // Bind DXVK to the device the preflight admitted: enumerating a GPU and then letting the
    // translator choose freely would not prove which one drew.
    if (SDL_setenv_unsafe("DXVK_FILTER_DEVICE_NAME", selectedDevice.name.c_str(), 1) != 0)
    {
        std::fprintf(stderr, "world_lit_replay: could not set DXVK_FILTER_DEVICE_NAME\n");
        SDL_Quit();
        return ExitDeviceUnavailable;
    }

    SDL_Window* window = SDL_CreateWindow("kisak world lit replay", static_cast<int>(kRenderWidth),
        static_cast<int>(kRenderHeight), SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window)
    {
        std::fprintf(stderr, "world_lit_replay: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return ExitDeviceUnavailable;
    }

    const HWND hwnd = dxvk::wsi::toHwnd(window);

    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS params{};
    if (!CreateDevice(hwnd, &d3d, &device, &params, &error))
    {
        std::fprintf(stderr, "world_lit_replay: %s\n", error.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return ExitDeviceUnavailable;
    }

    {
        // Printed for the record only. DXVK spoofs this identity, which is why the Vulkan
        // enumeration above is the statement about the GPU that counts.
        D3DADAPTER_IDENTIFIER9 identifier;
        std::memset(&identifier, 0, sizeof(identifier));
        if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier)))
            report.line(format("d3d9 adapter (spoofed by DXVK, not trusted): \"%s\" vendor 0x%04X "
                               "device 0x%04X",
                identifier.Description, static_cast<unsigned>(identifier.VendorId),
                static_cast<unsigned>(identifier.DeviceId)));
    }

    // Every device object this replay owns, created in one place so the Reset stage below can
    // rebuild exactly the same set instead of a hand-written subset.
    Resources resources;
    auto createResources = [&](std::string* createError) {
        std::uint32_t uploadedWidth = 0;
        std::uint32_t uploadedHeight = 0;
        if (!CreateShaderObjects(bundle, device, &resources, createError)
            || !CreateGeometry(bundle, device, &resources, createError)
            || !CreateTargets(device, &resources, createError))
            return false;
        if (!UploadBaseTexture(chain, device, &resources.baseTexture, &uploadedWidth,
                &uploadedHeight, createError))
            return false;
        if (uploadedWidth != baseImage.width || uploadedHeight != baseImage.height)
        {
            *createError = "the uploaded base texture extent disagrees with the CPU image";
            return false;
        }
        if (!UploadBaseTexture(perturbedChain, device, &resources.perturbedBaseTexture,
                &uploadedWidth, &uploadedHeight, createError))
            return false;
        if (uploadedWidth != perturbedBase.width || uploadedHeight != perturbedBase.height)
        {
            *createError = "the uploaded perturbed base texture extent disagrees with its CPU image";
            return false;
        }
        if (!UploadLightmap(bundle.capture.payload, bundle.capture.width, bundle.capture.height,
                device, &resources.lightmapTexture, createError))
            return false;
        if (!UploadLightmap(perturbedLightmapBytes, bundle.capture.width, bundle.capture.height,
                device, &resources.perturbedLightmapTexture, createError))
            return false;
        return true;
    };

    // Runs the four frames and reports them. Called once before the Reset and once after it.
    auto runFrames = [&](const char* stage, bool comparePerturbedControls) {
        Image baseRender;
        Image skipDraw;
        Image lightmapRender;
        Image basePerturbedRender;
        FrameInputs frame{};

        frame.issueDraw = true;
        frame.baseTexture = resources.baseTexture;
        frame.lightmap = resources.lightmapTexture;
        if (!RenderOnce(device, bundle, context, &resources, baseSampler, lightmapSampler, frame,
                &baseRender, &error))
            return false;

        frame.issueDraw = false;
        if (!RenderOnce(device, bundle, context, &resources, baseSampler, lightmapSampler, frame,
                &skipDraw, &error))
            return false;

        frame.issueDraw = true;
        frame.lightmap = resources.perturbedLightmapTexture;
        if (!RenderOnce(device, bundle, context, &resources, baseSampler, lightmapSampler, frame,
                &lightmapRender, &error))
            return false;

        frame.baseTexture = resources.perturbedBaseTexture;
        frame.lightmap = resources.lightmapTexture;
        if (!RenderOnce(device, bundle, context, &resources, baseSampler, lightmapSampler, frame,
                &basePerturbedRender, &error))
            return false;

        const ImageStats baseStats = Measure(baseRender);
        const ImageStats skipStats = Measure(skipDraw);
        const CoverageStats coverage = CompareCoverage(baseRender, allTriangles);

        // The uploaded texture, read back and compared with the bytes the prediction uses.
        if (std::strcmp(stage, "pre-reset") == 0)
        {
            const UploadCheck upload = CheckBaseUpload(resources.baseTexture, chain.front().View());
            report.check("base texture round-trips through the upload",
                upload.readable && upload.identicalRows == upload.rows, upload.detail);
        }

        report.line(std::string(stage) + " coverage: cpu mask " + std::to_string(coverage.ours)
            + " pixels, readback " + std::to_string(coverage.theirs) + ", cpu-only "
            + std::to_string(coverage.oursButClear) + ", readback-outside "
            + std::to_string(coverage.theirsButOutside));

        report.line(std::string(stage) + " readback: base " + std::to_string(baseStats.covered)
            + " covered, " + std::to_string(baseStats.nonBlack) + " nonblack, "
            + std::to_string(baseStats.opaque) + " opaque; RGB maxima "
            + std::to_string(baseStats.maximumRed) + "," + std::to_string(baseStats.maximumGreen)
            + "," + std::to_string(baseStats.maximumBlue));

        report.check("draw covered the target", baseStats.covered >= kMinCoveredPixels,
            std::to_string(baseStats.covered) + " pixels, need "
                + std::to_string(kMinCoveredPixels));
        report.check("skip-draw leaves the clear colour", skipStats.covered == 0,
            std::to_string(skipStats.covered) + " non-clear pixels");
        report.check("alpha is written as 1.0", baseStats.covered > 0
                && baseStats.opaque == baseStats.covered,
            std::to_string(baseStats.opaque) + " of " + std::to_string(baseStats.covered)
                + " covered pixels are opaque");

        std::size_t compared = 0;
        std::size_t matched = 0;
        int maximumDeviation = 0;
        for (const SamplePoint& point : basePoints)
        {
            ++compared;
            bool ok = true;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int actual = static_cast<int>(SampleChannel(baseRender, point.x, point.y, channel));
                const int predicted = static_cast<int>(point.predicted[channel]);
                maximumDeviation = std::max(maximumDeviation, std::abs(actual - predicted));
                if (std::abs(actual - predicted) > kPerChannelTolerance)
                    ok = false;
            }
            if (ok)
                ++matched;
            else
            {
                report.line(format(
                    "  mismatch at (%u,%u) predicted %u,%u,%u read %u,%u,%u | uv=(%.4f,%.4f) "
                    "lm=(%.4f,%.4f) A=(%.3f,%.3f,%.3f) B=(%.3f,%.3f,%.3f) T=(%.3f,%.3f,%.3f)",
                    point.x, point.y, point.predicted[0], point.predicted[1], point.predicted[2],
                    SampleChannel(baseRender, point.x, point.y, 0),
                    SampleChannel(baseRender, point.x, point.y, 1),
                    SampleChannel(baseRender, point.x, point.y, 2), point.uv[0], point.uv[1],
                    point.uv[2], point.uv[3], point.sampleA[0], point.sampleA[1], point.sampleA[2],
                    point.sampleB[0], point.sampleB[1], point.sampleB[2], point.baseTexel[0],
                    point.baseTexel[1], point.baseTexel[2]));
                report.line("    texel shift that would explain it: "
                    + DiagnoseTexelShift(point, baseImage, lightmapImage, constants, baseRender));
            }
        }
        report.check("readback matches the CPU prediction", matched == compared,
            std::to_string(matched) + " of " + std::to_string(compared) + " points within "
                + std::to_string(kPerChannelTolerance) + " byte/channel, largest deviation "
                + std::to_string(maximumDeviation) + " of 255");

        const ShiftTally tally =
            TallyBaseShifts(basePoints, baseImage, lightmapImage, constants, baseRender);
        report.line("sampling tally over the same points: as written " + std::to_string(tally.unshifted)
            + ", base texel v-1 " + std::to_string(tally.vMinusOne) + ", u-1 "
            + std::to_string(tally.uMinusOne) + ", u+1 " + std::to_string(tally.uPlusOne));

        if (!comparePerturbedControls)
            return true;

        const std::size_t lightmapDiff = CountDifferingPixels(baseRender, lightmapRender);
        report.check("the image responds to the lightmap", lightmapDiff > 0,
            std::to_string(lightmapDiff) + " pixels changed by a "
                + std::to_string(options.lightmapPerturbation) + " byte perturbation");

        const std::size_t baseDiff = CountDifferingPixels(baseRender, basePerturbedRender);
        report.check("the image responds to the base texture", baseDiff > 0,
            std::to_string(baseDiff) + " pixels changed by a "
                + std::to_string(options.basePerturbation) + " byte perturbation");

        // The same perturbed predictions must also match, which is what makes each control a
        // control rather than a difference between two unexplained images.
        std::size_t perturbedMatched = 0;
        for (const SamplePoint& point : lightmapPoints)
        {
            bool ok = true;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int actual = static_cast<int>(SampleChannel(lightmapRender, point.x, point.y, channel));
                const int predicted = static_cast<int>(point.predicted[channel]);
                if (std::abs(actual - predicted) > kPerChannelTolerance)
                    ok = false;
            }
            if (ok)
                ++perturbedMatched;
            else
                report.line(format(
                    "  perturbed mismatch at (%u,%u) predicted %u,%u,%u read %u,%u,%u | "
                    "A=(%.3f,%.3f,%.3f) B=(%.3f,%.3f,%.3f) T=(%.3f,%.3f,%.3f)",
                    point.x, point.y, point.predicted[0], point.predicted[1], point.predicted[2],
                    SampleChannel(lightmapRender, point.x, point.y, 0),
                    SampleChannel(lightmapRender, point.x, point.y, 1),
                    SampleChannel(lightmapRender, point.x, point.y, 2), point.sampleA[0],
                    point.sampleA[1], point.sampleA[2], point.sampleB[0], point.sampleB[1],
                    point.sampleB[2], point.baseTexel[0], point.baseTexel[1], point.baseTexel[2]));
        }
        report.check("the perturbed lightmap run matches the perturbed prediction",
            perturbedMatched == lightmapPoints.size(),
            std::to_string(perturbedMatched) + " of " + std::to_string(lightmapPoints.size()));

        std::size_t basePerturbedMatched = 0;
        for (const SamplePoint& point : basePerturbedPoints)
        {
            bool ok = true;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int actual =
                    static_cast<int>(SampleChannel(basePerturbedRender, point.x, point.y, channel));
                const int predicted = static_cast<int>(point.predicted[channel]);
                if (std::abs(actual - predicted) > kPerChannelTolerance)
                    ok = false;
            }
            if (ok)
                ++basePerturbedMatched;
        }
        report.check("the perturbed base run matches the perturbed prediction",
            basePerturbedMatched == basePerturbedPoints.size(),
            std::to_string(basePerturbedMatched) + " of "
                + std::to_string(basePerturbedPoints.size()));

        if (!WritePpm(options.outputPath, baseRender, &error))
        {
            report.check("write the PPM", false, error);
        }
        else
        {
            report.check("write the PPM", true, options.outputPath);
        }

        return true;
    };

    bool deviceReady = false;
    do
    {
        if (!createResources(&error))
            break;
        if (!runFrames("pre-reset", true))
            break;
        deviceReady = true;
    } while (false);

    if (!deviceReady && !error.empty())
        report.check("device pass", false, error);

    // --- device reset --------------------------------------------------------
    // D3D9 Reset invalidates every D3DPOOL_DEFAULT object, so the whole set is released and
    // rebuilt. The expectation is the same one the first pass had to meet: the rebuilt device
    // draws the same image from the same objects, and that image still matches the prediction.
    if (deviceReady)
    {
        ReleaseResources(&resources);
        const HRESULT resetResult = device->Reset(&params);
        if (FAILED(resetResult))
        {
            report.check("device Reset", false,
                "IDirect3DDevice9::Reset failed, hr=" + hrText(resetResult));
        }
        else
        {
            report.check("device Reset", true, "Reset accepted the same present parameters");
            std::string resetError;
            bool resetReady = false;
            if (!createResources(&resetError))
            {
                report.check("resources rebuild after Reset", false, resetError);
            }
            else
            {
                resetReady = true;
                report.check("resources rebuild after Reset", true,
                    "shaders, geometry, targets and four textures recreated");
            }

            if (resetReady)
            {
                // The same mandatory suite, not the base frame alone: skip-draw, base
                // perturbation, lightmap perturbation and the alpha prediction all run again on
                // the rebuilt device, and every one of them has to hold. A Reset that only
                // reproduced the untouched base frame would leave the controls untested on the
                // device that is actually being claimed.
                if (!runFrames("post-reset", true))
                    report.check("post-Reset suite", false,
                        error.empty() ? "the post-Reset device run failed" : error);
            }
        }
    }

    ReleaseResources(&resources);
    device->Release();
    d3d->Release();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("%s", report.text.c_str());
    std::printf("world_lit_replay: %s\n", report.failed ? "FAIL" : "PASS");

    if (!reportPath.empty())
    {
        std::FILE* file = std::fopen(reportPath.c_str(), "wb");
        if (file)
        {
            std::fwrite(report.text.data(), 1, report.text.size(), file);
            std::fclose(file);
        }
    }

    if (report.failed)
        return ExitOracleMismatch;
    return ExitPass;
}
