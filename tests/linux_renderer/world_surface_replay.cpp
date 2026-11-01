// tests/linux_renderer/world_surface_replay.cpp
//
// Standalone replay of ONE exported static world surface through DXVK Native D3D9.
//
// What this program is
// --------------------
// It reads one schema-1 world surface bundle (world_surface_bundle.h), builds the D3D9
// objects the bundle describes, draws the surface's indexed triangles once into a fixed
// 256x256 offscreen render target, reads the result back through a SYSTEMMEM surface and
// writes it as a PPM outside the repository.
//
// What this program is NOT
// ------------------------
// It is not a renderer and it is not a frame replay. Specifically:
//   * the camera is synthetic: there is no captured refdef, and the eye is derived from
//     the referenced triangle so that the geometry lands inside the frustum;
//   * the material colour is the value the caller states with --material-color, because the
//     bundle does not carry the scene's code-40 value. The run refuses to start without that
//     flag: a neutral picked here would be this file's opinion rather than a recorded number,
//     and code 40's alpha is a live shader input, not padding;
//   * fog is explicitly disabled with a declared previous fog colour, matching the
//     branch of R_SetFrameFog that overwrites code 41 only;
//   * the texture is whatever the texture preparer wrote, and when none is supplied the
//     run binds a 1x1 white texel and says so rather than pretending to have the asset.
// A pass therefore means "these recorded numbers drove this device without error and the
// image responds to them", not "the engine would have produced this pixel".
//
// Fidelity rules that shape the code
// ----------------------------------
//   * The D3D9 state comes from the bundle's recorded state block, decoded with the
//     engine's own bit layout (src/gfx_d3d/r_state.h). A state whose mapping this file
//     does not implement is reported as unsupported and stops the run: no field is ever
//     filled with a plausible default.
//   * The sampler DWORD is produced by the engine's R_DecodeSamplerState formula. The
//     filter entry half of it is built at renderer init from device caps and cvars that
//     the bundle does not carry, so those inputs are stated on the command line instead
//     of being assumed.
//   * The vertex declaration is transcribed from s_streamSourceInfo[VERTDECL_WORLD] and
//     s_streamDestInfo: position FLOAT4 at 0, colour D3DCOLOR at 16, texcoord FLOAT4 at
//     20, stride 44. The loader rejects any bundle that records a different layout.
//
// Build and run: see README.md next to this file. The DXVK native headers define
// __stdcall/WINAPI as empty, so this uses the default Linux ABI throughout; never add
// WINAPI or __stdcall here.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <d3d9.h>
#include <windows.h>
#include <wsi/native_sdl3.h>

#include "world_surface_bundle.h"
#include "world_surface_context.h"
#include "world_surface_oracle.h"

// The routing contract in world_surface_bundle.h transcribes code indices and destination
// registers that world_surface_context.h also has to know, because that is where the values
// are built. Two independent transcriptions of the same engine table is one too many unless
// something checks them: these make a disagreement a compile error rather than an upload to
// the wrong register.
static_assert(
    world_surface_bundle::kConstantCodeMaterialColor == world_surface_context::kCodeMaterialColor,
    "code 40 disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kConstantCodeFog == world_surface_context::kCodeFog,
    "code 41 disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kConstantCodeFogColor == world_surface_context::kCodeFogColor,
    "code 42 disagrees between the bundle contract and the context");
static_assert(
    world_surface_bundle::kConstantCodeTransposeWorld == world_surface_context::kCodeTransposeWorldMatrix,
    "code 60 disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kConstantCodeTransposeViewProjection
        == world_surface_context::kCodeTransposeViewProjectionMatrix,
    "code 76 disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kVertexDestTransposeViewProjection
        == world_surface_context::kVsDestTransposeViewProjection,
    "the code-76 destination disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kVertexDestTransposeWorld
        == world_surface_context::kVsDestTransposeWorld,
    "the code-60 destination disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kVertexDestFog == world_surface_context::kVsDestFog,
    "the code-41 destination disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kPixelDestFogColor == world_surface_context::kPsDestFogColor,
    "the code-42 destination disagrees between the bundle contract and the context");
static_assert(world_surface_bundle::kPixelDestMaterialColor
        == world_surface_context::kPsDestMaterialColor,
    "the code-40 destination disagrees between the bundle contract and the context");

// The replay links the repository's existing IWI decoder directly
// (src/linux/client/window/iwi_assets.cpp). No intermediate image container, no second
// decode path, and nothing that could re-encode the pixels before they reach the GPU.
#include "iwi_assets.h"

// The decoder objects call the engine's assert hook, which for Linux lives in
// src/linux/sys_assert.cpp and needs the common runtime (Com_PrintError). This binary
// deliberately does not link the engine runtime, so the hook is defined here with the
// engine's exact signature (universal/assertive.h) and behaviour that matters: report, then
// stop. An assertion in the decode path means the decoder refused to continue, and a
// standalone test binary must not carry on after that.
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
// Fixed render target. The size is a deliberate constant: a replay whose output size
// depends on the desktop would not be comparable between runs.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kRenderWidth = 256;
constexpr std::uint32_t kRenderHeight = 256;

// Clear colour. Magenta is a colour no map material produces, which keeps "the draw
// produced black" and "the draw produced nothing" distinguishable in the readback: with a
// black clear those two cases look identical and a zero-coverage bug hides.
constexpr std::uint8_t kClearRed = 255;
constexpr std::uint8_t kClearGreen = 0;
constexpr std::uint8_t kClearBlue = 255;

// A pass needs the triangle to occupy enough of the target to be measurable without
// demanding a particular silhouette.
constexpr std::size_t kMinCoveredPixels = 256;

enum ExitCode
{
    ExitPass = 0,
    ExitFailure = 1,
    ExitUsage = 2,
    ExitBundleInvalid = 3,
    ExitStateUnsupported = 4,
    ExitDeviceUnavailable = 5,
};

#if defined(__GNUC__)
#define REPLAY_PRINTF_FORMAT(first, second) __attribute__((format(printf, first, second)))
#else
#define REPLAY_PRINTF_FORMAT(first, second)
#endif

std::string format(const char* fmt, ...) REPLAY_PRINTF_FORMAT(1, 2);

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

// ---------------------------------------------------------------------------
// D3D9 state block decode.
//
// Bit layout copied from src/gfx_d3d/r_state.h (GFXS0_* / GFXS1_*) and the lookup tables
// in src/gfx_d3d/r_state.cpp (s_cullTable_30, s_depthTestTable_30).
//
// Every field is decoded or the whole block is reported unsupported. The struct has no
// default initialisers on purpose: a value that was never assigned cannot be mistaken
// for a decoded one.
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
    DWORD polygonOffsetFactor; // 0, 1 or 2; the engine selects a preset offset
};

// s_depthTestTable_30, indexed by (stateBits1 & GFXS1_DEPTHTEST_MASK) >> 2.
const D3DCMPFUNC kDepthTestTable[4] = {
    D3DCMP_ALWAYS, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL};

// s_cullTable_30 = {0, 1, 3, 2}, indexed by (stateBits0 & GFXS0_CULL_MASK) >> 14. The
// engine stores D3DCULL values in the table, so encoded NONE (0x4000 -> 1) maps to
// D3DCULL_NONE (1), encoded BACK (0x8000 -> 2) to D3DCULL_CCW (3) and encoded FRONT
// (0xC000 -> 3) to D3DCULL_CW (2). Slot 0 holds 0, which is not a D3DCULL, so a block
// selecting it is refused rather than mapped to a neighbouring mode.
const D3DCULL kCullTable[4] = {D3DCULL_NONE /* slot 0 is invalid, see the check below */,
    D3DCULL_NONE, D3DCULL_CCW, D3DCULL_CW};

// Decodes one cull field. False means the encoded value has no D3DCULL meaning.
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
        {
            // Every other encoded value is a D3DCULL from s_cullTable_30; refusing this one
            // keeps a no-cull material from being drawn with one winding dropped.
            unsupported("GFXS0_CULL 0 indexes slot 0 of s_cullTable_30, which is not a D3DCULL");
        }
    }

    // --- alpha test (stateBits0 bit 11, function bits 12-13) ---------------
    decoded.alphaTestEnabled = (stateBits0 & 0x800u) == 0;
    if (decoded.alphaTestEnabled)
    {
        // ATEST_GT_0 / ATEST_LT_128 / ATEST_GE_128 select a function and a reference
        // alpha that R_SetAlphaTestFunction derives together. This replay only needs the
        // disabled case; anything else is refused instead of being approximated.
        unsupported("alpha test enabled: R_SetAlphaTestFunction's reference-alpha pairing "
                    "is not modelled here");
    }

    // --- blend (stateBits0 bits 8-10) --------------------------------------
    // R_ChangeState_0 treats GFXS0_BLENDOP_RGB_MASK == 0 as "blending disabled" and
    // ignores the source/destination factors in that case.
    decoded.blendEnabled = (stateBits0 & 0x700u) != 0;
    if (decoded.blendEnabled)
    {
        unsupported("blending enabled: R_HW_SetBlend's factor and op tables are not modelled here");
    }
    else if ((stateBits0 & 0x7000000u) != 0)
    {
        // The engine asserts the alpha blend op is disabled whenever the RGB op is.
        unsupported("alpha blend op is set while the RGB blend op is disabled");
    }

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
// Sampler decode.
//
// R_DecodeSamplerState (src/gfx_d3d/r_state.cpp):
//
//   tableIndex = samplerState & 0x1F;
//   return s_decodeSamplerFilterState[tableIndex]
//        | 0x1500000
//        | ((samplerState & 0x20) << 16)
//        | ((samplerState & 0x40) << 17)
//        | ((samplerState & 0x80) << 18);
//
// The 0x1500000 part and the three clamp shifts are exact. s_decodeSamplerFilterState is
// filled by R_InitSamplerFilterState from r_texFilterMipMode, r_texFilterDisable and the
// device's reported anisotropy limits, none of which the bundle carries, so this file
// rebuilds the entry from those inputs stated explicitly rather than assuming them.
//
// DWORD layout consumed by R_HW_SetSamplerState (src/gfx_d3d/r_setstate_d3d.h):
//   bits 0-7    D3DSAMP_MAXANISOTROPY   ((uint8_t)samplerState)
//   bits 8-11   D3DSAMP_MINFILTER
//   bits 12-15  D3DSAMP_MAGFILTER
//   bits 16-19  D3DSAMP_MIPFILTER       ((samplerState & 0xF0000) >> 16)
//   bits 20-21  D3DSAMP_ADDRESSU
//   bits 22-23  D3DSAMP_ADDRESSV
//   bits 24-25  D3DSAMP_ADDRESSW
//
// The low byte is anisotropy, NOT mip filtering. Reading it as the mip filter is the
// mistake this layout comment exists to prevent: with r_texFilterDisable=1 the filter
// entry is 0x1101, so the low byte is 1 (one sample) while the engine's mip filter is
// NONE, and treating the byte as a filter selects POINT mips instead.
constexpr std::uint32_t kSamplerAddressWrapAll = 0x1500000u;

// s_mipFilterTable, transcribed from r_state.cpp. Values are D3DTEXTUREFILTERTYPE.
const DWORD kMipFilterTable[4][3] = {
    {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR},
    {D3DTEXF_NONE, D3DTEXF_LINEAR, D3DTEXF_LINEAR},
    {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_POINT},
    {D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_NONE},
};

// The table has three columns and R_InitSamplerFilterState indexes it with
// (entryIndex & 0x18) >> 3, which is 3 for entry indices 24..31. Those indices are outside
// the 24 entries the engine fills, so the replay refuses a raw sampler whose low five bits
// land there instead of reading past the end of the transcribed table.
constexpr std::uint32_t kMaxSamplerTableIndex = 24;

// The inputs R_InitSamplerFilterState derives at renderer init.
struct FilterTableInputs
{
    std::uint32_t mipFilterMode = 0; // r_texFilterMipMode
    std::uint32_t anisotropicFilter = 0;
    std::uint32_t aniso2x = 0;
    std::uint32_t aniso4x = 0;
    std::uint32_t linearMippedAnisotropy = 0;
    std::uint32_t linearMippedFilter = 0;
    std::uint32_t linearNonMippedFilter = 0;
};

// r_texFilterDisable=1 pins every one of those inputs in r_state.cpp, so that branch is
// fully determined by source and needs nothing from the caller.
FilterTableInputs DisabledFilterTableInputs()
{
    FilterTableInputs inputs{};
    inputs.mipFilterMode = 3;
    inputs.anisotropicFilter = 4352u; // 0x1100
    inputs.aniso2x = 1u;
    inputs.aniso4x = 1u;
    inputs.linearMippedAnisotropy = 1u;
    inputs.linearMippedFilter = 4352u;
    inputs.linearNonMippedFilter = 4352u;
    return inputs;
}

// R_InitSamplerFilterState's loop body, for one entry index.
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

// R_DecodeSamplerState, with the filter entry supplied instead of read from engine state.
// Returns false for a table index the engine never fills, rather than indexing past the
// transcribed s_mipFilterTable.
bool DecodeSamplerState(std::uint8_t samplerState, const FilterTableInputs& inputs,
    std::uint32_t* decoded, std::string* error)
{
    const std::uint32_t tableIndex = samplerState & 0x1Fu;
    if (tableIndex >= kMaxSamplerTableIndex)
    {
        *error = format("raw sampler 0x%02X selects filter-table index %u, outside the %u "
                        "entries R_InitSamplerFilterState fills",
            static_cast<unsigned>(samplerState), tableIndex, kMaxSamplerTableIndex);
        return false;
    }
    *decoded = BuildFilterTableEntry(tableIndex, inputs)
        | kSamplerAddressWrapAll
        | (static_cast<std::uint32_t>(samplerState & 0x20u) << 16)
        | (static_cast<std::uint32_t>(samplerState & 0x40u) << 17)
        | (static_cast<std::uint32_t>(samplerState & 0x80u) << 18);
    return true;
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
};

// R_HW_SetSamplerState's per-field decode of the DWORD R_DecodeSamplerState produced.
SamplerStates SplitSamplerDword(std::uint32_t decoded)
{
    SamplerStates states{};
    states.maxAnisotropy = decoded & 0xFFu;
    states.minFilter = (decoded & 0xF00u) >> 8;
    states.magFilter = (decoded & 0xF000u) >> 12;
    states.mipFilter = (decoded & 0xF0000u) >> 16;
    states.addressU = (decoded & 0x300000u) >> 20;
    states.addressV = (decoded & 0xC00000u) >> 22;
    states.addressW = (decoded & 0x3000000u) >> 24;
    return states;
}

// ---------------------------------------------------------------------------
// PPM P6 for the render output only. The texture crossing does not use this format: the
// texture is an IWI image decoded in this process into RGBA mip chains by the repository's
// own decoder, so the pixels come from that decoder rather than from an invented image
// format.
// ---------------------------------------------------------------------------
struct Image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // `rgb` is the three-channel view the PPM writer and the statistics read. `rgba` is the
    // full readback of the A8R8G8B8 target, alpha included, and it exists because the pixel
    // oracle predicts four channels: the decoded program computes output alpha as
    // texel.a * vertex.a and that value cannot be checked from a three-channel copy.
    std::vector<std::uint8_t> rgb;  // width * height * 3
    std::vector<std::uint8_t> rgba; // width * height * 4
};

bool WritePpm(const std::string& path, const Image& image, std::string* error)
{
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
// Geometry helpers for the synthetic camera.
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

// Camera basis in the convention the engine uses (CoD basis: forward = axis[0],
// left = axis[1], up = axis[2]) with left == up x forward.
//
// The basis is built from geometry and then normalised and re-orthogonalised here, so the
// context builder receives vectors that are unit length and orthogonal to within float
// rounding instead of relying on the arithmetic to happen to work out.
bool BuildCameraBasis(const Vec3& forwardIn, world_surface_context::CameraInputs* camera,
    std::string* error)
{
    Vec3 forward{};
    if (!Normalize(forwardIn, &forward))
    {
        *error = "the derived view direction has zero length";
        return false;
    }

    // World up for a CoD map is +z. When the view direction is nearly parallel to it the
    // cross product degenerates, so switch to +y for that case only.
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

    // Re-derive left from the finished up and forward so that left == up x forward holds
    // exactly, not just to within the first cross product's rounding.
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

// ---------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------
struct Options
{
    std::string bundlePath;
    std::string outputPath = "/tmp/kisak-world-surface-replay/world-surface.ppm";
    std::string reportPath;

    // Sampler filter-table inputs. When --tex-filter-disable 1 is given the remaining
    // values are pinned by r_state.cpp and must not be supplied.
    bool texFilterDisableSupplied = false;
    bool texFilterDisable = false;
    bool filterInputsSupplied = false;
    FilterTableInputs filterInputs{};

    // Camera.
    bool cameraSideBack = false;
    bool cameraSideSupplied = false;
    float cameraDistanceScale = 3.0f;
    float tanHalfFov = 0.57735026f; // tan(30 deg)

    // Explicit camera. When both points are given, the eye and the target are the caller's
    // numbers and the synthetic derivation does not run at all: nothing is derived from the
    // referenced triangle, so a surface with more than one triangle can be replayed without
    // editing the bundle's indices. With neither given, every camera number is derived exactly
    // as before. One without the other is refused rather than completed with a default.
    bool cameraEyeSupplied = false;
    Vec3 cameraEye{};
    bool cameraTargetSupplied = false;
    Vec3 cameraTarget{};

    // Material colour, i.e. the value of constant code 40. The bundle does not carry the
    // scene's value (its own colorTint literal has no register base), so it has to be stated
    // on the command line and is printed in the report.
    //
    // There is deliberately no default. A neutral invented here would make the pass depend on
    // a value no source recorded, and every control that compares two runs would be reading a
    // colour this file chose rather than one the caller stated. Alpha is not padding either:
    // the decoded pixel shader uses code 40's alpha in an alpha replacement lerp, so 1,1,1,1
    // and 1,1,1,0 are materially different runs. The array is zero until the flag is parsed,
    // and a run whose flag is missing stops in parseArgs before any of it is read.
    float materialColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool materialColorSupplied = false;

    // Optional texture for sampler 0: an IWI image decoded in this process by the repository's
    // own decoder (iwi_assets.cpp). There is no intermediate image container.
    std::string texturePath;

    // Pure-function checks for the decode tables this file transcribes, so the cull and
    // sampler mappings can be verified without a device.
    bool selfTest = false;
};

void printUsage(std::FILE* out)
{
    std::fprintf(out,
        "usage: world_surface_replay --bundle PATH --material-color R,G,B,A [options]\n"
        "\n"
        "  --bundle PATH             schema-1 world surface bundle to replay (required)\n"
        "  --output PATH             PPM P6 to write (default %s)\n"
        "  --report PATH             also write the text report to this file\n"
        "  --texture PATH            IWI image, decoded in this process by the repository's\n"
        "                            own decoder, bound to sampler 0. Without it the run binds\n"
        "                            a 1x1 white texel and reports that the result is not\n"
        "                            textured.\n"
        "  --camera-side front|back  which side of the referenced triangle the synthetic\n"
        "                            eye sits on, relative to its winding (default front)\n"
        "  --camera-distance SCALE   eye distance as a multiple of the longest triangle\n"
        "                            edge (default 3.0)\n"
        "  --tan-half-fov VALUE      projection half-angle tangent (default 0.57735026)\n"
        "  --camera-eye X,Y,Z        explicit eye. Given together with --camera-target, the\n"
        "                            camera is these numbers and nothing is derived from the\n"
        "                            referenced triangle. This is the only way to replay a\n"
        "                            surface with more than one triangle, whose indices are\n"
        "                            never edited for the camera. One of the two flags without\n"
        "                            the other is refused.\n"
        "  --camera-target X,Y,Z     explicit look-at point, with --camera-eye\n"
        "  --material-color R,G,B,A  value of constant code 40, the material colour (required).\n"
        "                            The bundle does not carry it (the exporter records only a\n"
        "                            colorTint literal with no register base), so the run\n"
        "                            states it and prints it. Alpha is a real input, not\n"
        "                            padding: the pixel shader lerps a replacement alpha\n"
        "                            through it, so 1,1,1,1 and 1,1,1,0 give different images.\n"
        "  --self-test               run the cull/sampler decode self-checks and exit; needs\n"
        "                            no bundle and no device\n"
        "\n"
        "There is no option that skips the controls: a run that cannot show the image\n"
        "responding to the constants and to the bound texture is not a pass. A prepared\n"
        "texture is likewise required for a pass, because the material samples one.\n"
        "\n"
        "Sampler filter-table inputs (R_InitSamplerFilterState fills these from device caps\n"
        "and cvars, which the bundle does not carry). Either pin the disabled branch:\n"
        "  --tex-filter-disable 1    every filter input is then fixed by r_state.cpp\n"
        "or state the cap-derived values explicitly:\n"
        "  --mip-filter-mode N --anisotropic-filter N --aniso2x N --aniso4x N\n"
        "  --linear-mipped-anisotropy N --linear-mipped-filter N --linear-nonmipped-filter N\n"
        "\n"
        "Exit codes: 0 pass, 1 failure, 2 usage, 3 bundle invalid,\n"
        "            4 state block unsupported, 5 device unavailable.\n",
        "/tmp/kisak-world-surface-replay/world-surface.ppm");
}

bool parseU32(const char* text, std::uint32_t* value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (!end || *end != '\0')
        return false;
    *value = static_cast<std::uint32_t>(parsed);
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

// Three comma-separated numbers, for a world-space point. Field by field with strtod and an
// explicit delimiter check, so "1,2" and "1,2,3,4" are refused instead of being truncated or
// accepted with a trailing component dropped. A non-finite component is refused too: an eye or
// target of inf/nan would produce a camera the context builder has to reject later, and the
// parser is where the caller can still be told which flag is wrong.
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
            // Refused rather than ignored: a control set that can be switched off is a
            // control set that will be switched off, and the run would still print PASS.
            *error = "--skip-controls does not exist: the constant, texture, skip-draw and "
                     "offscreen controls always run";
            return false;
        }
        if (arg == "--self-test")
        {
            options.selfTest = true;
            continue;
        }

        // Every other option takes a value.
        if (value.empty())
        {
            if (index + 1 >= argc)
            {
                *error = "missing value for " + arg;
                return false;
            }
            value = argv[++index];
        }

        if (arg == "--bundle")
        {
            options.bundlePath = value;
        }
        else if (arg == "--output")
        {
            options.outputPath = value;
        }
        else if (arg == "--report")
        {
            options.reportPath = value;
        }
        else if (arg == "--texture")
        {
            options.texturePath = value;
        }
        else if (arg == "--camera-eye")
        {
            if (!parseVec3(value.c_str(), &options.cameraEye))
            {
                *error = "--camera-eye expects three comma-separated numbers, e.g. -3250,3375,1080";
                return false;
            }
            options.cameraEyeSupplied = true;
        }
        else if (arg == "--camera-target")
        {
            if (!parseVec3(value.c_str(), &options.cameraTarget))
            {
                *error = "--camera-target expects three comma-separated numbers, "
                         "e.g. -1984,2680,192";
                return false;
            }
            options.cameraTargetSupplied = true;
        }
        else if (arg == "--material-color")
        {
            // Comma-separated, so each field is parsed with strtod and the delimiter checked
            // explicitly: the generic parseFloat above requires the whole string to be one
            // number.
            float parsed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const char* cursor = value.c_str();
            bool ok = true;
            for (int index = 0; index < 4 && ok; ++index)
            {
                char* end = nullptr;
                const double component = std::strtod(cursor, &end);
                if (!end || end == cursor)
                {
                    ok = false;
                    break;
                }
                parsed[index] = static_cast<float>(component);
                if (index < 3)
                {
                    if (*end != ',')
                        ok = false;
                    else
                        cursor = end + 1;
                }
                else if (*end != '\0')
                {
                    ok = false;
                }
            }
            if (!ok)
            {
                *error = "--material-color expects four comma-separated numbers, e.g. 1,1,1,0";
                return false;
            }
            for (int index = 0; index < 4; ++index)
                options.materialColor[index] = parsed[index];
            options.materialColorSupplied = true;
        }
        else if (arg == "--camera-side")
        {
            if (value == "back")
                options.cameraSideBack = true;
            else if (value == "front")
                options.cameraSideBack = false;
            else
            {
                *error = "--camera-side expects front or back";
                return false;
            }
            options.cameraSideSupplied = true;
        }
        else if (arg == "--camera-distance")
        {
            if (!parseFloat(value.c_str(), &options.cameraDistanceScale)
                || !(options.cameraDistanceScale > 0.0f) || options.cameraDistanceScale > 1000.0f)
            {
                *error = "--camera-distance expects a positive scale";
                return false;
            }
        }
        else if (arg == "--tan-half-fov")
        {
            if (!parseFloat(value.c_str(), &options.tanHalfFov)
                || !(options.tanHalfFov > 0.0f) || options.tanHalfFov > 1000.0f)
            {
                *error = "--tan-half-fov expects a positive value";
                return false;
            }
        }
        else if (arg == "--tex-filter-disable")
        {
            std::uint32_t flag = 0;
            if (!parseU32(value.c_str(), &flag) || flag > 1)
            {
                *error = "--tex-filter-disable expects 0 or 1";
                return false;
            }
            options.texFilterDisableSupplied = true;
            options.texFilterDisable = flag != 0;
            if (options.texFilterDisable)
                options.filterInputs = DisabledFilterTableInputs();
        }
        else if (arg == "--mip-filter-mode")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.mipFilterMode)
                || options.filterInputs.mipFilterMode > 3)
            {
                *error = "--mip-filter-mode expects 0..3";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--anisotropic-filter")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.anisotropicFilter))
            {
                *error = "--anisotropic-filter expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--aniso2x")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.aniso2x))
            {
                *error = "--aniso2x expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--aniso4x")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.aniso4x))
            {
                *error = "--aniso4x expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--linear-mipped-anisotropy")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.linearMippedAnisotropy))
            {
                *error = "--linear-mipped-anisotropy expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--linear-mipped-filter")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.linearMippedFilter))
            {
                *error = "--linear-mipped-filter expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else if (arg == "--linear-nonmipped-filter")
        {
            if (!parseU32(value.c_str(), &options.filterInputs.linearNonMippedFilter))
            {
                *error = "--linear-nonmipped-filter expects a number";
                return false;
            }
            options.filterInputsSupplied = true;
        }
        else
        {
            *error = "unknown argument: " + arg;
            return false;
        }
    }

    if (options.bundlePath.empty() && !options.selfTest)
    {
        *error = "--bundle is required";
        return false;
    }

    // No guessed neutral: the code-40 material colour is an input the bundle does not carry,
    // so a run that does not state it would be measuring a colour this file invented. Alpha
    // counts, because the pixel shader lerps a replacement alpha through it.
    if (!options.materialColorSupplied && !options.selfTest)
    {
        *error = "--material-color is required: the bundle does not carry the code-40 material "
                 "colour, and a neutral guessed here would make the pass depend on a value no "
                 "source recorded. Alpha is a real input (the pixel shader lerps a replacement "
                 "alpha through it), so state all four components, e.g. --material-color 1,1,1,0";
        return false;
    }
    if (options.texFilterDisableSupplied && options.texFilterDisable && options.filterInputsSupplied)
    {
        *error = "--tex-filter-disable 1 already fixes the filter inputs; do not also pass them";
        return false;
    }
    if (!options.selfTest && !options.texFilterDisableSupplied && !options.filterInputsSupplied)
    {
        // All-zero filter inputs would silently produce a POINT/POINT sampler, which is a
        // rendering choice this program is not entitled to make on its own.
        *error = "sampler filter inputs are required: pass --tex-filter-disable 1 (values fixed "
                 "by r_state.cpp) or the explicit --mip-filter-mode/--anisotropic-filter/... set";
        return false;
    }
    // No asset or capture is committed to the repository, so the replay refuses to write
    // its output inside the source tree.
    if (options.outputPath.find("KisakCOD") != std::string::npos)
    {
        *error = "--output must point outside the repository";
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
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* renderTarget = nullptr;
    IDirect3DSurface9* depthStencil = nullptr;

    void Release()
    {
        // Release order mirrors creation order; every pointer is nulled so a second
        // Release() on the same struct cannot double-free.
        IDirect3DSurface9** surfaces[2] = {&depthStencil, &renderTarget};
        for (IDirect3DSurface9** surface : surfaces)
        {
            if (*surface)
            {
                (*surface)->Release();
                *surface = nullptr;
            }
        }
        IDirect3DBaseTexture9** textures[1] = {reinterpret_cast<IDirect3DBaseTexture9**>(&texture)};
        for (IDirect3DBaseTexture9** entry : textures)
        {
            if (*entry)
            {
                (*entry)->Release();
                *entry = nullptr;
            }
        }
        if (indexBuffer)
        {
            indexBuffer->Release();
            indexBuffer = nullptr;
        }
        if (vertexBuffer)
        {
            vertexBuffer->Release();
            vertexBuffer = nullptr;
        }
        if (declaration)
        {
            declaration->Release();
            declaration = nullptr;
        }
        if (pixelShader)
        {
            pixelShader->Release();
            pixelShader = nullptr;
        }
        if (vertexShader)
        {
            vertexShader->Release();
            vertexShader = nullptr;
        }
    }
};

// The declaration the loader already proved: FLOAT4 position at 0, D3DCOLOR at 16,
// FLOAT4 texcoord at 20, transcribed from s_streamSourceInfo[VERTDECL_WORLD].
D3DVERTEXELEMENT9 MakeDeclarationElement(WORD stream, WORD offset, BYTE type, BYTE usage, BYTE usageIndex)
{
    D3DVERTEXELEMENT9 element{};
    element.Stream = stream;
    element.Offset = offset;
    element.Type = type;
    element.Method = D3DDECLMETHOD_DEFAULT;
    element.Usage = usage;
    element.UsageIndex = usageIndex;
    return element;
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
        std::string firstFailure = hrText(hr);
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device);
        if (FAILED(hr) || !device)
        {
            *error = "CreateDevice failed: hardware vertex processing " + firstFailure
                + ", software vertex processing " + hrText(hr);
            d3d->Release();
            return false;
        }
        std::printf("[device] hardware vertex processing unavailable (%s), using software\n",
            firstFailure.c_str());
    }

    *d3dOut = d3d;
    *deviceOut = device;
    *paramsOut = params;
    return true;
}

bool CreateShaderObjects(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    Resources* resources, std::string* error)
{
    HRESULT hr = device->CreateVertexShader(
        reinterpret_cast<const DWORD*>(bundle.vertex_shader_bytes.data()), &resources->vertexShader);
    if (FAILED(hr) || !resources->vertexShader)
    {
        *error = "CreateVertexShader failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->CreatePixelShader(
        reinterpret_cast<const DWORD*>(bundle.pixel_shader_bytes.data()), &resources->pixelShader);
    if (FAILED(hr) || !resources->pixelShader)
    {
        *error = "CreatePixelShader failed, hr=" + hrText(hr);
        return false;
    }

    D3DVERTEXELEMENT9 elements[4];
    elements[0] = MakeDeclarationElement(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    elements[1] = MakeDeclarationElement(0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
    elements[2] = MakeDeclarationElement(0, 20, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, 0);
    elements[3] = D3DDECL_END();

    hr = device->CreateVertexDeclaration(elements, &resources->declaration);
    if (FAILED(hr) || !resources->declaration)
    {
        *error = "CreateVertexDeclaration failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

bool CreateGeometry(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    Resources* resources, std::string* error)
{
    HRESULT hr = device->CreateVertexBuffer(bundle.slice_vertex_bytes, D3DUSAGE_DYNAMIC,
        0, D3DPOOL_DEFAULT, &resources->vertexBuffer, nullptr);
    if (FAILED(hr) || !resources->vertexBuffer)
    {
        *error = "CreateVertexBuffer failed, hr=" + hrText(hr);
        return false;
    }
    void* locked = nullptr;
    hr = resources->vertexBuffer->Lock(0, bundle.slice_vertex_bytes, &locked, D3DLOCK_DISCARD);
    if (FAILED(hr) || !locked)
    {
        *error = "vertex buffer Lock failed, hr=" + hrText(hr);
        return false;
    }
    std::memcpy(locked, bundle.vertex_bytes.data(), bundle.vertex_bytes.size());
    resources->vertexBuffer->Unlock();

    hr = device->CreateIndexBuffer(bundle.slice_index_bytes, D3DUSAGE_DYNAMIC,
        D3DFMT_INDEX16, D3DPOOL_DEFAULT, &resources->indexBuffer, nullptr);
    if (FAILED(hr) || !resources->indexBuffer)
    {
        *error = "CreateIndexBuffer failed, hr=" + hrText(hr);
        return false;
    }
    locked = nullptr;
    hr = resources->indexBuffer->Lock(0, bundle.slice_index_bytes, &locked, D3DLOCK_DISCARD);
    if (FAILED(hr) || !locked)
    {
        *error = "index buffer Lock failed, hr=" + hrText(hr);
        return false;
    }
    std::memcpy(locked, bundle.index_bytes.data(), bundle.index_bytes.size());
    resources->indexBuffer->Unlock();
    return true;
}

// Uploads one texture. A null chain means no texture was prepared, in which case a 1x1
// white texel stands in and the caller reports that substitution.
bool CreateTexture(const kisak::window::IwiMipChain* chain, IDirect3DDevice9* device,
    IDirect3DTexture9** textureOut, std::string* error)
{
    const bool haveChain = chain != nullptr && !chain->empty();
    const kisak::window::PixelExtent baseExtent = haveChain
        ? chain->front().Extent() : kisak::window::PixelExtent{1, 1};
    const std::uint32_t width = static_cast<std::uint32_t>(baseExtent.width);
    const std::uint32_t height = static_cast<std::uint32_t>(baseExtent.height);
    const std::uint32_t providedLevels = haveChain ? static_cast<std::uint32_t>(chain->size()) : 0u;

    // The chain is declared at its full depth so the sampler can address every level the
    // engine would have built, even when the preparer supplied fewer.
    std::uint32_t levels = 1;
    while ((width >> levels) != 0 || (height >> levels) != 0)
        ++levels;

    HRESULT hr = device->CreateTexture(width, height, levels, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, textureOut, nullptr);
    if (FAILED(hr) || !*textureOut)
    {
        *error = "CreateTexture failed, hr=" + hrText(hr);
        return false;
    }

    for (std::uint32_t level = 0; level < levels; ++level)
    {
        D3DLOCKED_RECT locked{};
        hr = (*textureOut)->LockRect(level, &locked, nullptr, 0);
        if (FAILED(hr))
        {
            *error = "texture LockRect(level " + std::to_string(level) + ") failed, hr=" + hrText(hr);
            return false;
        }

        const std::uint32_t levelWidth = std::max(1u, width >> level);
        const std::uint32_t levelHeight = std::max(1u, height >> level);
        const std::uint8_t* source = nullptr;
        std::uint32_t sourceWidth = 0;
        std::uint32_t sourceHeight = 0;
        bool replicate = false;
        if (!haveChain)
        {
            // No prepared texture: a valid surface still has to be bound, but white leaves
            // a vertex-colour shader's output unchanged by texel choice, which is exactly
            // why the caller reports this as an untextured result.
            static const std::uint8_t white[4] = {255, 255, 255, 255};
            source = white;
            sourceWidth = 1;
            sourceHeight = 1;
        }
        else if (level < providedLevels)
        {
            const kisak::window::PixelBufferView view = (*chain)[level].View();
            source = view.bytes.data();
            sourceWidth = static_cast<std::uint32_t>(view.width);
            sourceHeight = static_cast<std::uint32_t>(view.height);
            if (sourceWidth != levelWidth || sourceHeight != levelHeight)
            {
                *error = "level " + std::to_string(level) + " is " + std::to_string(sourceWidth) + "x"
                    + std::to_string(sourceHeight) + ", the chain expects " + std::to_string(levelWidth)
                    + "x" + std::to_string(levelHeight);
                (*textureOut)->UnlockRect(level);
                return false;
            }
        }
        else
        {
            // The decoder returned a shorter chain than the texture has levels. Replicating
            // the base level into the tail is stated in the report instead of being passed
            // off as sampled detail.
            const kisak::window::PixelBufferView view = chain->front().View();
            source = view.bytes.data();
            sourceWidth = static_cast<std::uint32_t>(view.width);
            sourceHeight = static_cast<std::uint32_t>(view.height);
            replicate = true;
        }

        for (std::uint32_t row = 0; row < levelHeight; ++row)
        {
            auto* destination = reinterpret_cast<std::uint32_t*>(
                static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch);
            for (std::uint32_t column = 0; column < levelWidth; ++column)
            {
                const std::uint32_t sourceColumn = std::min(column, sourceWidth - 1);
                const std::uint32_t sourceRow = std::min(row, sourceHeight - 1);
                const std::size_t offset = (static_cast<std::size_t>(sourceRow) * sourceWidth + sourceColumn) * 4u;
                const std::uint32_t red = source[offset + 0];
                const std::uint32_t green = source[offset + 1];
                const std::uint32_t blue = source[offset + 2];
                const std::uint32_t alpha = source[offset + 3];
                destination[column] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        (void)replicate;
        (*textureOut)->UnlockRect(level);
    }
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

    // A depth buffer is bound because the decoded state block both tests and writes depth.
    // Rendering with no depth surface would silently discard two decoded states.
    hr = device->CreateDepthStencilSurface(kRenderWidth, kRenderHeight, D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &resources->depthStencil, nullptr);
    if (FAILED(hr) || !resources->depthStencil)
    {
        *error = "CreateDepthStencilSurface failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

// sRGB conversion is test context, not decoded bundle state.
//
// The recorded state block uses the engine's GFXS0_/GFXS1_ bit layout, and neither of
// these two states has a bit there: R_ChangeState_0 never touches them, so the bundle
// cannot say what they were. D3D9 leaves both at their device defaults, and a sampler with
// SRGBTEXTURE set would convert the sampled 8-bit texel to linear before the shader saw
// it, which would silently contradict every value the CPU oracle predicts -- the oracle
// samples the uploaded texel bytes as they are.
//
// So both are disabled explicitly here, at the point where the frame is set up, and the
// report records that this is a stated test input rather than a captured one. Doing it in
// RenderOnce rather than once after device creation is deliberate: IDirect3DDevice9::Reset
// returns the device to its defaults, and the device-reset check re-enters this path.
bool DisableSrgbConversion(IDirect3DDevice9* device, std::string* error)
{
    HRESULT hr = device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    if (FAILED(hr))
    {
        *error = "SetSamplerState(D3DSAMP_SRGBTEXTURE) failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (FAILED(hr))
    {
        *error = "SetRenderState(D3DRS_SRGBWRITEENABLE) failed, hr=" + hrText(hr);
        return false;
    }
    return true;
}

// Applies the decoded state block. Each assignment comes from DecodedState, so nothing
// here reintroduces a default that the decode refused to produce.
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

    // GFXS1_POLYGON_OFFSET_1/_2 select D3DRS_SLOPESCALEDEPTHBIAS presets in the engine.
    // The replay binds the index-derived bias the state asks for and names it, rather
    // than silently leaving 0 in place.
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
    struct SamplerState
    {
        D3DSAMPLERSTATETYPE state;
        DWORD value;
    };
    const SamplerState entries[] = {
        {D3DSAMP_MINFILTER, states.minFilter},
        {D3DSAMP_MAGFILTER, states.magFilter},
        {D3DSAMP_MIPFILTER, states.mipFilter},
        {D3DSAMP_MAXANISOTROPY, states.maxAnisotropy},
        {D3DSAMP_ADDRESSU, states.addressU},
        {D3DSAMP_ADDRESSV, states.addressV},
        {D3DSAMP_ADDRESSW, states.addressW},
    };
    for (const SamplerState& entry : entries)
    {
        const HRESULT hr = device->SetSamplerState(samplerIndex, entry.state, entry.value);
        if (FAILED(hr))
        {
            *error = "SetSamplerState(" + std::to_string(static_cast<int>(entry.state)) + ", "
                + std::to_string(entry.value) + ") failed, hr=" + hrText(hr);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Constant upload, driven by the bundle's own routing.
// ---------------------------------------------------------------------------
struct UploadStats
{
    int vertexRows = 0;
    int pixelRows = 0;
    int texturesBound = 0;
    // No "skipped" list: the routing contract in world_surface_bundle.h refuses any argument
    // the context cannot supply, so there is no such thing as a constant this run declines to
    // upload. A skip would have made the report read as a pass while a material input was
    // silently absent.
};

bool ApplyArguments(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    const world_surface_context::Context& context, IDirect3DTexture9* texture,
    const world_surface_bundle::ConstantCaps& caps, UploadStats* stats, std::string* error)
{
    for (const world_surface_bundle::Argument& argument : bundle.arguments)
    {
        if (argument.has_texture)
        {
            // The material texture argument names the engine texture slot in `index` and
            // the sampler to bind it to in `dest`. The replay has exactly one prepared
            // image, so any other slot is reported rather than bound to the wrong sampler.
            if (argument.dest != 0)
            {
                *error = "material texture argument binds sampler " + std::to_string(argument.dest)
                    + "; this replay binds sampler 0 only";
                return false;
            }
            HRESULT hr = device->SetTexture(argument.dest, texture);
            if (FAILED(hr))
            {
                *error = "SetTexture failed, hr=" + hrText(hr);
                return false;
            }
            // Read the binding back from the device. SetTexture succeeding and the sampler
            // actually holding the image are different claims, and only the second one makes
            // the texture control meaningful.
            IDirect3DBaseTexture9* bound = nullptr;
            hr = device->GetTexture(argument.dest, &bound);
            if (FAILED(hr))
            {
                *error = "GetTexture(" + std::to_string(argument.dest) + ") failed, hr=" + hrText(hr);
                return false;
            }
            const bool same = bound == reinterpret_cast<IDirect3DBaseTexture9*>(texture);
            if (bound)
                bound->Release();
            if (!same)
            {
                *error = "sampler " + std::to_string(argument.dest)
                    + " does not hold the uploaded texture after SetTexture";
                return false;
            }
            ++stats->texturesBound;
            continue;
        }

        // The same pure contract the loader enforced, re-run here. The row count below is
        // handed to SetShaderConstantF, so this is the call that has to hold for the upload to
        // stay inside the context array it points at.
        {
            std::string routeError;
            if (!world_surface_bundle::ValidateArgumentRoute(argument, caps, &routeError))
            {
                *error = "argument in group '" + argument.group + "': " + routeError;
                return false;
            }
        }

        const float* source = nullptr;
        bool pixelStage = false;

        switch (argument.code_index)
        {
        case world_surface_context::kCodeTransposeWorldMatrix:
            source = &context.vsTransposeWorld.rows[0][0];
            break;
        case world_surface_context::kCodeTransposeViewProjectionMatrix:
            source = &context.vsTransposeViewProjection.rows[0][0];
            break;
        case world_surface_context::kCodeFog:
            source = context.vsFog;
            break;
        case world_surface_context::kCodeFogColor:
            source = context.psFogColor;
            pixelStage = true;
            break;
        case world_surface_context::kCodeMaterialColor:
            source = context.psMaterialColor;
            pixelStage = true;
            break;
        default:
            // Unreachable: the contract above rejects every other code. Kept as a refusal
            // rather than a skip so that a code added to the table without a source here fails
            // the run instead of quietly drawing without its constant.
            *error = "internal: constant code " + std::to_string(argument.code_index)
                + " passed the routing contract but has no source in this context";
            return false;
        }

        // The contract derives the stage from the argument's recorded type, the switch above
        // derives it from the code. Both are transcribed from engine tables, so a disagreement
        // means one of the two transcriptions is wrong: refuse instead of uploading to the
        // stage the shader is not reading.
        const bool contractPixelStage = world_surface_bundle::FindConstantRoute(argument.code_index)->type
            == world_surface_bundle::kArgTypeCodePixelConst;
        if (contractPixelStage != pixelStage)
        {
            *error = "internal: code " + std::to_string(argument.code_index) + " is a "
                + (pixelStage ? "pixel" : "vertex") + "-stage upload here, but the argument type "
                  "in the bundle says the other stage";
            return false;
        }

        const std::uint32_t destRegister = argument.dest;
        const HRESULT hr = pixelStage
            ? device->SetPixelShaderConstantF(destRegister, source, argument.row_count)
            : device->SetVertexShaderConstantF(destRegister, source, argument.row_count);
        if (FAILED(hr))
        {
            *error = "SetShaderConstantF(code " + std::to_string(argument.code_index) + ", dest "
                + std::to_string(destRegister) + ") failed, hr=" + hrText(hr);
            return false;
        }
        if (pixelStage)
            stats->pixelRows += static_cast<int>(argument.row_count);
        else
            stats->vertexRows += static_cast<int>(argument.row_count);
    }
    return true;
}

// ---------------------------------------------------------------------------
// How the recorded geometry is issued. Single is the recorded draw: one call covering every
// triangle of the slice. PerTriangle issues one single-triangle call per triangle in the
// recorded order, and Reverse issues them last-first. Both split modes address the same indices
// through StartIndex = 3 * k, so the index buffer is never rewritten and the reversal cannot be
// explained by an edited buffer. PerTriangle exists to show that splitting the call is not
// itself a change: it has to reproduce the recorded image exactly, which is what leaves
// Reverse, and Reverse with the depth test off, as the only remaining differences.
enum class DrawOrder
{
    Single = 0,
    PerTriangle,
    Reverse,
};

// Every frame is built from this. A control differs from the baseline in exactly the inputs it
// names here, and every control goes through CreateAll -> ApplyFrameState -> RenderOnce.
struct FrameInputs
{
    const world_surface_context::Context* context = nullptr;
    const kisak::window::IwiMipChain* mips = nullptr; // nullptr -> 1x1 white stand-in
    const SamplerStates* sampler = nullptr;           // nullptr -> no sampler decode applied
    bool issueDraw = true;
    DrawOrder drawOrder = DrawOrder::Single;

    // Frame-level depth-test override, applied in RenderOnce after the decoded state block has
    // been uploaded. The recorded state stays what every other frame runs with, and the
    // device-reset path is untouched. D3D9 ignores ZWRITEENABLE while ZENABLE is FALSE, so this
    // also stops the frame writing depth.
    bool overrideDepthTest = false;
    bool depthTestEnabled = false;
};

// ---------------------------------------------------------------------------
// One frame: draw, read back, and turn the SYSTEMEMEM lock into an RGB image.
// ---------------------------------------------------------------------------
bool RenderOnce(IDirect3DDevice9* device, const world_surface_bundle::Bundle& bundle,
    Resources* resources, Image* image, const FrameInputs& inputs, std::string* error)
{
    HRESULT hr = device->SetRenderTarget(0, resources->renderTarget);
    if (FAILED(hr))
    {
        *error = "SetRenderTarget failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetDepthStencilSurface(resources->depthStencil);
    if (FAILED(hr))
    {
        *error = "SetDepthStencilSurface failed, hr=" + hrText(hr);
        return false;
    }

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = kRenderWidth;
    viewport.Height = kRenderHeight;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    hr = device->SetViewport(&viewport);
    if (FAILED(hr))
    {
        *error = "SetViewport failed, hr=" + hrText(hr);
        return false;
    }

    const D3DCOLOR clearColor = D3DCOLOR_XRGB(kClearRed, kClearGreen, kClearBlue);
    // Test context, applied before anything is drawn: see DisableSrgbConversion. Both the
    // sampler's sRGB decode and the target's sRGB write are off, which is what lets the
    // CPU oracle predict the uploaded texel bytes directly.
    if (!DisableSrgbConversion(device, error))
        return false;

    // Depth-test override, after the decoded block has been applied and before anything is
    // cleared or drawn. Only the frame that asks for it sees this value.
    if (inputs.overrideDepthTest)
    {
        hr = device->SetRenderState(
            D3DRS_ZENABLE, inputs.depthTestEnabled ? D3DZB_TRUE : D3DZB_FALSE);
        if (FAILED(hr))
        {
            *error = "SetRenderState(D3DRS_ZENABLE override) failed, hr=" + hrText(hr);
            return false;
        }
    }

    hr = device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
    if (FAILED(hr))
    {
        *error = "Clear failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->BeginScene();
    if (FAILED(hr))
    {
        *error = "BeginScene failed, hr=" + hrText(hr);
        return false;
    }

    hr = device->SetVertexDeclaration(resources->declaration);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetVertexDeclaration failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetStreamSource(0, resources->vertexBuffer, 0, bundle.vertex_stride);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetStreamSource failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetIndices(resources->indexBuffer);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetIndices failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetVertexShader(resources->vertexShader);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetVertexShader failed, hr=" + hrText(hr);
        return false;
    }
    hr = device->SetPixelShader(resources->pixelShader);
    if (FAILED(hr))
    {
        device->EndScene();
        *error = "SetPixelShader failed, hr=" + hrText(hr);
        return false;
    }

    // Surface-local indices address the whole slice, so the draw covers every translated
    // vertex and lets the index buffer alone decide which ones the triangle uses.
    //
    // issueDraw=false is the skip-draw control: every binding, state and constant above is
    // identical, and only the DrawIndexedPrimitive call is missing, so the readback must be
    // the clear colour exactly.
    if (inputs.issueDraw)
    {
        if (inputs.drawOrder == DrawOrder::Single)
        {
            hr = device->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST, 0, 0, bundle.vertex_count, 0, bundle.tri_count);
            if (FAILED(hr))
            {
                device->EndScene();
                *error = "DrawIndexedPrimitive failed, hr=" + hrText(hr);
                return false;
            }
        }
        else
        {
            // One single-triangle call per triangle, walking the slice forwards or backwards.
            // StartIndex = 3 * k names triangle k's three indices, so the reversal is a change
            // of draw order and nothing else: no index is rewritten, reordered or duplicated.
            for (std::uint32_t step = 0; step < bundle.tri_count; ++step)
            {
                const std::uint32_t triangle = inputs.drawOrder == DrawOrder::Reverse
                    ? bundle.tri_count - 1u - step
                    : step;
                hr = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, bundle.vertex_count,
                    3u * triangle, 1);
                if (FAILED(hr))
                {
                    device->EndScene();
                    *error = "DrawIndexedPrimitive(triangle " + std::to_string(triangle)
                        + ", StartIndex " + std::to_string(3u * triangle) + ") failed, hr="
                        + hrText(hr);
                    return false;
                }
            }
        }
    }

    hr = device->EndScene();
    if (FAILED(hr))
    {
        *error = "EndScene failed, hr=" + hrText(hr);
        return false;
    }

    // Readback through a SYSTEMMEM offscreen surface: the render target itself is not
    // lockable, and copying through this path also proves the target really holds the
    // drawn frame rather than an untouched clear.
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
            const std::size_t offset = (static_cast<std::size_t>(row) * kRenderWidth + column) * 3u;
            image->rgb[offset + 0] = static_cast<std::uint8_t>((texel >> 16) & 0xFFu);
            image->rgb[offset + 1] = static_cast<std::uint8_t>((texel >> 8) & 0xFFu);
            image->rgb[offset + 2] = static_cast<std::uint8_t>(texel & 0xFFu);

            // The A8R8G8B8 readback is the same texel with alpha kept. Written in the same
            // pass so the two views cannot disagree.
            image->rgba[offset / 3u * 4u + 0u] = image->rgb[offset + 0];
            image->rgba[offset / 3u * 4u + 1u] = image->rgb[offset + 1];
            image->rgba[offset / 3u * 4u + 2u] = image->rgb[offset + 2];
            image->rgba[offset / 3u * 4u + 3u] = static_cast<std::uint8_t>((texel >> 24) & 0xFFu);
        }
    }

    systemSurface->UnlockRect();
    systemSurface->Release();
    return true;
}

// ---------------------------------------------------------------------------
// Image measurements used by the checks and the controls.
// ---------------------------------------------------------------------------
struct ImageStats
{
    std::size_t coveredPixels = 0;
    std::size_t distinctColors = 0;
    double meanLuminance = 0.0;
    double luminanceVariance = 0.0;
    // Brighter of the covered pixels' strongest channel. The sentinel check uses this to
    // refuse a pass over a region that was drawn but produced nothing: a black image and a
    // correct image both "cover pixels", so coverage alone cannot tell them apart.
    unsigned brightestChannel = 0;
};

bool IsClearPixel(const Image& image, std::size_t offset)
{
    return image.rgb[offset + 0] == kClearRed && image.rgb[offset + 1] == kClearGreen
        && image.rgb[offset + 2] == kClearBlue;
}

ImageStats Measure(const Image& image)
{
    ImageStats stats{};
    // Collected up front and reduced once: a per-pixel linear search would be quadratic
    // on an image that happens to hold tens of thousands of distinct colours.
    std::vector<std::uint32_t> seen;
    seen.reserve(4096);

    double sum = 0.0;
    double sumSquares = 0.0;
    for (std::uint32_t row = 0; row < image.height; ++row)
    {
        for (std::uint32_t column = 0; column < image.width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * image.width + column) * 3u;
            if (IsClearPixel(image, offset))
                continue;
            ++stats.coveredPixels;
            for (int channel = 0; channel < 3; ++channel)
            {
                const unsigned value = image.rgb[offset + static_cast<std::size_t>(channel)];
                if (value > stats.brightestChannel)
                    stats.brightestChannel = value;
            }
            seen.push_back((static_cast<std::uint32_t>(image.rgb[offset + 0]) << 16)
                | (static_cast<std::uint32_t>(image.rgb[offset + 1]) << 8)
                | image.rgb[offset + 2]);

            const double luminance = 0.2126 * image.rgb[offset + 0]
                + 0.7152 * image.rgb[offset + 1] + 0.0722 * image.rgb[offset + 2];
            sum += luminance;
            sumSquares += luminance * luminance;
        }
    }

    std::sort(seen.begin(), seen.end());
    stats.distinctColors = static_cast<std::size_t>(
        std::unique(seen.begin(), seen.end()) - seen.begin());

    if (stats.coveredPixels > 0)
    {
        stats.meanLuminance = sum / static_cast<double>(stats.coveredPixels);
        const double meanSquare = sumSquares / static_cast<double>(stats.coveredPixels);
        stats.luminanceVariance = meanSquare - stats.meanLuminance * stats.meanLuminance;
        if (stats.luminanceVariance < 0.0)
            stats.luminanceVariance = 0.0;
    }
    return stats;
}

std::size_t CountDifferingPixels(const Image& a, const Image& b)
{
    if (a.width != b.width || a.height != b.height)
        return a.rgb.size() + b.rgb.size();
    std::size_t differing = 0;
    for (std::size_t index = 0; index < a.rgb.size(); index += 3)
        if (a.rgb[index + 0] != b.rgb[index + 0] || a.rgb[index + 1] != b.rgb[index + 1]
            || a.rgb[index + 2] != b.rgb[index + 2])
            ++differing;
    return differing;
}

std::uint32_t SampleChannel(const Image& image, std::uint32_t column, std::uint32_t row, int channel)
{
    const std::size_t offset = (static_cast<std::size_t>(row) * image.width + column) * 3u;
    return image.rgb[offset + static_cast<std::size_t>(channel)];
}

// ---------------------------------------------------------------------------
// One frame, built and applied: every resource the bundle describes, then the recorded state,
// sampler and constants. Every control enters here, so a control differs from the baseline in
// exactly the input it is named for.
// ---------------------------------------------------------------------------
bool CreateAll(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    const FrameInputs& inputs, Resources* resources, std::string* error)
{
    return CreateShaderObjects(bundle, device, resources, error)
        && CreateGeometry(bundle, device, resources, error)
        && CreateTexture(inputs.mips, device, &resources->texture, error)
        && CreateTargets(device, resources, error);
}

bool ApplyFrameState(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    const DecodedState& state, const FrameInputs& inputs, IDirect3DTexture9* texture,
    UploadStats* stats, std::string* error)
{
    if (!inputs.context)
    {
        *error = "internal: frame has no constant context";
        return false;
    }

    // Ask the device how large its constant files are rather than assuming the caps the
    // contract defaults to. D3D9 reports the vertex count (MaxVertexShaderConst) but has no
    // field for the pixel count, whose size is fixed by the pixel shader version: the replay
    // creates PS 3.0, so 224 is the whole file. A zero vertex report would refuse every
    // upload for a reason that is not about the bundle, so it keeps the D3D9 default and the
    // report states the numbers actually used.
    D3DCAPS9 deviceCaps{};
    const HRESULT capsHr = device->GetDeviceCaps(&deviceCaps);
    if (FAILED(capsHr))
    {
        *error = "GetDeviceCaps failed, hr=" + hrText(capsHr);
        return false;
    }
    world_surface_bundle::ConstantCaps caps;
    if (deviceCaps.MaxVertexShaderConst != 0)
        caps.vertexRegisters = deviceCaps.MaxVertexShaderConst;

    if (!ApplyStateBlock(device, state, error))
        return false;
    if (inputs.sampler && !ApplySampler(device, 0, *inputs.sampler, error))
        return false;
    return ApplyArguments(bundle, device, *inputs.context, texture, caps, stats, error);
}

bool RunFrame(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    const DecodedState& state, const FrameInputs& inputs, Image* image, UploadStats* stats,
    std::string* error)
{
    Resources resources;
    bool ok = CreateAll(bundle, device, inputs, &resources, error);
    if (ok)
        ok = ApplyFrameState(bundle, device, state, inputs, resources.texture, stats, error);
    if (ok)
        ok = RenderOnce(device, bundle, &resources, image, inputs, error);
    resources.Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Device Reset.
//
// D3D9 refuses Reset while any default-pool resource is alive, and this frame's vertex
// buffer, index buffer, render target and depth surface are all default pool. So the
// sequence below is itself the check: unbind, release, Reset, rebuild, reapply, redraw.
// A retained reference shows up as a failed Reset instead of a green "resource cycle".
// ---------------------------------------------------------------------------
bool UnbindAll(IDirect3DDevice9* device, std::string* error)
{
    struct Step
    {
        const char* name;
        HRESULT hr;
    };
    Step steps[8];

    // DXVK refuses to unbind render target 0 outright (D3DERR_INVALIDCALL), so the target is
    // handed back to the swap chain's back buffer instead of being set to null. That is what
    // a D3D9 application must do before Reset anyway: the device may not be left with a
    // default-pool surface it cannot release by itself.
    IDirect3DSurface9* backBuffer = nullptr;
    const HRESULT backBufferResult = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    // Depth surface first: it pairs with the render target and must go before it.
    steps[0] = {"depth surface", device->SetDepthStencilSurface(nullptr)};
    steps[1] = {"stream 0", device->SetStreamSource(0, nullptr, 0, 0)};
    steps[2] = {"indices", device->SetIndices(nullptr)};
    steps[3] = {"texture 0", device->SetTexture(0, nullptr)};
    steps[4] = {"vertex shader", device->SetVertexShader(nullptr)};
    steps[5] = {"pixel shader", device->SetPixelShader(nullptr)};
    steps[6] = {"declaration", device->SetVertexDeclaration(nullptr)};
    steps[7] = {"render target 0 (back buffer)",
        FAILED(backBufferResult) ? backBufferResult : device->SetRenderTarget(0, backBuffer)};

    bool ok = true;
    for (const Step& step : steps)
    {
        if (FAILED(step.hr))
        {
            *error = std::string("unbinding ") + step.name + " failed, hr=" + hrText(step.hr);
            ok = false;
            break;
        }
    }
    if (FAILED(backBufferResult))
        *error = "GetBackBuffer for the reset preamble failed, hr=" + hrText(backBufferResult);
    if (backBuffer)
        backBuffer->Release();
    return ok && SUCCEEDED(backBufferResult);
}

// Builds and draws a frame, tears every default-pool object down, calls Reset, then
// rebuilds, reapplies the complete state/constant set and draws again into *after.
bool ResetAndRebuild(const world_surface_bundle::Bundle& bundle, IDirect3DDevice9* device,
    const D3DPRESENT_PARAMETERS& params, const DecodedState& state, const FrameInputs& inputs,
    Image* after, UploadStats* stats, std::string* error)
{
    Resources resources;
    if (!CreateAll(bundle, device, inputs, &resources, error))
        return false;
    if (!ApplyFrameState(bundle, device, state, inputs, resources.texture, stats, error))
    {
        resources.Release();
        return false;
    }

    if (!UnbindAll(device, error))
    {
        resources.Release();
        return false;
    }
    resources.Release();

    D3DPRESENT_PARAMETERS resetParams = params;
    const HRESULT hr = device->Reset(&resetParams);
    if (FAILED(hr))
    {
        *error = "IDirect3DDevice9::Reset failed, hr=" + hrText(hr)
            + ": a default-pool resource or a bound object was still referenced";
        return false;
    }

    if (!CreateAll(bundle, device, inputs, &resources, error))
        return false;
    if (!ApplyFrameState(bundle, device, state, inputs, resources.texture, stats, error))
    {
        resources.Release();
        return false;
    }
    const bool rendered = RenderOnce(device, bundle, &resources, after, inputs, error);
    resources.Release();
    return rendered;
}

// ---------------------------------------------------------------------------
// Camera derivation from the referenced triangle.
// ---------------------------------------------------------------------------
struct CameraDerivation
{
    world_surface_context::CameraInputs camera{};
    Vec3 centroid{};
    Vec3 normal{};
    float longestEdge = 0.0f;
    std::uint16_t indexValues[3] = {0, 0, 0};
};

// The camera the caller stated, used as given. Nothing here reads the geometry: the eye and the
// target are the numbers from the command line, the basis is built from eye -> target, and the
// surface's index order is never consulted. That is what lets a surface with more than one
// triangle be replayed without editing its indices, and it is also why the winding probe in main
// does not run for this camera: the caller said where to look, so the replay does not move the
// eye afterwards.
bool DeriveExplicitCamera(const Options& options, CameraDerivation* derivation, std::string* error)
{
    const Vec3 direction = Subtract(options.cameraTarget, options.cameraEye);
    const float distance = Length(direction);
    if (!(distance > 0.0f))
    {
        *error = "--camera-eye and --camera-target are the same point";
        return false;
    }

    world_surface_context::CameraInputs& camera = derivation->camera;
    for (int component = 0; component < 3; ++component)
        camera.origin[component] = options.cameraEye.value[component];

    if (!BuildCameraBasis(direction, &camera, error))
        return false;

    camera.tanHalfFovX = options.tanHalfFov;
    camera.tanHalfFovY = options.tanHalfFov;
    // No captured near plane exists in either mode. Here it is a fixed fraction of the stated
    // viewing distance, which stays well in front of the eye for any camera a caller states; the
    // value is printed in the report rather than being assumed silently.
    camera.zNear = std::max(0.01f, distance * 0.05f);

    // The target stands in for the derived centroid in the two places that read it: the report,
    // and the look-away control, which needs the point the camera is aimed at. `normal` and
    // `indexValues` stay at zero because no triangle was consulted.
    derivation->centroid = options.cameraTarget;
    derivation->longestEdge = distance;
    return true;
}

bool DeriveCamera(const world_surface_bundle::Bundle& bundle, const Options& options,
    CameraDerivation* derivation, std::string* error)
{
    if (options.cameraEyeSupplied != options.cameraTargetSupplied)
    {
        *error = "--camera-eye and --camera-target must be given together: one without the other "
                 "would leave half the camera to be derived from the geometry";
        return false;
    }
    if (options.cameraEyeSupplied)
        return DeriveExplicitCamera(options, derivation, error);

    if (bundle.index_count != 3)
    {
        *error = "camera derivation expects exactly one triangle, the bundle has "
            + std::to_string(bundle.index_count)
            + " indices; a surface with more triangles needs --camera-eye and --camera-target, "
              "and its indices are not edited to fit a derived camera";
        return false;
    }

    for (int corner = 0; corner < 3; ++corner)
    {
        const std::uint16_t index = world_surface_bundle::ReadIndex(bundle, static_cast<std::uint32_t>(corner));
        derivation->indexValues[corner] = index;
        if (index >= bundle.vertex_count)
        {
            *error = "index " + std::to_string(index) + " addresses outside the " + std::to_string(bundle.vertex_count)
                + "-vertex slice";
            return false;
        }
    }
    if (derivation->indexValues[0] == derivation->indexValues[1]
        || derivation->indexValues[1] == derivation->indexValues[2]
        || derivation->indexValues[0] == derivation->indexValues[2])
    {
        *error = "the referenced triangle repeats a vertex index";
        return false;
    }

    Vec3 corners[3];
    for (int corner = 0; corner < 3; ++corner)
    {
        const world_surface_bundle::WorldVertex vertex
            = world_surface_bundle::ReadVertex(bundle, derivation->indexValues[corner]);
        for (int component = 0; component < 3; ++component)
        {
            corners[corner].value[component] = vertex.position[component];
            if (!(vertex.position[component] == vertex.position[component]))
            {
                *error = "the referenced triangle has a non-finite position component";
                return false;
            }
        }
    }

    const Vec3 edge0 = Subtract(corners[1], corners[0]);
    const Vec3 edge1 = Subtract(corners[2], corners[0]);
    Vec3 normal{};
    if (!Normalize(Cross(edge0, edge1), &normal))
    {
        *error = "the referenced triangle is degenerate: its winding normal has zero length";
        return false;
    }

    derivation->longestEdge = std::max(Length(edge0), std::max(Length(edge1), Length(Subtract(corners[2], corners[1]))));
    if (!(derivation->longestEdge > 0.0f))
    {
        *error = "the referenced triangle has zero extent";
        return false;
    }

    for (int component = 0; component < 3; ++component)
        derivation->centroid.value[component] =
            (corners[0].value[component] + corners[1].value[component] + corners[2].value[component]) / 3.0f;
    derivation->normal = normal;

    // The eye sits on the winding normal side by default, so the triangle is the face the
    // index order names. --camera-side back selects the other side explicitly; the run
    // never flips the camera on its own to make coverage appear.
    const float side = options.cameraSideBack ? -1.0f : 1.0f;
    const float distance = derivation->longestEdge * options.cameraDistanceScale;

    world_surface_context::CameraInputs& camera = derivation->camera;
    Vec3 eye{};
    for (int component = 0; component < 3; ++component)
    {
        eye.value[component] = derivation->centroid.value[component] + normal.value[component] * distance * side;
        camera.origin[component] = eye.value[component];
    }

    const Vec3 forward = Subtract(derivation->centroid, eye);
    if (!BuildCameraBasis(forward, &camera, error))
        return false;

    camera.tanHalfFovX = options.tanHalfFov;
    camera.tanHalfFovY = options.tanHalfFov;
    camera.zNear = std::max(0.01f, distance * 0.05f);
    return true;
}

// Signed area of the referenced triangle in the camera's own basis (x right, y up), which is
// the same basis the uploaded constants are built from. A perspective projection with
// positive x/y scales preserves orientation, and all three corners are required to have
// z > 0 here, so this is the screen-space winding up to a uniform scale.
//
// D3D9's front face is clockwise on screen, and the framebuffer's y axis points down, so a
// front-facing triangle has a POSITIVE area in this y-up basis.
bool CameraSpaceSignedArea(const world_surface_bundle::Bundle& bundle,
    const world_surface_context::CameraInputs& camera, const CameraDerivation& derivation,
    double* area, std::string* error)
{
    double screen[3][2];
    for (int corner = 0; corner < 3; ++corner)
    {
        const world_surface_bundle::WorldVertex vertex
            = world_surface_bundle::ReadVertex(bundle, derivation.indexValues[corner]);
        double relative[3];
        for (int axis = 0; axis < 3; ++axis)
            relative[axis] = static_cast<double>(vertex.position[axis]) - camera.origin[axis];

        const double depth = relative[0] * camera.forward[0] + relative[1] * camera.forward[1]
            + relative[2] * camera.forward[2];
        if (!(depth > 0.0))
        {
            *error = "corner " + std::to_string(corner) + " is behind the eye (depth = "
                + std::to_string(depth) + ")";
            return false;
        }
        screen[corner][0] = relative[0] * camera.left[0] + relative[1] * camera.left[1]
            + relative[2] * camera.left[2];
        screen[corner][1] = relative[0] * camera.up[0] + relative[1] * camera.up[1]
            + relative[2] * camera.up[2];
    }

    *area = 0.5 * ((screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1])
        - (screen[2][0] - screen[0][0]) * (screen[1][1] - screen[0][1]));
    return true;
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------
struct Report
{
    std::vector<std::string> lines;
    int checksPassed = 0;
    int checksFailed = 0;
    // Lines already written to stdout. Flush is incremental so the report can be streamed
    // as it is produced, which is how the oracle's prediction is shown to precede the frame
    // it predicts rather than being reconstructed after it.
    std::size_t flushedLines = 0;

    void Line(const std::string& text) { lines.push_back(text); }

    bool Check(const char* name, bool condition, const std::string& detail)
    {
        lines.push_back(format("[check] %-26s %-4s %s", name, condition ? "OK" : "FAIL", detail.c_str()));
        if (condition)
            ++checksPassed;
        else
            ++checksFailed;
        return condition;
    }

    void Flush()
    {
        for (; flushedLines < lines.size(); ++flushedLines)
            std::printf("%s\n", lines[flushedLines].c_str());
    }

    bool WriteTo(const std::string& path) const
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (!file)
            return false;
        for (const std::string& line : lines)
            std::fprintf(file, "%s\n", line.c_str());
        return std::fclose(file) == 0;
    }
};

std::string StateBitsDescription(std::uint32_t bits)
{
    return format("0x%08X", bits);
}

// ---------------------------------------------------------------------------
// Self-test of the decode tables this file transcribes. No bundle, no device: it exists so
// the cull and sampler mappings can be checked against the engine's tables on any machine,
// including one with no GPU. Run with --self-test.
// ---------------------------------------------------------------------------
bool ExpectEqual(const char* what, long long actual, long long expected)
{
    const bool ok = actual == expected;
    std::printf("[selftest] %-46s %-4s actual=%lld expected=%lld\n", what, ok ? "OK" : "FAIL",
        actual, expected);
    return ok;
}

int RunSelfTest()
{
    bool ok = true;

    // s_cullTable_30 = {0, 1, 3, 2}. Encoded values are GFXS0_CULL_NONE 0x4000 -> 1,
    // BACK 0x8000 -> 2, FRONT 0xC000 -> 3; slot 0 is not a D3DCULL.
    {
        D3DCULL cull = D3DCULL_NONE;
        ok &= ExpectEqual("cull encoded 0 rejected", DecodeCull(0, &cull) ? 1 : 0, 0);
        ok &= ExpectEqual("cull encoded 1 -> D3DCULL_NONE",
            (DecodeCull(1, &cull) ? static_cast<int>(cull) : -1), D3DCULL_NONE);
        ok &= ExpectEqual("cull encoded 2 -> D3DCULL_CCW",
            (DecodeCull(2, &cull) ? static_cast<int>(cull) : -1), D3DCULL_CCW);
        ok &= ExpectEqual("cull encoded 3 -> D3DCULL_CW",
            (DecodeCull(3, &cull) ? static_cast<int>(cull) : -1), D3DCULL_CW);
    }

    // r_texFilterDisable=1 fixes the filter entry at 0x1101 for table index 19: one
    // anisotropy sample, POINT minification and magnification, and no mip filtering.
    {
        const FilterTableInputs inputs = DisabledFilterTableInputs();
        std::string error;
        std::uint32_t decoded = 0;
        ok &= ExpectEqual("sampler index 19 decodes",
            DecodeSamplerState(19, inputs, &decoded, &error) ? 1 : 0, 1);
        const SamplerStates states = SplitSamplerDword(decoded);
        ok &= ExpectEqual("mip filter is bits 16-19 (NONE)",
            static_cast<int>(states.mipFilter), D3DTEXF_NONE);
        ok &= ExpectEqual("low byte is anisotropy, not a filter",
            static_cast<int>(states.maxAnisotropy), 1);
        ok &= ExpectEqual("min filter is POINT", static_cast<int>(states.minFilter), D3DTEXF_POINT);
        ok &= ExpectEqual("mag filter is POINT", static_cast<int>(states.magFilter), D3DTEXF_POINT);
        ok &= ExpectEqual("address U is WRAP", static_cast<int>(states.addressU), D3DTADDRESS_WRAP);
        ok &= ExpectEqual("address V is WRAP", static_cast<int>(states.addressV), D3DTADDRESS_WRAP);
        ok &= ExpectEqual("address W is WRAP", static_cast<int>(states.addressW), D3DTADDRESS_WRAP);
    }

    // A different r_texFilterMipMode must move the mip bits, and only those bits.
    {
        FilterTableInputs inputs = DisabledFilterTableInputs();
        inputs.mipFilterMode = 0; // s_mipFilterTable[0][2] = D3DTEXF_LINEAR
        std::string error;
        std::uint32_t decoded = 0;
        ok &= ExpectEqual("sampler index 19 decodes with mip mode 0",
            DecodeSamplerState(19, inputs, &decoded, &error) ? 1 : 0, 1);
        const SamplerStates states = SplitSamplerDword(decoded);
        ok &= ExpectEqual("mip mode 0 gives LINEAR mip",
            static_cast<int>(states.mipFilter), D3DTEXF_LINEAR);
        ok &= ExpectEqual("mip mode 0 leaves anisotropy at 1",
            static_cast<int>(states.maxAnisotropy), 1);
    }

    // Indices 24..31 are outside the 24 entries the engine fills, and the transcribed
    // three-column table has no fourth column to read.
    {
        const FilterTableInputs inputs = DisabledFilterTableInputs();
        std::string error;
        std::uint32_t decoded = 0;
        ok &= ExpectEqual("raw sampler index 24 rejected",
            DecodeSamplerState(24, inputs, &decoded, &error) ? 1 : 0, 0);
        ok &= ExpectEqual("raw sampler index 31 rejected",
            DecodeSamplerState(31, inputs, &decoded, &error) ? 1 : 0, 0);
        ok &= ExpectEqual("raw sampler 0xFF rejected",
            DecodeSamplerState(0xFF, inputs, &decoded, &error) ? 1 : 0, 0);
    }

    std::printf("[selftest] %s\n", ok ? "PASS" : "FAIL");
    return ok ? ExitPass : ExitFailure;
}

// ---------------------------------------------------------------------------
// The CPU pixel oracle, as this replay uses it.
//
// The prediction is built here, from the bundle's vertex bytes, the constant payload this
// run uploads and the mip0 texels the decoder returned, and it is printed before the device
// exists. The readback is compared against it afterwards. Nothing in this path reads a
// rendered image, so the comparison cannot be satisfied by construction, and the
// expectations are not revised once the frame has been seen.
//
// Scope: the audited fixture only. world_surface_oracle.h transcribes one vs_3_0/ps_3_0
// pair and reduces its pixel stage for all-white vertex colours, material (1,1,1,0), fog
// colour zero and a disabled fog vector. A run that changes any of those is outside that
// reduction, and CheckBaseline refuses it by name instead of predicting something nearby.
// ---------------------------------------------------------------------------
struct OracleDeclaration
{
    bool available = false;
    std::string reason;
    world_surface_oracle::ProgramCheck programs;
    // The facts of the first triangle. Every triangle is gated; a later one that leaves the
    // audited fixture refuses the whole declaration and is named in `reason`.
    world_surface_oracle::BaselineFacts baseline;
    world_surface_oracle::Selection selection;
    // The geometry the selection was made from, and the vertex outputs behind it: the corner
    // output of triangle t, corner c is outputs[3 * t + c].
    std::vector<world_surface_oracle::TriangleInput> triangles;
    std::vector<world_surface_oracle::VertexOutput> outputs;
    world_surface_oracle::PixelConstants pixelConstants{};
    world_surface_oracle::Mip0View mip0{};
};

// The uploaded payload, read straight out of the ConstantRows the context already built.
// context.vsTransposeViewProjection is VS c0..c3 and context.vsTransposeWorld is c4..c7,
// both already transposed by R_GetCodeMatrix, so nothing is transposed again here.
world_surface_oracle::VertexConstants OracleVertexConstants(
    const world_surface_context::Context& context)
{
    world_surface_oracle::VertexConstants constants{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            constants.clipRows[row][column] = context.vsTransposeViewProjection.rows[row][column];
            constants.worldRows[row][column] = context.vsTransposeWorld.rows[row][column];
        }
    }
    for (int component = 0; component < 4; ++component)
        constants.fog[component] = context.vsFog[component];
    return constants;
}

world_surface_oracle::PixelConstants OraclePixelConstants(const world_surface_context::Context& context)
{
    world_surface_oracle::PixelConstants constants{};
    for (int component = 0; component < 4; ++component)
    {
        constants.fogColor[component] = context.psFogColor[component];
        constants.material[component] = context.psMaterialColor[component];
    }
    return constants;
}

// ---------------------------------------------------------------------------
// Plane grouping for a multi-triangle surface.
//
// Coplanar triangles are grouped by their plane, not by material index or by index order: the
// normal comes from the triangle's own corners and the offset from dot(normal, p0). Neither
// number is matched by float equality -- two triangles of one map plane arrive with independently
// rounded positions, so the match is a tolerance on both -- and the number of groups is bounded,
// because "one group per triangle" would turn the per-group quota into the aggregate sample
// count and stop saying anything about the planes.
// ---------------------------------------------------------------------------
struct PlaneGroup
{
    Vec3 normal{};
    float offset = 0.0f;
    std::uint32_t group = 0;
};

constexpr std::uint32_t kMaxPlaneGroups = 32;
// cos of about 2.6 degrees between two normals of one plane.
constexpr float kPlaneNormalCosTolerance = 1.0f - 1e-3f;
// World units. The surfaces this replay is used on put their planes hundreds of units apart,
// while float rounding on positions around 1e3 is far below one tenth of a unit.
constexpr float kPlaneOffsetTolerance = 0.125f;

// The plane of one triangle in a canonical form, so both windings of one plane produce one
// normal and one offset: the grouping asks which plane a triangle lies in, not which way it
// faces, and facing is applied to the triangles themselves by the cull mode. False means the
// triangle has zero area and therefore no plane.
bool TrianglePlane(const world_surface_bundle::Bundle& bundle, const std::uint32_t index[3],
    Vec3* normal, float* offset)
{
    Vec3 corners[3];
    for (int corner = 0; corner < 3; ++corner)
    {
        const world_surface_bundle::WorldVertex vertex
            = world_surface_bundle::ReadVertex(bundle, index[corner]);
        for (int component = 0; component < 3; ++component)
            corners[corner].value[component] = vertex.position[component];
    }

    if (!Normalize(Cross(Subtract(corners[1], corners[0]), Subtract(corners[2], corners[0])), normal))
    {
        *offset = 0.0f;
        return false;
    }

    // The first component that is not effectively zero decides the sign.
    int sign = 0;
    for (int axis = 0; axis < 3 && sign == 0; ++axis)
        if (std::fabs(normal->value[axis]) > 1e-6f)
            sign = normal->value[axis] < 0.0f ? -1 : 1;
    if (sign < 0)
    {
        for (int axis = 0; axis < 3; ++axis)
            normal->value[axis] = -normal->value[axis];
    }

    // Offset from the canonical normal, so the sign flip above carries into it.
    *offset = Dot(*normal, corners[0]);
    return true;
}

// The group of the matching plane, or a new group. `overflow` is set when the surface needs more
// groups than the bound allows; the caller refuses rather than merging planes silently.
std::uint32_t FindOrAddPlaneGroup(const Vec3& normal, float offset,
    std::vector<PlaneGroup>* groups, bool* overflow)
{
    for (const PlaneGroup& group : *groups)
    {
        if (Dot(group.normal, normal) < kPlaneNormalCosTolerance)
            continue;
        if (std::fabs(group.offset - offset) > kPlaneOffsetTolerance)
            continue;
        return group.group;
    }
    if (groups->size() >= kMaxPlaneGroups)
    {
        *overflow = true;
        return 0;
    }
    PlaneGroup group;
    group.normal = normal;
    group.offset = offset;
    group.group = static_cast<std::uint32_t>(groups->size());
    groups->push_back(group);
    return group.group;
}

// The winding the device keeps, from the decoded state block: D3DCULL_CCW removes the
// counter-clockwise triangles, so the clockwise ones -- D3D's front face -- are what survives,
// and D3DCULL_CW is the other way round. DecodeCull only ever produces these three values, so
// false here means an unexpected mode reached this point rather than a mapping to a neighbour.
bool CullModeFromState(D3DCULL cull, world_surface_oracle::CullMode* mode)
{
    if (cull == D3DCULL_CCW)
    {
        *mode = world_surface_oracle::CullMode::ClockwiseOnly;
        return true;
    }
    if (cull == D3DCULL_CW)
    {
        *mode = world_surface_oracle::CullMode::CounterClockwiseOnly;
        return true;
    }
    if (cull == D3DCULL_NONE)
    {
        *mode = world_surface_oracle::CullMode::None;
        return true;
    }
    return false;
}

// The whole declaration: program identity, fixture gate, per-triangle vertex evaluation, then
// the stratified sample set with its predicted colours.
//
// Every triangle of the bundle goes through the vertex stage. `available == false` means the
// oracle refused and `reason` says why; the caller reports that as a failed check, never as a
// skip. The single-triangle case is the same loop over one element, so there is no second code
// path that could drift from it.
bool BuildOracleDeclaration(const world_surface_bundle::Bundle& bundle, const DecodedState& state,
    const world_surface_context::Context& context, const kisak::window::IwiMipChain* mips,
    OracleDeclaration* declaration)
{
    *declaration = OracleDeclaration{};

    declaration->programs = world_surface_oracle::CheckAuditedPrograms(
        bundle.vertex_shader_bytes, bundle.pixel_shader_bytes);
    if (!declaration->programs.ok)
    {
        declaration->reason = "shader pair is not the audited program: " + declaration->programs.reason;
        return true;
    }

    if (bundle.tri_count == 0 || bundle.index_count != bundle.tri_count * 3u)
    {
        declaration->reason = "geometry does not declare whole triangles";
        return true;
    }

    world_surface_oracle::CullMode cullMode = world_surface_oracle::CullMode::None;
    if (!CullModeFromState(state.cull, &cullMode))
    {
        declaration->reason = "the decoded cull mode has no oracle equivalent";
        return true;
    }

    const world_surface_oracle::VertexConstants vertexConstants = OracleVertexConstants(context);
    declaration->pixelConstants = OraclePixelConstants(context);

    std::vector<PlaneGroup> planeGroups;
    declaration->triangles.reserve(bundle.tri_count);
    declaration->outputs.resize(static_cast<std::size_t>(bundle.tri_count) * 3u);

    for (std::uint32_t triangle = 0; triangle < bundle.tri_count; ++triangle)
    {
        world_surface_oracle::VertexInput vertices[3];
        std::uint32_t indices[3] = {0, 0, 0};
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::uint16_t index = world_surface_bundle::ReadIndex(
                bundle, triangle * 3u + static_cast<std::uint32_t>(corner));
            if (index >= bundle.vertex_count)
            {
                declaration->reason = "triangle " + std::to_string(triangle) + " index "
                    + std::to_string(index) + " is outside the slice";
                return true;
            }
            indices[corner] = index;
            const world_surface_bundle::WorldVertex vertex
                = world_surface_bundle::ReadVertex(bundle, index);
            for (int component = 0; component < 4; ++component)
            {
                vertices[corner].position[component] = vertex.position[component];
                vertices[corner].texcoord[component] = vertex.texcoord[component];
            }
            vertices[corner].color = vertex.color;
        }

        // The audited fixture is a property of every triangle, not of the first one: the
        // reduction this oracle performs holds for all-white vertex colours, material
        // (1,1,1,0) and a disabled fog vector. The first triangle's facts are the ones printed;
        // a later triangle that leaves the fixture is named and refuses the declaration.
        const world_surface_oracle::BaselineFacts facts = world_surface_oracle::CheckBaseline(
            vertices, declaration->pixelConstants, vertexConstants);
        if (triangle == 0)
            declaration->baseline = facts;
        if (!facts.Ok())
        {
            declaration->reason = "triangle " + std::to_string(triangle)
                + " is outside the audited fixture: " + facts.Describe();
            return true;
        }

        world_surface_oracle::TriangleInput input;
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::size_t slot = static_cast<std::size_t>(triangle) * 3u
                + static_cast<std::size_t>(corner);
            if (!world_surface_oracle::EvalVertex(vertices[corner], vertexConstants,
                    &declaration->outputs[slot]))
            {
                declaration->reason = "the vertex stage refused triangle "
                    + std::to_string(triangle) + " corner " + std::to_string(corner)
                    + " (non-finite, or a zero eye-relative distance)";
                return true;
            }
            input.output[corner] = declaration->outputs[slot];
            input.index[corner] = indices[corner];
        }

        Vec3 normal{};
        float offset = 0.0f;
        if (!TrianglePlane(bundle, indices, &normal, &offset))
        {
            // A zero-area triangle has no plane. It gets a group of its own so that it can never
            // fill another plane's quota: the oracle counts and refuses it by name, and it covers
            // no pixels, so the quota for that group is never demanded.
            input.group = kMaxPlaneGroups + triangle;
        }
        else
        {
            bool overflow = false;
            input.group = FindOrAddPlaneGroup(normal, offset, &planeGroups, &overflow);
            if (overflow)
            {
                declaration->reason = "the surface's triangles do not fall into at most "
                    + std::to_string(kMaxPlaneGroups) + " coplanar planes (triangle "
                    + std::to_string(triangle) + " needed a new group); the grouping is bounded, "
                    "so the declaration is refused rather than sampled without per-plane quotas";
                return true;
            }
        }

        declaration->triangles.push_back(input);
    }

    if (mips == nullptr || mips->empty())
    {
        declaration->reason = "no prepared texture: the oracle samples mip0 and there is none";
        return true;
    }

    const kisak::window::PixelBufferView base = mips->front().View();
    declaration->mip0.rgba = base.bytes.data();
    declaration->mip0.width = static_cast<std::uint32_t>(base.width);
    declaration->mip0.height = static_cast<std::uint32_t>(base.height);

    world_surface_oracle::OracleConfig config;
    config.viewportWidth = kRenderWidth;
    config.viewportHeight = kRenderHeight;
    // A sample whose nearest triangle is not separated from the next by this much NDC z is
    // refused. The value is the oracle's own tie threshold, declared here rather than left at
    // zero: a pixel inside that bound is one the depth test cannot arbitrate, so a prediction
    // taken from it would be a coin flip dressed as a sample. It is an error bound on z/w, not a
    // tolerance chosen to make a frame pass.
    config.minDepthGap = world_surface_oracle::kDepthTieEpsilon;
    // The winding the device keeps, so a triangle the device culls is not predicted as covering
    // pixels. This is the recorded state's cull mode, not a choice made here.
    config.cull = cullMode;
    if (bundle.tri_count > 1)
    {
        // Every plane has to be represented by samples of its own: a four-plane surface predicted
        // from samples that all landed on one plane is not a prediction of the surface. Four is
        // the stated per-group minimum.
        //
        // The budget is raised with it, and this is the reason: maxSamples bounds the total, and
        // the walk spends it cell by cell, group by group. Four planes x four samples is sixteen,
        // so a sixteen-sample budget leaves no slack at all -- a plane whose candidates start one
        // cell later than its neighbours' finds the budget already spent, which is what produced
        // "covers 384 pixels but supplied only 2". Eight per plane keeps one large plane from
        // taking the budget while giving every plane room, and the per-group cap makes the total
        // bound (4 x 8) exact rather than incidental.
        config.minSamplesPerGroup = 4;
        config.maxSamplesPerGroup = 8;
        config.maxSamples = 32;
    }

    declaration->selection = world_surface_oracle::SelectSamplesBounded(declaration->triangles,
        declaration->mip0, declaration->pixelConstants, config);

    if (!declaration->selection.Ok())
    {
        declaration->reason = std::string("sample selection refused: ")
            + world_surface_oracle::SelectStatusName(declaration->selection.status) + ": "
            + declaration->selection.message;
        return true;
    }

    declaration->available = true;
    return true;
}

std::string DescribeSample(const world_surface_oracle::Sample& sample)
{
    return format("pixel=(%3u,%3u) cell=(%u,%u) weights=(%.4f,%.4f,%.4f) uv=(%.6f,%.6f) "
                  "texel=(%.3f,%.3f) texel_index=(%u,%u) expected=(%u,%u,%u,%u) "
                  "edge=%.2fpx boundary=%.3ftexel",
        sample.pixelX, sample.pixelY, sample.cellRow, sample.cellColumn, sample.weights[0],
        sample.weights[1], sample.weights[2], sample.uv[0], sample.uv[1],
        sample.texelCoordinate[0], sample.texelCoordinate[1], sample.texelX, sample.texelY,
        sample.expected[0], sample.expected[1], sample.expected[2], sample.expected[3],
        sample.edgeMarginPixels, sample.texelBoundaryMargin);
}

// Prints the declaration. Called before the device is created, and followed by a flush, so
// the numbers are on stdout ahead of the frame that is supposed to agree with them.
void PrintOracleDeclaration(Report& report, const char* label, const OracleDeclaration& declaration)
{
    report.Line(format("[oracle] %s: program gate vs=0x%016llx ps=0x%016llx words=%u/%u "
                       "instructions=%u/%u",
        label, static_cast<unsigned long long>(declaration.programs.vertexFnv),
        static_cast<unsigned long long>(declaration.programs.pixelFnv),
        declaration.programs.vertexWords, declaration.programs.pixelWords,
        declaration.programs.vertexInstructions, declaration.programs.pixelInstructions));

    if (!declaration.available)
    {
        report.Line(format("[oracle] %s: unavailable -- %s", label, declaration.reason.c_str()));
        return;
    }

    report.Line(format("[oracle] %s: fixture %s", label, declaration.baseline.Describe().c_str()));
    report.Line(format("[oracle] %s: mip0 %ux%u, grid %ux%u over the geometry bounding box "
                       "(%d,%d)-(%d,%d), samples=%zu, occupied_cells=%u, eligible_pixels=%u of %u, "
                       "margins >= %.1fpx interior and >= %.3f texel, tolerance %d level/channel",
        label, declaration.mip0.width, declaration.mip0.height, declaration.selection.cellCount,
        declaration.selection.cellCount, declaration.selection.boundingBox[0],
        declaration.selection.boundingBox[1], declaration.selection.boundingBox[2],
        declaration.selection.boundingBox[3], declaration.selection.samples.size(),
        declaration.selection.occupiedCells, declaration.selection.eligiblePixels,
        declaration.selection.consideredPixels, 2.0f, 0.25f, world_surface_oracle::kMaxChannelDelta));
    // The triangle census, so what the device did with each triangle is on the record: culled by
    // the recorded winding, wholly behind the eye (dropped, as the device drops it), degenerate,
    // non-finite, or straddling the eye plane -- which would need clipping this oracle does not
    // model and is refused rather than predicted.
    report.Line(format("[oracle] %s: triangles %u of %u kept, %u culled, %u wholly behind the eye, "
                       "%u degenerate, %u non-finite, %u straddling the eye plane",
        label, declaration.selection.trianglesKept, declaration.selection.triangleCount,
        declaration.selection.trianglesCulled, declaration.selection.trianglesBehindEye,
        declaration.selection.trianglesDegenerate, declaration.selection.trianglesNonFinite,
        declaration.selection.trianglesStraddlingEye));

    for (std::size_t index = 0; index < declaration.selection.samples.size(); ++index)
        report.Line(format("[oracle] %s: #%zu %s", label, index,
            DescribeSample(declaration.selection.samples[index]).c_str()));
}

// Compares one readback against a declaration and prints every sample, numerically.
bool CompareOracle(Report& report, const char* label, const OracleDeclaration& declaration,
    const Image& image, world_surface_oracle::Comparison* comparison)
{
    *comparison = world_surface_oracle::CompareSamples(declaration.selection.samples,
        image.rgba.data(), image.width, image.height, world_surface_oracle::kMaxChannelDelta);

    for (std::size_t index = 0; index < comparison->results.size(); ++index)
    {
        const world_surface_oracle::SampleResult& result = comparison->results[index];
        report.Line(format("[oracle-check] %s: #%zu pixel=(%3u,%3u) expected=(%u,%u,%u,%u) "
                           "actual=(%u,%u,%u,%u) delta=(%+d,%+d,%+d,%+d) %s",
            label, index, result.pixelX, result.pixelY, result.expected[0], result.expected[1],
            result.expected[2], result.expected[3], result.actual[0], result.actual[1],
            result.actual[2], result.actual[3], result.delta[0], result.delta[1], result.delta[2],
            result.delta[3], result.matched ? "match" : "MISMATCH"));
    }
    return comparison->Ok();
}

// Negative controls, split into the two claims they can actually support.
//
//   * Sensitivity: the declared comparison can tell the baseline prediction apart from a
//     prediction built from a deliberately wrong uv, or from swapped texel channels.
//     Scored at the tolerance the comparison uses -- a one-level change is ACCEPTED by
//     that rule, so counting "any nonzero difference" would claim a discrimination the
//     comparison does not make.
//   * Readback rejection: those altered expectations, scored against the baseline readback
//     at the very pixel coordinates the baseline declared, are rejected. That is the test a
//     wrong transcription would have to survive, and it goes through CompareSamples rather
//     than through a second comparison written for the occasion.
//
// Neither is a wrong-shader experiment: no altered program is uploaded and no image of a
// wrong shader exists in this run. The check names say so, and the report does not upgrade
// CPU rejection into a statement about a GPU shader.
struct ToleranceSensitivity
{
    std::uint32_t beyondTolerance = 0; // samples the declared comparison would reject
    std::uint32_t total = 0;
    int worstDelta = 0;
};

// Counts altered expectations that differ from the declared one by MORE than
// kMaxChannelDelta on some channel -- exactly the rule CompareSamples applies.
ToleranceSensitivity CountBeyondTolerance(const std::vector<world_surface_oracle::Sample>& declared,
    const std::vector<world_surface_oracle::Sample>& altered)
{
    ToleranceSensitivity sensitivity;
    sensitivity.total = static_cast<std::uint32_t>(std::min(declared.size(), altered.size()));
    for (std::size_t index = 0; index < sensitivity.total; ++index)
    {
        int worst = 0;
        for (int channel = 0; channel < 4; ++channel)
        {
            const int delta = std::abs(static_cast<int>(altered[index].expected[channel])
                - static_cast<int>(declared[index].expected[channel]));
            worst = std::max(worst, delta);
        }
        sensitivity.worstDelta = std::max(sensitivity.worstDelta, worst);
        if (worst > world_surface_oracle::kMaxChannelDelta)
            ++sensitivity.beyondTolerance;
    }
    return sensitivity;
}

struct NegativeRejection
{
    std::uint32_t rejected = 0;
    std::uint32_t total = 0;
    int worstDelta = 0;
};

// Scores altered expectations against a readback through the declared comparison. The
// sample coordinates are the declaration's own, so no sample is reselected here.
NegativeRejection RejectAgainstReadback(const std::vector<world_surface_oracle::Sample>& altered,
    const Image& image)
{
    const world_surface_oracle::Comparison comparison = world_surface_oracle::CompareSamples(
        altered, image.rgba.data(), image.width, image.height, world_surface_oracle::kMaxChannelDelta);
    NegativeRejection rejection;
    rejection.rejected = comparison.mismatched;
    rejection.total = static_cast<std::uint32_t>(comparison.results.size());
    rejection.worstDelta = comparison.worstDelta;
    return rejection;
}

// The three vertex outputs one declared sample was interpolated from. A sample carries the
// triangle it was predicted from, so an altered expectation recomputes exactly the varyings the
// baseline used instead of the first triangle of the list. Null means the sample named a
// triangle outside the declaration, which the altered-expectation builders treat as a failure.
const world_surface_oracle::VertexOutput* SampleTriangleOutputs(
    const OracleDeclaration& declaration, const world_surface_oracle::Sample& sample)
{
    const std::size_t base = static_cast<std::size_t>(sample.triangle) * 3u;
    if (base + 3u > declaration.outputs.size())
        return nullptr;
    return &declaration.outputs[base];
}

// A copy of the declared samples with the interpolated u shifted by a stated number of
// texels at mip0, and the expectation recomputed through the same pixel program. Pixel
// coordinates are copied unchanged, so the result is scored against a readback at exactly
// the positions the baseline declared. A flat region would not move, which is why the
// channel swap below exists as the fallback.
bool ShiftedUvExpectations(const OracleDeclaration& declaration, float shiftTexels,
    std::vector<world_surface_oracle::Sample>* altered)
{
    altered->assign(declaration.selection.samples.begin(), declaration.selection.samples.end());
    const float shift = shiftTexels / static_cast<float>(declaration.mip0.width);
    for (world_surface_oracle::Sample& sample : *altered)
    {
        float color[4];
        float uv[2];
        float fog = 0.0f;
        const world_surface_oracle::VertexOutput* outputs = SampleTriangleOutputs(declaration, sample);
        if (!outputs)
            return false;
        world_surface_oracle::InterpolateVaryings(sample.weights, outputs, color, uv, &fog);
        const float shifted[2] = {uv[0] + shift, uv[1]};
        float out[4];
        if (!world_surface_oracle::EvalPixelAt(shifted, fog, color, declaration.mip0,
                declaration.pixelConstants, out))
            return false;
        for (int channel = 0; channel < 4; ++channel)
            sample.expected[channel] = world_surface_oracle::QuantizeUnorm8(out[channel]);
    }
    return true;
}

// A copy of the declared samples with the red and blue channels of the sampled texel
// swapped and the expectation recomputed. This changes the prediction even where the
// texture is flat, unless the chosen texel happens to have r == b -- which is exactly the
// case the synthetic test in world_surface_oracle_test.cpp pins, so that a one-level
// swap cannot be mistaken for a discriminating control.
bool ChannelSwappedExpectations(const OracleDeclaration& declaration,
    std::vector<world_surface_oracle::Sample>* altered)
{
    altered->assign(declaration.selection.samples.begin(), declaration.selection.samples.end());
    for (world_surface_oracle::Sample& sample : *altered)
    {
        float color[4];
        float uv[2];
        float fog = 0.0f;
        const world_surface_oracle::VertexOutput* outputs = SampleTriangleOutputs(declaration, sample);
        if (!outputs)
            return false;
        world_surface_oracle::InterpolateVaryings(sample.weights, outputs, color, uv, &fog);
        float texel[4];
        world_surface_oracle::Sample2DPointWrapMip0(declaration.mip0, uv[0], uv[1], texel);
        const float swapped[4] = {texel[2], texel[1], texel[0], texel[3]};

        // The pixel program with the swapped texel, in the token order EvalPixelAt uses.
        const float* material = declaration.pixelConstants.material;
        const float* fogColor = declaration.pixelConstants.fogColor;
        float intermediate[3];
        for (int channel = 0; channel < 3; ++channel)
            intermediate[channel] = color[channel] * (-swapped[channel]) + material[channel];
        float register0[4];
        for (int channel = 0; channel < 4; ++channel)
            register0[channel] = swapped[channel] * color[channel];
        for (int channel = 0; channel < 3; ++channel)
            register0[channel] = material[3] * intermediate[channel] + register0[channel];
        for (int channel = 0; channel < 3; ++channel)
            register0[channel] = register0[channel] - fogColor[channel];
        float out[4];
        out[3] = register0[3];
        for (int channel = 0; channel < 3; ++channel)
            out[channel] = fog * register0[channel] + fogColor[channel];

        for (int channel = 0; channel < 4; ++channel)
            sample.expected[channel] = world_surface_oracle::QuantizeUnorm8(out[channel]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The moved-camera control.
//
// The control asks whether the image follows a change in the uploaded view-projection, so it
// needs a moved frame the oracle can actually predict. A moved frame whose prediction was
// refused is evidence of nothing, and letting it pass by being skipped would make the control
// disappear exactly when it is hardest to satisfy.
//
// The perturbations are therefore fixed here, in order, and each is a stated fraction of the
// eye-to-target distance along the camera's own axes. The list is bounded, the declarations are
// all built before the moved frame is drawn, and the first entry that declares is the one that
// is drawn; if none declares, the control fails.
// ---------------------------------------------------------------------------
struct CameraPerturbation
{
    float forward; // along the camera's forward axis, as a fraction of eye-to-target distance
    float left;    // along the camera's left axis, same unit
    const char* name;
};

constexpr CameraPerturbation kMovedCameraCandidates[] = {
    {0.25f, 0.0f, "forward 0.25"},
    {0.15f, 0.0f, "forward 0.15"},
    {-0.20f, 0.0f, "back 0.20"},
    {0.25f, 0.15f, "forward 0.25 + left 0.15"},
};

// The same Options with the eye moved by one candidate. An explicit camera is moved along the
// reference camera's own basis; a derived camera is moved by scaling --camera-distance, which
// puts the eye on the same normal axis the baseline used. A derived camera has no way to express
// the sideways component, so only its forward component is applied -- and the report names the
// candidate that was used, so the frame that was drawn is a stated one either way.
Options PerturbCamera(const Options& options, const world_surface_context::CameraInputs& reference,
    const CameraPerturbation& perturbation)
{
    Options moved = options;
    if (!options.cameraEyeSupplied)
    {
        moved.cameraDistanceScale = options.cameraDistanceScale * (1.0f + perturbation.forward);
        return moved;
    }

    const float distance = Length(Subtract(options.cameraTarget, options.cameraEye));
    for (int axis = 0; axis < 3; ++axis)
    {
        moved.cameraEye.value[axis] = options.cameraEye.value[axis]
            + reference.forward[axis] * distance * perturbation.forward
            + reference.left[axis] * distance * perturbation.left;
    }
    return moved;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    bool helpRequested = false;
    std::string error;
    if (!parseArgs(argc, argv, options, &helpRequested, &error))
    {
        std::fprintf(stderr, "%s\n\n", error.c_str());
        printUsage(stderr);
        return ExitUsage;
    }
    if (helpRequested)
    {
        printUsage(stdout);
        return ExitUsage;
    }

    if (options.selfTest)
        return RunSelfTest();

    Report report;
    report.Line("=== world surface replay (DXVK Native D3D9, standalone) ===");
    report.Line("[claim]  this run replays recorded numbers through a real D3D9 device; it is not");
    report.Line("[claim]  a frame capture, not engine parity, and not an image-parity result");

    // --- bundle ------------------------------------------------------------
    world_surface_bundle::Bundle bundle;
    if (!world_surface_bundle::Load(options.bundlePath, &bundle, &error))
    {
        std::fprintf(stderr, "[fail] bundle: %s\n", error.c_str());
        return ExitBundleInvalid;
    }

    // This replay implements one measured pass, not a general shader runner.
    // This non-cryptographic allowlist catches accidental pair substitution;
    // the preparation verifier must separately enforce the recorded SHA-256s.
    if (bundle.vertex_shader_hash_actual != UINT64_C(0x5068db98e3f4c40e) ||
        bundle.pixel_shader_hash_actual != UINT64_C(0x1e7bdf7881fe189f))
    {
        std::fprintf(stderr, "[fail] shader pair is outside this bounded replay's allowlist\n");
        return ExitBundleInvalid;
    }

    report.Line(format("[bundle] schema=%u surface=%u map=%s material=%s",
        bundle.schema_version, bundle.surface_index, bundle.map_name.c_str(), bundle.material_name.c_str()));
    report.Line(format("[bundle] technique=%s type=%s declaration=%s",
        bundle.technique_set_name.c_str(), bundle.technique_type_name.c_str(),
        bundle.declaration_name.c_str()));
    report.Line(format("[bundle] geometry: first_vertex=%u vertices=%u indices=%u triangles=%u stride=%u space=%s",
        bundle.first_vertex, bundle.vertex_count, bundle.index_count, bundle.tri_count,
        bundle.vertex_stride, bundle.index_space.c_str()));
    report.Line(format("[bundle] shaders: vs=%s (%u words) ps=%s (%u words)",
        bundle.vertex_shader.name.c_str(), bundle.vertex_shader.word_count,
        bundle.pixel_shader.name.c_str(), bundle.pixel_shader.word_count));
    report.Line(format("[bundle] payload hashes verified: vs=0x%016llx ps=0x%016llx verts=0x%016llx idx=0x%016llx",
        static_cast<unsigned long long>(bundle.vertex_shader_hash_actual),
        static_cast<unsigned long long>(bundle.pixel_shader_hash_actual),
        static_cast<unsigned long long>(bundle.vertex_hash_actual),
        static_cast<unsigned long long>(bundle.index_hash_actual)));

    // --- state block -------------------------------------------------------
    const DecodedState state = DecodeStateBlock(bundle.state_bits0, bundle.state_bits1);
    report.Line(format("[state]  load_bits = {%s, %s}", StateBitsDescription(bundle.state_bits0).c_str(),
        StateBitsDescription(bundle.state_bits1).c_str()));
    if (!state.supported)
    {
        report.Line(std::string("[state]  UNSUPPORTED: ") + state.unsupportedReason);
        report.Line("[state]  the replay will not substitute a default for an undecoded state");
        report.Flush();
        if (!options.reportPath.empty())
            report.WriteTo(options.reportPath);
        return ExitStateUnsupported;
    }
    report.Line(format("[state]  cull=%d depth_test=%s depth_func=%d depth_write=%s",
        static_cast<int>(state.cull), state.depthTestEnabled ? "on" : "off",
        static_cast<int>(state.depthFunction), state.depthWriteEnabled ? "on" : "off"));
    report.Line(format("[state]  blend=%s alpha_test=%s color_write=0x%X fill=%s stencil=%s",
        state.blendEnabled ? "on" : "off", state.alphaTestEnabled ? "on" : "off",
        static_cast<unsigned>(state.colorWriteMask),
        state.fillMode == D3DFILL_WIREFRAME ? "wireframe" : "solid",
        state.stencilEnabled ? "on" : "off"));

    // --- sampler -----------------------------------------------------------
    const world_surface_bundle::Argument* textureArgument = nullptr;
    for (const world_surface_bundle::Argument& argument : bundle.arguments)
        if (argument.has_texture)
            textureArgument = &argument;

    SamplerStates sampler{};
    bool haveSampler = false;
    if (textureArgument)
    {
        const std::uint8_t rawSampler = static_cast<std::uint8_t>(textureArgument->sampler_state);
        std::uint32_t decoded = 0;
        if (!DecodeSamplerState(rawSampler, options.filterInputs, &decoded, &error))
        {
            std::fprintf(stderr, "[fail] sampler: %s\n", error.c_str());
            return ExitStateUnsupported;
        }
        sampler = SplitSamplerDword(decoded);
        haveSampler = true;
        report.Line(format("[sampler] raw=0x%02X image=%s slot=%u -> decoded=0x%08X",
            static_cast<unsigned>(rawSampler), textureArgument->image_name.c_str(),
            textureArgument->texture_index, decoded));
        report.Line(format("[sampler] min=%u mag=%u mip=%u max_aniso=%u address u=%u v=%u w=%u "
                           "(filter entry rebuilt from stated r_texFilter inputs)",
            static_cast<unsigned>(sampler.minFilter), static_cast<unsigned>(sampler.magFilter),
            static_cast<unsigned>(sampler.mipFilter), static_cast<unsigned>(sampler.maxAnisotropy),
            static_cast<unsigned>(sampler.addressU), static_cast<unsigned>(sampler.addressV),
            static_cast<unsigned>(sampler.addressW)));
        if (options.texFilterDisable)
            report.Line("[sampler] filter inputs: r_texFilterDisable=1, all values fixed by r_state.cpp");
        else
            report.Line("[sampler] filter inputs: supplied on the command line (cap-derived in the engine)");
    }
    else
    {
        report.Line("[sampler] the bundle's pass binds no material texture");
    }

    // --- texture -----------------------------------------------------------
    // Decoded here, in this process, from the IWI the preparation step extracted. The
    // decoder is the repository's own; there is no intermediate image file.
    kisak::window::IwiMipChain textureChain;
    bool haveTexture = false;
    if (!options.texturePath.empty())
    {
        try
        {
            textureChain = kisak::window::LoadIwiMipChainFile(options.texturePath);
        }
        catch (const std::exception& exception)
        {
            std::fprintf(stderr, "[fail] texture decode: %s\n", exception.what());
            return ExitFailure;
        }
        if (textureChain.empty())
        {
            std::fprintf(stderr, "[fail] texture decode returned no levels for %s\n",
                options.texturePath.c_str());
            return ExitFailure;
        }
        haveTexture = true;
        const kisak::window::PixelExtent base = textureChain.front().Extent();
        report.Line(format("[texture] %s levels=%u base=%dx%d (DecodeIwiMipChain, in-process)",
            options.texturePath.c_str(), static_cast<unsigned>(textureChain.size()),
            base.width, base.height));
        for (std::size_t level = 0; level < textureChain.size(); ++level)
        {
            const kisak::window::PixelExtent extent = textureChain[level].Extent();
            report.Line(format("[texture] level %u: %dx%d", static_cast<unsigned>(level),
                extent.width, extent.height));
        }
    }
    else
    {
        report.Line("[texture] none supplied: binding a 1x1 white texel");
        report.Line("[texture] the image below is NOT a textured result; no texture fidelity");
        report.Line("[texture] is claimed for this run");
    }
    // The render and control call sites all bind this chain; naming it once keeps the
    // texture argument identical across every frame.
    const kisak::window::IwiMipChain* mips = haveTexture ? &textureChain : nullptr;

    // --- camera and constants ---------------------------------------------
    CameraDerivation derivation;
    if (!DeriveCamera(bundle, options, &derivation, &error))
    {
        std::fprintf(stderr, "[fail] camera: %s\n", error.c_str());
        return ExitFailure;
    }

    // Which side of the winding the eye sits on is decided by the recorded cull mode, not by
    // what makes the image look good. D3D9 keeps clockwise front faces; the viewport's y flip
    // makes that a negative NDC signed area. So: measure the winding from the +normal side,
    // and if that side is the back face the eye moves to the other side. The decision uses
    // geometry only, is printed with its number, and --camera-side overrides it.
    if (options.cameraEyeSupplied)
    {
        report.Line("[camera] explicit: the eye and the target are the values on the command line,");
        report.Line("[camera] and nothing is derived from the referenced triangle. The winding probe");
        report.Line("[camera] is therefore not applied: the stated eye is kept even when the index");
        report.Line("[camera] order faces away from it, which is what --camera-eye states.");
    }
    else if (!options.cameraSideSupplied)
    {
        world_surface_context::Inputs probeInputs{};
        probeInputs.camera = derivation.camera;
        double signedArea = 0.0;
        std::string probeMessage;
        if (!CameraSpaceSignedArea(bundle, probeInputs.camera, derivation, &signedArea, &probeMessage))
        {
            std::fprintf(stderr, "[fail] camera winding probe: %s\n", probeMessage.c_str());
            return ExitFailure;
        }

        report.Line(format("[camera] winding: camera-space signed area from the +normal side = %.6f",
            signedArea));
        report.Line("[camera] D3D9's front face is clockwise on screen, which is a positive area in");
        report.Line("[camera] this y-up camera basis, so the sign above says which side is front.");

        if (signedArea <= 0.0)
        {
            options.cameraSideBack = true;
            if (!DeriveCamera(bundle, options, &derivation, &error))
            {
                std::fprintf(stderr, "[fail] camera: %s\n", error.c_str());
                return ExitFailure;
            }
            report.Line("[camera] side derived: the recorded index order winds counter-clockwise");
            report.Line("[camera] from its own cross-product normal, so under D3D's clockwise");
            report.Line("[camera] front-face rule the eye is placed on the opposite side");
        }
        else
        {
            report.Line("[camera] side derived: the recorded index order is front-facing from its");
            report.Line("[camera] cross-product normal side, so the eye stays there");
        }
        report.Line(format("[camera] cull=%d from the bundle; --camera-side overrides this choice",
            static_cast<int>(state.cull)));
    }
    else
    {
        report.Line("[camera] side given explicitly with --camera-side; the winding derivation is "
                    "not applied");
    }

    if (options.cameraEyeSupplied)
    {
        report.Line(format("[camera] eye=(%.4f, %.4f, %.4f) target=(%.4f, %.4f, %.4f) z_near=%.4f "
                           "tan_half_fov=%.4f",
            options.cameraEye.value[0], options.cameraEye.value[1], options.cameraEye.value[2],
            options.cameraTarget.value[0], options.cameraTarget.value[1],
            options.cameraTarget.value[2], derivation.camera.zNear, options.tanHalfFov));
        report.Line(format("[camera] synthetic: stated with --camera-eye/--camera-target, not a "
                           "captured refdef; cull=%d from the bundle",
            static_cast<int>(state.cull)));
    }
    else
    {
        report.Line(format("[camera] indices=%u,%u,%u centroid=(%.4f, %.4f, %.4f) longest_edge=%.4f",
            derivation.indexValues[0], derivation.indexValues[1], derivation.indexValues[2],
            derivation.centroid.value[0], derivation.centroid.value[1], derivation.centroid.value[2],
            derivation.longestEdge));
        report.Line(format("[camera] normal=(%.4f, %.4f, %.4f) side=%s eye=(%.4f, %.4f, %.4f)",
            derivation.normal.value[0], derivation.normal.value[1], derivation.normal.value[2],
            options.cameraSideBack ? "back" : "front",
            derivation.camera.origin[0], derivation.camera.origin[1], derivation.camera.origin[2]));
        report.Line("[camera] synthetic: derived from the referenced triangle, not a captured refdef");
    }

    world_surface_context::Inputs contextInputs{};
    contextInputs.camera = derivation.camera;
    // Explicit neutral: a test context has no captured material, and the bundle records
    // colorTint as the only material constant. Stated here so the report cannot be read as
    // "the engine's material colour produced this".
    contextInputs.materialColor.rgba[0] = options.materialColor[0];
    contextInputs.materialColor.rgba[1] = options.materialColor[1];
    contextInputs.materialColor.rgba[2] = options.materialColor[2];
    contextInputs.materialColor.rgba[3] = options.materialColor[3];
    // Fog explicitly disabled, with the previous fog colour declared rather than assumed:
    // with r_fog off the engine leaves code 42 alone and the pixel stage keeps reading
    // whatever was there, so that value has to be stated.
    contextInputs.fog.enabled = false;
    contextInputs.fog.density = 0.0f;
    contextInputs.fog.fogStart = 0.0f;
    contextInputs.fog.colorBgra[0] = 0;
    contextInputs.fog.colorBgra[1] = 0;
    contextInputs.fog.colorBgra[2] = 0;
    contextInputs.fog.colorBgra[3] = 0;
    contextInputs.fog.previousColor[0] = 0.0f;
    contextInputs.fog.previousColor[1] = 0.0f;
    contextInputs.fog.previousColor[2] = 0.0f;
    contextInputs.fog.previousColor[3] = 0.0f;
    report.Line(format("[constant] code 40 material colour = (%.3f, %.3f, %.3f, %.3f), stated on "
                       "the command line; alpha %.3f is the lerp factor the pixel shader uses to "
                       "replace it",
        options.materialColor[0], options.materialColor[1], options.materialColor[2],
        options.materialColor[3], options.materialColor[3]));
    report.Line("[constant] code 42 fog colour = (0, 0, 0, 0) declared previous value, r_fog off");
    report.Line("[constant] code 41 fog vector = (0, 1, 0, 0) from the disabled branch");

    world_surface_context::Context context{};
    world_surface_context::Error contextError = world_surface_context::Error::None;
    if (!world_surface_context::Build(contextInputs, &context, &contextError))
    {
        std::fprintf(stderr, "[fail] camera context rejected: %s\n",
            world_surface_context::ErrorName(contextError));
        if (contextError == world_surface_context::Error::AxisNotLeftHanded)
        {
            std::fprintf(stderr,
                "[fail] the camera basis is built as left = up x forward, the production\n"
                "[fail] convention. world_surface_context.h still checks left == forward x up,\n"
                "[fail] so this run stops here instead of flipping the basis to satisfy it.\n");
        }
        return ExitFailure;
    }
    report.Line(format("[constant] vp m00=%.6f fog=(%.1f, %.1f, %.1f, %.1f)",
        context.vsTransposeViewProjection.rows[0][0], context.vsFog[0], context.vsFog[1],
        context.vsFog[2], context.vsFog[3]));
    // The payload as uploaded, so the frame is analysable from the report alone. These are
    // the exact values the vertex shader reads: c0..c3 the transposed view-projection rows,
    // c4..c7 the transposed world rows, c21 the fog vector; PS c0 is the fog colour and
    // PS c1 the material colour.
    for (int row = 0; row < 4; ++row)
        report.Line(format("[constant] VS c%d = (%.9g, %.9g, %.9g, %.9g)", row,
            context.vsTransposeViewProjection.rows[row][0],
            context.vsTransposeViewProjection.rows[row][1],
            context.vsTransposeViewProjection.rows[row][2],
            context.vsTransposeViewProjection.rows[row][3]));
    for (int row = 0; row < 4; ++row)
        report.Line(format("[constant] VS c%d = (%.9g, %.9g, %.9g, %.9g)", 4 + row,
            context.vsTransposeWorld.rows[row][0], context.vsTransposeWorld.rows[row][1],
            context.vsTransposeWorld.rows[row][2], context.vsTransposeWorld.rows[row][3]));
    report.Line(format("[constant] VS c21 = (%.9g, %.9g, %.9g, %.9g)", context.vsFog[0],
        context.vsFog[1], context.vsFog[2], context.vsFog[3]));
    report.Line(format("[constant] PS c0 = (%.9g, %.9g, %.9g, %.9g)", context.psFogColor[0],
        context.psFogColor[1], context.psFogColor[2], context.psFogColor[3]));
    report.Line(format("[constant] PS c1 = (%.9g, %.9g, %.9g, %.9g)", context.psMaterialColor[0],
        context.psMaterialColor[1], context.psMaterialColor[2], context.psMaterialColor[3]));

    // --- CPU pixel oracle: declaration -------------------------------------
    // Everything below is computed from the bundle, the uploaded payloads and the decoded
    // mip0 texels. It runs before SDL is initialised, and its numbers are flushed to stdout
    // here, so the expectation provably precedes the frame it describes.
    OracleDeclaration oracleDeclaration;
    if (!BuildOracleDeclaration(bundle, state, context, mips, &oracleDeclaration))
    {
        std::fprintf(stderr, "[fail] oracle: the declaration could not be built\n");
        return ExitFailure;
    }
    PrintOracleDeclaration(report, "baseline", oracleDeclaration);
    report.Check("oracle-declared", oracleDeclaration.available,
        oracleDeclaration.available
            ? format("%zu samples over %u of %ux%u cells, mip0 %ux%u",
                oracleDeclaration.selection.samples.size(), oracleDeclaration.selection.occupiedCells,
                oracleDeclaration.selection.cellCount, oracleDeclaration.selection.cellCount,
                oracleDeclaration.mip0.width, oracleDeclaration.mip0.height)
            : std::string("declined: ") + oracleDeclaration.reason);

    // Negative controls, CPU only and before any frame exists: the declared comparison has
    // to be able to tell an altered expectation from the declared one, at the tolerance it
    // actually applies. A one-level change sits inside that tolerance, so the counts below
    // are "beyond tolerance", not "different". A flat texture region defeats the uv shift,
    // which is why the channel swap is measured as well.
    //
    // The altered lists are kept: after the frame is drawn they are scored against the
    // readback at these same pixel coordinates, which is a rejection test rather than a
    // sensitivity count.
    std::vector<world_surface_oracle::Sample> wrongUvExpectations;
    std::vector<world_surface_oracle::Sample> channelSwapExpectations;
    ToleranceSensitivity wrongUvSensitivity;
    ToleranceSensitivity channelSwapSensitivity;
    bool negativeBuilt = false;
    if (oracleDeclaration.available)
    {
        if (!ShiftedUvExpectations(oracleDeclaration, 17.0f, &wrongUvExpectations)
            || !ChannelSwappedExpectations(oracleDeclaration, &channelSwapExpectations))
        {
            std::fprintf(stderr, "[fail] negative control: the altered prediction could not be built\n");
            return ExitFailure;
        }
        wrongUvSensitivity
            = CountBeyondTolerance(oracleDeclaration.selection.samples, wrongUvExpectations);
        channelSwapSensitivity
            = CountBeyondTolerance(oracleDeclaration.selection.samples, channelSwapExpectations);
        negativeBuilt = true;

        report.Line(format("[oracle] sensitivity wrong-uv(+17 texels of %u): %u of %u expectations "
                           "differ by more than %d level/channel (worst %d)",
            oracleDeclaration.mip0.width, wrongUvSensitivity.beyondTolerance,
            wrongUvSensitivity.total, world_surface_oracle::kMaxChannelDelta,
            wrongUvSensitivity.worstDelta));
        report.Line(format("[oracle] sensitivity channel-swap(r<->b): %u of %u expectations differ "
                           "by more than %d level/channel (worst %d)",
            channelSwapSensitivity.beyondTolerance, channelSwapSensitivity.total,
            world_surface_oracle::kMaxChannelDelta, channelSwapSensitivity.worstDelta));
        report.Check("oracle-negative-cpu",
            wrongUvSensitivity.beyondTolerance > 0 || channelSwapSensitivity.beyondTolerance > 0,
            format("wrong-uv %u/%u, channel-swap %u/%u beyond the %d level/channel tolerance "
                   "(at least one must be nonzero, or the expectation is texture-independent at "
                   "the tolerance the comparison uses)",
                wrongUvSensitivity.beyondTolerance, wrongUvSensitivity.total,
                channelSwapSensitivity.beyondTolerance, channelSwapSensitivity.total,
                world_surface_oracle::kMaxChannelDelta));
    }
    report.Flush();

    // --- device ------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "[fail] SDL_Init(SDL_INIT_VIDEO): %s\n", SDL_GetError());
        return ExitDeviceUnavailable;
    }

    // DXVK reads this while it brings the WSI up; it must be set before any D3D9 object.
    if (SDL_getenv("DXVK_WSI_DRIVER") == nullptr)
        SDL_setenv_unsafe("DXVK_WSI_DRIVER", "SDL3", 0);

    SDL_Window* window = SDL_CreateWindow("world-surface-replay",
        static_cast<int>(kRenderWidth), static_cast<int>(kRenderHeight),
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window)
    {
        std::fprintf(stderr, "[fail] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return ExitDeviceUnavailable;
    }
    const HWND hwnd = dxvk::wsi::toHwnd(window);

    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS presentParams{};
    if (!CreateDevice(hwnd, &d3d, &device, &presentParams, &error))
    {
        std::fprintf(stderr, "[fail] %s\n", error.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return ExitDeviceUnavailable;
    }
    report.Line("[device] D3D9 device created at D3DDEVTYPE_HAL");

    // --- render ------------------------------------------------------------
    FrameInputs frame{};
    frame.context = &context;
    frame.mips = mips;
    frame.sampler = haveSampler ? &sampler : nullptr;

    Image rendered;
    UploadStats uploadStats;
    bool ok = true;
    if (!RunFrame(bundle, device, state, frame, &rendered, &uploadStats, &error))
    {
        std::fprintf(stderr, "[fail] %s\n", error.c_str());
        ok = false;
    }
    if (ok)
    {
        report.Line(format("[upload] vertex constant rows=%d pixel constant rows=%d textures bound=%d",
            uploadStats.vertexRows, uploadStats.pixelRows, uploadStats.texturesBound));
        // Nothing is listed as skipped because nothing can be: every argument either matched
        // the fixed-source contract or the run stopped before this point.
        report.Line(format("[upload] routing contract held for %zu constant argument(s); no constant "
                           "was skipped or defaulted",
            bundle.arguments.size()));
        for (const world_surface_bundle::MaterialConstant& constant : bundle.material_constants)
            report.Line(format("[upload] material constant '%s' (hash 0x%08X) literal "
                               "(%.3f, %.3f, %.3f, %.3f) not uploaded: its register base is "
                               "renderer state, not bundle data",
                constant.name.c_str(), constant.name_hash, constant.literal[0],
                constant.literal[1], constant.literal[2], constant.literal[3]));
    }

    if (!ok)
    {
        device->Release();
        d3d->Release();
        SDL_DestroyWindow(window);
        SDL_Quit();
        report.Flush();
        if (!options.reportPath.empty())
            report.WriteTo(options.reportPath);
        return ExitFailure;
    }

    // --- checks ------------------------------------------------------------
    const ImageStats stats = Measure(rendered);
    report.Check("coverage", stats.coveredPixels >= kMinCoveredPixels,
        format("%zu of %u pixels differ from the clear colour", stats.coveredPixels,
            kRenderWidth * kRenderHeight));
    report.Check("distinct-colours", stats.distinctColors > 0,
        format("%zu distinct colours", stats.distinctColors));
    // Objective pixel sentinel. Coverage alone cannot tell "the shader drew the surface"
    // from "the shader drew it black", and the clear colour is magenta so neither case can
    // hide behind the background. This refuses a pass over an image whose drawn pixels are
    // all dark, which is what a broken constant upload looks like.
    report.Check("sentinel-not-black", stats.coveredPixels > 0 && stats.brightestChannel > 8,
        format("brightest channel among covered pixels = %u", stats.brightestChannel));
    // The material samples a prepared texture, so a run without one cannot establish that
    // the recorded sampler and image were applied. Stated as a check, not a skip: a missing
    // texture must not be able to print PASS.
    report.Check("texture-supplied", haveTexture,
        haveTexture ? format("%s decoded in-process", options.texturePath.c_str())
                    : std::string("no --texture: the run bound a 1x1 white stand-in"));

    // --- pixel oracle: baseline comparison ---------------------------------
    // The readback against the prediction printed before the device was created. A sample
    // matches when every channel is within kMaxChannelDelta of the CPU value.
    {
        world_surface_oracle::Comparison oracleComparison;
        const bool oracleOk = oracleDeclaration.available
            && CompareOracle(report, "baseline", oracleDeclaration, rendered, &oracleComparison);
        report.Check("oracle", oracleOk,
            oracleDeclaration.available
                ? format("%u of %zu samples matched within %d level/channel, worst delta %d",
                    oracleComparison.matched, oracleComparison.results.size(),
                    world_surface_oracle::kMaxChannelDelta, oracleComparison.worstDelta)
                : std::string("declined: ") + oracleDeclaration.reason);
    }

    // --- pixel oracle: the altered expectations must be rejected by the readback --------
    // The same two altered sample lists the sensitivity check counted, scored against the
    // frame just drawn at the coordinates the baseline declared. This is the rejection a
    // wrong transcription would have to survive, and it runs through CompareSamples: the
    // comparison that accepted the baseline is the comparison that has to refuse these.
    //
    // The label states what is rejected -- a CPU expectation -- because no altered shader
    // was uploaded and no image of a wrong shader exists in this run.
    if (negativeBuilt)
    {
        const NegativeRejection wrongUv = RejectAgainstReadback(wrongUvExpectations, rendered);
        const NegativeRejection channelSwap = RejectAgainstReadback(channelSwapExpectations, rendered);
        report.Line(format("[oracle] rejection wrong-uv: %u of %u samples rejected, worst delta %d",
            wrongUv.rejected, wrongUv.total, wrongUv.worstDelta));
        report.Line(format("[oracle] rejection channel-swap: %u of %u samples rejected, worst delta %d",
            channelSwap.rejected, channelSwap.total, channelSwap.worstDelta));
        report.Check("oracle-negative-readback",
            wrongUv.rejected > 0 || channelSwap.rejected > 0,
            format("the readback rejects %u of %u wrong-uv and %u of %u channel-swapped CPU "
                   "expectations at the declared pixel coordinates (worst delta %d and %d); at "
                   "least one must be rejected, or the comparison would accept a prediction it "
                   "cannot tell from the wrong one",
                wrongUv.rejected, wrongUv.total, channelSwap.rejected, channelSwap.total,
                wrongUv.worstDelta, channelSwap.worstDelta));
    }

    // Spatial sampling: a triangle that covered the target with one flat value would pass
    // a coverage count and still be wrong, so sample three interior points.
    {
        const std::uint32_t quarterX = kRenderWidth / 4;
        const std::uint32_t midX = kRenderWidth / 2;
        const std::uint32_t threeQuarterX = (kRenderWidth * 3) / 4;
        const std::uint32_t midY = kRenderHeight / 2;
        report.Line(format("[sample] left=(%u,%u,%u) centre=(%u,%u,%u) right=(%u,%u,%u)",
            SampleChannel(rendered, quarterX, midY, 0), SampleChannel(rendered, quarterX, midY, 1),
            SampleChannel(rendered, quarterX, midY, 2),
            SampleChannel(rendered, midX, midY, 0), SampleChannel(rendered, midX, midY, 1),
            SampleChannel(rendered, midX, midY, 2),
            SampleChannel(rendered, threeQuarterX, midY, 0), SampleChannel(rendered, threeQuarterX, midY, 1),
            SampleChannel(rendered, threeQuarterX, midY, 2)));
    }

    // --- controls ----------------------------------------------------------
    // Every control answers the same question: is the image actually driven by the value
    // under test? They always run. A control set that can be switched off is a control set
    // that will be switched off before the run that matters, so there is no such flag.
    {
        // Constant control: move the eye, everything else identical. The perturbation comes from
        // the fixed list above and its declaration is built here, before the moved frame is drawn;
        // the first entry that declares is the one drawn and compared. A moved frame whose
        // prediction was refused would be evidence of nothing, so a list that declares nothing
        // fails the control rather than skipping it.
        Options moved;
        CameraDerivation movedDerivation;
        world_surface_context::Context movedContext{};
        OracleDeclaration movedOracle;
        std::string movedRejection;
        const char* movedCandidate = nullptr;
        bool movedChosen = false;
        for (const CameraPerturbation& candidate : kMovedCameraCandidates)
        {
            const Options trial = PerturbCamera(options, derivation.camera, candidate);
            CameraDerivation trialDerivation;
            if (!DeriveCamera(bundle, trial, &trialDerivation, &error))
            {
                movedRejection = std::string(candidate.name) + ": " + error;
                continue;
            }
            world_surface_context::Inputs trialInputs = contextInputs;
            trialInputs.camera = trialDerivation.camera;
            world_surface_context::Context trialContext{};
            world_surface_context::Error trialError = world_surface_context::Error::None;
            if (!world_surface_context::Build(trialInputs, &trialContext, &trialError))
            {
                movedRejection = std::string(candidate.name) + ": context rejected ("
                    + world_surface_context::ErrorName(trialError) + ")";
                continue;
            }
            OracleDeclaration trialOracle;
            if (!BuildOracleDeclaration(bundle, state, trialContext, mips, &trialOracle))
            {
                std::fprintf(stderr, "[fail] control oracle: declaration failed\n");
                ok = false;
                break;
            }
            if (!trialOracle.available)
            {
                movedRejection = std::string(candidate.name) + ": " + trialOracle.reason;
                continue;
            }
            moved = trial;
            movedDerivation = trialDerivation;
            movedContext = trialContext;
            movedOracle = trialOracle;
            movedCandidate = candidate.name;
            movedChosen = true;
            break;
        }

        report.Check("oracle-moved-camera-declared", movedChosen,
            movedChosen
                ? format("the moved camera ('%s') was declared on the CPU before its frame was "
                         "drawn, at eye (%.4f, %.4f, %.4f)",
                    movedCandidate, movedDerivation.camera.origin[0],
                    movedDerivation.camera.origin[1], movedDerivation.camera.origin[2])
                : std::string("no entry of the fixed perturbation list could be declared, so the "
                              "moved-camera comparison has no prediction to make; last rejection: ")
                    + movedRejection);
        if (!movedChosen)
            ok = false;
        else
        {
            // Declared before the frame is drawn, by the same a priori procedure as the baseline.
            // This is the only control that changes the vertex transform, so a prediction that
            // still matches shows the oracle tracks clip-space geometry rather than reproducing
            // the texels.
            PrintOracleDeclaration(report, "moved-camera", movedOracle);
            FrameInputs movedFrame = frame;
            movedFrame.context = &movedContext;
            Image movedImage;
            if (!RunFrame(bundle, device, state, movedFrame, &movedImage, &uploadStats, &error))
            {
                std::fprintf(stderr, "[fail] control render: %s\n", error.c_str());
                ok = false;
            }
            else
            {
                const std::size_t differing = CountDifferingPixels(rendered, movedImage);
                const ImageStats movedStats = Measure(movedImage);
                report.Check("control-constant",
                    differing > 0 && movedStats.coveredPixels > 0 && movedStats.brightestChannel > 8,
                    format("%zu pixels changed when the eye moved ('%s') and the uploaded constants "
                           "with it (covered=%zu brightest=%u)",
                        differing, movedCandidate, movedStats.coveredPixels,
                        movedStats.brightestChannel));

                world_surface_oracle::Comparison movedComparison;
                const bool movedOk = CompareOracle(
                    report, "moved-camera", movedOracle, movedImage, &movedComparison);
                report.Check("oracle-moved-camera", movedOk,
                    format("%u of %zu samples of the moved camera matched within %d level/channel, "
                           "worst delta %d",
                        movedComparison.matched, movedComparison.results.size(),
                        world_surface_oracle::kMaxChannelDelta, movedComparison.worstDelta));
            }
        }

        // Skip-draw control: identical bindings, state and constants; the only difference is
        // that DrawIndexedPrimitive is never called. The target must then hold the clear
        // colour exactly, which is what separates "the triangle covered these pixels" from
        // "something else wrote them".
        {
            FrameInputs noDraw = frame;
            noDraw.issueDraw = false;
            Image emptyImage;
            if (!RunFrame(bundle, device, state, noDraw, &emptyImage, &uploadStats, &error))
            {
                std::fprintf(stderr, "[fail] control skip-draw: %s\n", error.c_str());
                ok = false;
            }
            else
            {
                const ImageStats emptyStats = Measure(emptyImage);
                report.Check("control-skip-draw", emptyStats.coveredPixels == 0,
                    format("%zu pixels differ from the clear colour with the draw call "
                           "omitted (expected 0)",
                        emptyStats.coveredPixels));

                // ...and the oracle must reject that same frame. This is what makes the
                // baseline comparison non-vacuous: a comparison that also passed against a
                // cleared target would be measuring nothing about the draw.
                if (oracleDeclaration.available)
                {
                    world_surface_oracle::Comparison skipped;
                    CompareOracle(report, "skip-draw", oracleDeclaration, emptyImage, &skipped);
                    report.Check("oracle-skip-draw",
                        !skipped.results.empty() && skipped.mismatched == skipped.results.size(),
                        format("%u of %zu samples disagree with the prediction when the draw is "
                               "omitted (expected all)",
                            skipped.mismatched, skipped.results.size()));
                }
            }
        }

        // Offscreen control: same geometry and constants, but the eye looks away from the
        // triangle, so the whole draw is clipped. The target must stay at the clear colour.
        {
            world_surface_context::Inputs awayInputs = contextInputs;
            // Away is derived from the eye that was actually used, so the control also holds
            // when --camera-side moved the eye to the other side of the winding.
            const Vec3 awayForward = Subtract(
                Vec3{{derivation.camera.origin[0], derivation.camera.origin[1],
                    derivation.camera.origin[2]}},
                derivation.centroid);
            if (!BuildCameraBasis(awayForward, &awayInputs.camera, &error))
            {
                std::fprintf(stderr, "[fail] control offscreen basis: %s\n", error.c_str());
                ok = false;
            }
            else
            {
                world_surface_context::Context awayContext{};
                world_surface_context::Error awayError = world_surface_context::Error::None;
                if (!world_surface_context::Build(awayInputs, &awayContext, &awayError))
                {
                    std::fprintf(stderr, "[fail] control offscreen context: %s\n",
                        world_surface_context::ErrorName(awayError));
                    ok = false;
                }
                else
                {
                    FrameInputs awayFrame = frame;
                    awayFrame.context = &awayContext;
                    Image awayImage;
                    if (!RunFrame(bundle, device, state, awayFrame, &awayImage, &uploadStats, &error))
                    {
                        std::fprintf(stderr, "[fail] control offscreen render: %s\n", error.c_str());
                        ok = false;
                    }
                    else
                    {
                        const ImageStats awayStats = Measure(awayImage);
                        report.Check("control-offscreen", awayStats.coveredPixels == 0,
                            format("%zu pixels differ from the clear colour when the eye looks "
                                   "away (expected 0)",
                                awayStats.coveredPixels));
                    }
                }
            }
        }

        // Texture control: bind a deliberately different image. Every other input is the
        // one the baseline used, so any change in the readback is caused by the texels.
        // Note the baseline may itself be the 1x1 white stand-in: the control still proves
        // the sampler reads the bound texture, while the texture-supplied check above is
        // what refuses a pass without a prepared image.
        {
            kisak::window::IwiMipChain checkerMips(1);
            if (!checkerMips[0].Resize(2, 2))
            {
                std::fprintf(stderr, "[fail] cannot allocate the control texture\n");
                ok = false;
            }
            else
            {
                // No texel in the perturbation image is white or near-white, so whichever
                // texel the recorded UV lands on, the sampled colour differs from the prepared
                // image's. A control image that happened to contain a white texel could pass
                // while proving nothing.
                const std::uint8_t primaries[16] = {
                    255, 0, 0, 255, 0, 255, 0, 255,
                    0, 0, 255, 255, 16, 16, 16, 255};
                std::memcpy(checkerMips[0].Bytes().data(), primaries, sizeof(primaries));

                FrameInputs textureFrame = frame;
                textureFrame.mips = &checkerMips;
                Image textureImage;
                if (!RunFrame(bundle, device, state, textureFrame, &textureImage, &uploadStats, &error))
                {
                    std::fprintf(stderr, "[fail] control texture render: %s\n", error.c_str());
                    ok = false;
                }
                else
                {
                    const std::size_t differing = CountDifferingPixels(rendered, textureImage);
                    const ImageStats textureStats = Measure(textureImage);
                    report.Check("control-texture",
                        differing > 0 && textureStats.coveredPixels > 0 && textureStats.brightestChannel > 8,
                        format("%zu pixels changed when the bound texture changed "
                               "(covered=%zu brightest=%u)",
                            differing, textureStats.coveredPixels, textureStats.brightestChannel));
                }
            }
        }
    }

    // --- draw order and the depth test -------------------------------------
    // These run only for geometry with more than one triangle: a single triangle has no other
    // order to be drawn in, and the three frames would be identical by construction. The gate is
    // the bundle's triangle count, not a flag, so the comparison cannot be switched off for a
    // surface that has an order to get wrong.
    if (bundle.tri_count > 1)
    {
        // Split draw in the recorded order: one call per triangle instead of one call over the
        // slice. If this differs from the baseline, splitting the draw is a change of its own and
        // the reversal below could not be attributed to the order.
        FrameInputs splitOrder = frame;
        splitOrder.drawOrder = DrawOrder::PerTriangle;
        Image splitImage;
        if (!RunFrame(bundle, device, state, splitOrder, &splitImage, &uploadStats, &error))
        {
            std::fprintf(stderr, "[fail] control split draw: %s\n", error.c_str());
            ok = false;
        }
        else
        {
            const std::size_t differing = CountDifferingPixels(rendered, splitImage);
            report.Check("control-split-draw", differing == 0 && splitImage.rgba == rendered.rgba,
                format("%zu of %u pixels differ when the %u triangles are issued as %u single-"
                       "triangle calls in the recorded order (expected 0, so the draw order "
                       "control below is about the order and not about the call count)",
                    differing, kRenderWidth * kRenderHeight, bundle.tri_count, bundle.tri_count));
        }

        // Reversed draw order with the recorded depth test still on. The depth buffer arbitrates
        // the overlaps, so the image has to be the baseline again.
        FrameInputs reversed = frame;
        reversed.drawOrder = DrawOrder::Reverse;
        Image reversedImage;
        if (!RunFrame(bundle, device, state, reversed, &reversedImage, &uploadStats, &error))
        {
            std::fprintf(stderr, "[fail] control reversed order: %s\n", error.c_str());
            ok = false;
        }
        else
        {
            const std::size_t differing = CountDifferingPixels(rendered, reversedImage);
            report.Check("control-reverse-depth-on",
                differing == 0 && reversedImage.rgba == rendered.rgba,
                format("%zu pixels differ when the %u triangles are drawn last-first with the "
                       "recorded depth test (expected 0: depth decides the overlap, not order)",
                    differing, bundle.tri_count));

            // Draw order with the depth test off, forward against reversed. This is the pair the
            // oracle can declare a difference for: with the test gone the last triangle drawn wins
            // each pixel, so the forward frame shows the last-drawn triangle's colour and the
            // reversed frame shows the first-drawn one's. Before either frame was drawn, the
            // selection counted the pixels where both of those cover with margin AND predict
            // colours differing by more than the comparison's tolerance (orderContrastPixels).
            // That count is the preregistered contrast, and it is a lower bound on the difference
            // the two frames must show.
            //
            // Comparing the baseline against the reversed depth-disabled frame instead would not
            // be sound: where the nearest triangle happens to be the one drawn first, or the two
            // triangles paint the same colour, those two frames agree even though the pixels are
            // order-sensitive. That is why the control is this pair and not "baseline differs".
            FrameInputs forwardNoDepth = frame;
            forwardNoDepth.drawOrder = DrawOrder::PerTriangle;
            forwardNoDepth.overrideDepthTest = true;
            forwardNoDepth.depthTestEnabled = false;
            FrameInputs reversedNoDepth = reversed;
            reversedNoDepth.overrideDepthTest = true;
            reversedNoDepth.depthTestEnabled = false;
            Image forwardNoDepthImage;
            Image reversedNoDepthImage;
            if (!RunFrame(bundle, device, state, forwardNoDepth, &forwardNoDepthImage,
                    &uploadStats, &error)
                || !RunFrame(bundle, device, state, reversedNoDepth, &reversedNoDepthImage,
                    &uploadStats, &error))
            {
                std::fprintf(stderr, "[fail] control draw order without depth: %s\n", error.c_str());
                ok = false;
            }
            else
            {
                const std::size_t differing
                    = CountDifferingPixels(forwardNoDepthImage, reversedNoDepthImage);
                const world_surface_oracle::Selection& selection = oracleDeclaration.selection;
                report.Line(format("[control] overlapping=%u order-sensitive=%u contrast=%u "
                                   "(box (%d,%d)-(%d,%d)) depth-ties=%u; forward and reversed "
                                   "without the depth test differ on %zu pixels",
                    selection.multiCoveredPixels, selection.orderSensitivePixels,
                    selection.orderContrastPixels, selection.orderContrastBox[0],
                    selection.orderContrastBox[1], selection.orderContrastBox[2],
                    selection.orderContrastBox[3], selection.depthTiePixels, differing));

                if (!oracleDeclaration.available)
                {
                    report.Line("[control] reverse-depth-off not claimed: the oracle declined for "
                                "this run, so there is no a-priori count of order-sensitive pixels");
                }
                else if (selection.orderContrastPixels == 0)
                {
                    report.Line("[control] reverse-depth-off not claimed: no pixel is covered by two "
                                "triangles that both put the sample point inside their own edges and "
                                "predict different colours, so reversing the draw order has nothing "
                                "to show");
                }
                else if (selection.depthTiePixels > 0)
                {
                    report.Line(format("[control] reverse-depth-off not claimed: %u pixel(s) have "
                                       "their two nearest surfaces within the %.1e depth-tie "
                                       "threshold, where the depth test cannot arbitrate either",
                        selection.depthTiePixels, world_surface_oracle::kDepthTieEpsilon));
                }
                else
                {
                    report.Check("control-reverse-depth-off",
                        differing > 0 && differing >= selection.orderContrastPixels,
                        format("%zu pixels differ between the forward and reversed draws with the "
                               "depth test disabled, against %u pixels the oracle declared as "
                               "draw-order contrast before either frame was drawn (0 declared "
                               "depth ties); the difference is the depth test's, not the order's",
                            differing, selection.orderContrastPixels));
                }
            }
        }
    }

    // --- device reset ------------------------------------------------------
    // Not a resource-recreation check on the same device: the frame's default-pool objects
    // (vertex buffer, index buffer, render target, depth surface) are unbound and released,
    // IDirect3DDevice9::Reset is called, and only then are they rebuilt. A retained
    // reference makes Reset fail, which fails this check rather than passing it.
    {
        Image afterReset;
        if (!ResetAndRebuild(bundle, device, presentParams, state, frame, &afterReset, &uploadStats, &error))
        {
            std::fprintf(stderr, "[fail] device reset: %s\n", error.c_str());
            report.Check("device-reset", false, error);
            ok = false;
        }
        else
        {
            const std::size_t differing = CountDifferingPixels(rendered, afterReset);
            const ImageStats resetStats = Measure(afterReset);
            report.Check("device-reset",
                differing == 0 && resetStats.coveredPixels > 0 && resetStats.brightestChannel > 8,
                format("%zu pixels differ from the pre-reset frame (covered=%zu brightest=%u)",
                    differing, resetStats.coveredPixels, resetStats.brightestChannel));
        }
    }

    // --- output ------------------------------------------------------------
    if (ok)
    {
        std::string directory = world_surface_bundle::DirectoryOf(options.outputPath);
        char command[512];
        std::snprintf(command, sizeof(command), "mkdir -p '%s'", directory.c_str());
        if (std::system(command) != 0)
            report.Line("[output] warning: could not create the output directory");
        if (!WritePpm(options.outputPath, rendered, &error))
        {
            std::fprintf(stderr, "[fail] output: %s\n", error.c_str());
            ok = false;
        }
        else
        {
            report.Line(format("[output] wrote %s (%ux%u P6)", options.outputPath.c_str(),
                rendered.width, rendered.height));
        }
    }

    // --- teardown ----------------------------------------------------------
    device->Release();
    d3d->Release();
    SDL_DestroyWindow(window);
    SDL_Quit();

    report.Line("");
    report.Line(format("[summary] checks_passed=%d checks_failed=%d exit=%d verdict=%s",
        report.checksPassed, report.checksFailed,
        (ok && report.checksFailed == 0) ? ExitPass : ExitFailure,
        (ok && report.checksFailed == 0) ? "PASS" : "FAIL"));
    report.Line("[summary] PASS means: the recorded bundle drove this device, the decoded state");
    report.Line("[summary] block was applied as recorded, a prepared texture was decoded and");
    report.Line("[summary] sampled, all four controls held (draw omitted and eye looking away");
    report.Line("[summary] leave the target clear; moving the constants and the bound texture");
    report.Line("[summary] change it), the drawn region is not black, and the frame survived an");
    report.Line("[summary] IDirect3DDevice9::Reset with every default-pool object released and");
    report.Line("[summary] rebuilt. It is not image parity with the engine and no capture was");
    report.Line("[summary] replayed.");
    report.Flush();

    if (!options.reportPath.empty() && !report.WriteTo(options.reportPath))
        std::fprintf(stderr, "[warn] could not write the report to %s\n", options.reportPath.c_str());

    return ok && report.checksFailed == 0 ? ExitPass : ExitFailure;
}
