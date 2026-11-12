// The GXM Legacy back end's draws: what D3D9 state becomes in sceGxm.
//
// - Scenes. sceGxm draws inside scenes, one colour surface and one
//   depth-stencil surface each. The scene opens at the first draw or clear
//   after the targets change and closes when they change again, before the
//   CPU reads a target, and at Present. Depth-stencil surfaces load and store
//   their contents every scene, so a frame may come back to a target. When
//   the colour target has no GPU memory the scene opens on the depth surface
//   alone: clears of the depth still happen, draws do not.
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
// - A colour target that found no GPU memory (EnsureTargetMemory), or whose
//   own source had none, is unfilled: draws and copies that sample it are
//   skipped and counted, and leave their own target as it was. A full-screen
//   post effect then does not happen instead of painting the frame black.
// - Memory the GPU may still read (a DISCARD's old buffer, a destroyed
//   resource, released programs) is freed three frames later: the display
//   queue lets the CPU run at most two frames ahead.
//
// See gxm_legacy.cpp for the rest of the back end, docs/vita-client.md.

#if !defined(KISAK_GFX_GXM)
#error "gxm_legacy_draw.cpp is the PS Vita's Legacy back end (KISAK_GFX_GXM)"
#endif

#include "gxm_legacy_internal.h"

#include <gfx_gxm/gxm_draw_label.h>
#include <gfx_gxm/gxm_engine_options.h>
#include <gfx_gxm/gxm_program_cache.h>

#include <algorithm>
#include <psp2/kernel/threadmgr.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace gfx_gxm
{
const char *g_drawPass = nullptr;
const char *g_drawSurface = nullptr;
bool g_depthPrepass = true;
} // namespace gfx_gxm

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

// The programs a draw ran with, kept by what they depend on. Finding them
// the long way takes the vertex input plan, two variant keys and two map
// keys built as strings, for every draw, and a frame of a map is thousands
// of draws from a few hundred combinations. A draw looks its combination up
// in a small table first (FindPipeline) and takes the long way only when it
// is not there. Only programs that were made are kept, so a draw that waits
// for its program or is refused one takes the long way and is counted there.
struct PipelineKey
{
    const Shader *vs, *ps;
    const VertexDecl *decl;
    uint32_t strides[kMaxStreams];
    uint8_t alphaFunc; // the pixel shader variant's alpha test
    uint8_t index32;
    uint8_t pad[2];
};
struct Pipeline
{
    PipelineKey key; // key.vs null: empty
    Variant *vertex, *pixel;
    const VertexProgramEntry *vertexProgram;
    // The fragment program for the blend state and the target format it was
    // last drawn with; another takes the long way and takes its place.
    const FragmentProgramEntry *fragmentProgram;
    uint32_t blendBits, output;
    bool blended;
};
constexpr uint32_t kPipelines = 512; // a power of two

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
    SkipNoTargetMemory,
    SkipUnfilledSource,
    SkipCopy,
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
    "its colour target has no GPU memory",
    "it samples a render target that has no GPU memory or was not filled",
    "a StretchRect whose source or destination has no GPU memory or was not filled",
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
    // SCE_GXM_FRAGMENT_PROGRAM_DISABLED: depth and stencil only, no colour
    // written and no fragment program run (clears that keep the colour).
    uint32_t fragmentMode;
    bool operator==(const FixedState &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};
static_assert(SCE_GXM_FRAGMENT_PROGRAM_ENABLED == 0, "a zeroed FixedState leaves the fragment program on");

constexpr uint32_t kRingSegment = 512u << 10; // per frame, for DrawPrimitiveUP and clears
constexpr uint32_t kRingSegments = 3;
constexpr uint32_t kSequentialIndices = 60000; // sceGxm wants 16-bit index values below 64000
constexpr uint64_t kFramesInFlight = 3;
} // namespace

constexpr int kMaxGuessEdits = 16; // run diagnostics (kGuessEdits)

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
    Pipeline pipelines[kPipelines] = {};
    uint64_t pipelineLookups = 0, pipelineHits = 0;
    // The per-draw savings (Pipeline, kept texture views and binds, uniform
    // buffers left out). A file named no-draw-reuse beside the compiled
    // programs turns them off, to tell a fault of theirs on a console.
    bool reuse = true;
    const SceGxmVertexProgram *boundVertex = nullptr;
    const SceGxmFragmentProgram *boundFragment = nullptr;
    // What each fragment texture unit holds in the open scene (bit set in
    // unitBound), so that a draw does not set what is set already.
    SceGxmTexture unitView[SCE_GXM_MAX_TEXTURE_UNITS];
    uint32_t unitBound = 0;
    uint64_t textureBinds = 0, textureBindsLeftOut = 0, textureViewsMade = 0;
    // The default uniform buffers the bound programs last reserved hold these
    // constants: a draw with the same two programs and the same constants
    // reserves and writes neither again. Both end when either program is set
    // (SetPrograms) and with the scene.
    bool vertexUniformsLive = false, fragmentUniformsLive = false;
    uint64_t vertexUniformsVersion = 0, fragmentUniformsVersion = 0;
    uint32_t fragmentAlphaRef = 0;
    uint64_t uniformWrites = 0, uniformWritesLeftOut = 0;
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
    // Every wait for the GPU (FinishGpu): after one, nothing drawn before it
    // is still read.
    uint64_t gpuWaits = 0;
    // Run diagnostic (a file named gpu-scene-times beside the programs): the
    // CPU waits for the GPU after every scene and adds the wait to that
    // scene's target, so the log says which pass costs the GPU's time. The
    // waits keep the CPU and the GPU from working at once, so frames are
    // slower than without it.
    struct SceneTime
    {
        std::string name;
        uint64_t micros = 0, scenes = 0, draws = 0;
    };
    bool sceneTimes = false;
    std::vector<SceneTime> sceneTimeList;
    uint64_t sceneDraws = 0, sceneTimesFrames = 0;
    // Run diagnostic (gpu-draw-times): every draw is a scene of its own, ended
    // and waited for, and the wait goes to its pair of programs. Each such
    // scene also loads and stores its whole targets, so the smallest time a
    // draw shows is mostly that, not the draw.
    struct DrawTime
    {
        uint64_t vertexKey = 0, pixelKey = 0;
        std::string target;
        uint64_t micros = 0, draws = 0, indices = 0, slowest = 0;
    };
    bool drawTimes = false;
    std::vector<DrawTime> drawTimeList;
    // The same waits by what the engine said it was drawing (gxm_draw_label.h):
    // the pass of the frame and the kind of surface.
    struct PassTime
    {
        const char *pass = nullptr, *surface = nullptr;
        uint64_t micros = 0, draws = 0, indices = 0, slowest = 0;
    };
    std::vector<PassTime> passTimeList;
    uint64_t drawTimesFrames = 0, drawTimesFastest = ~0ull, drawTimesSince = 0;
    uint64_t waitsBy[size_t(WaitCause::Count)] = {}, waitsLogged[size_t(WaitCause::Count)] = {};
    // Which frames the GPU has drawn (gxm_gpu_progress.h), from the
    // notification the last scene of each frame carries. `overlap` off (a
    // file named no-frame-overlap beside the programs, or notifications that
    // never came): the CPU waits for the whole GPU at every Present, as the
    // back end did before.
    gfx_gxm::GpuProgress progress;
    volatile uint32_t *notifications = nullptr;
    bool overlap = true, notifyScene = false;
    uint32_t paceTimeouts = 0;
    uint64_t paceWaits = 0, paceMicros = 0;
    // Renames that found no room and waited for the GPU, renames a wait since
    // the buffer's last draw made needless, and the bytes of renamed buffers'
    // old memory the GPU may still read.
    uint64_t renameWaits = 0, renamesSpared = 0, renameSwaps = 0, textureSwaps = 0;
    // Memory a discarded buffer left and the GPU may still read; the next
    // discard of a buffer of its size and use takes it once the GPU is done
    // (Rename).
    struct KeptCopy
    {
        Memory memory;
        uint64_t drawnWait = 0, drawnStamp = 0;
    };
    std::vector<KeptCopy> kept;
    uint32_t copyBytes = 0; // the kept pieces and the second pieces of textures
    uint32_t drawFailures = 0, uniformFailures = 0;
    uint64_t copies = 0;
    uint32_t variantsMade = 0, variantFailures = 0;

    // Run diagnostics (see DiagnoseFrame): draws of this frame still to log,
    // and this frame's test (DiagMode).
    uint32_t diagnosedDraws = 0;
    // Run diagnostic: a file named diag-state-census beside the compiled
    // programs logs each new combination of shaders, declaration, blend and
    // texture state a draw brings, once (LogCensus).
    bool census = false;
    std::set<uint64_t> censusSeen;
    uint32_t censusLines = 0;
    int diagMode = 0;
    // The tests and the magenta rectangle only run when the program cache
    // folder holds a file named "run-tests"; frame 300's log and the two
    // read-backs always do.
    bool diagTests = false;
    bool diagReadBack = false; // frames 300 and 700 (and the tests) read the back buffer
    Variant diagGreen; // a fragment program that writes opaque green
    // A vertex program that takes the game's POSITION as screen pixels.
    Variant diagPass;
    const SceGxmProgramParameter *diagPassPosition = nullptr;
    std::map<uint32_t, SceGxmVertexProgram *> diagPassPrograms;
    // A hand-written vertex program that does what the game's does to the
    // position: four dots with the constants c[0] to c[3].
    Variant diagMimic;
    const SceGxmProgramParameter *diagMimicPosition = nullptr;
    std::map<uint32_t, SceGxmVertexProgram *> diagMimicPrograms;
    // A fragment program that shows TEXCOORD0's x, y, z as the colour.
    Variant diagProbe;
    // Shaders written for these tests in the shape of the menu's vertex
    // shader (kDiagTests): two go through the translator like the game's,
    // one is plain Cg.
    Shader diagSynthetic[3];
    Variant diagPlain;
    // The guess's translation with one thing changed each (kGuessEdits).
    Variant diagEdited[kMaxGuessEdits];
};

// Run diagnostics (DiagnoseDraw, DiagnoseFrame). Frame 300's first 8 draws
// are logged at length. With a file named read-back in the program cache
// folder the back buffer is read at frames 300 and 700. With one named
// run-tests it is read there too, a magenta rectangle is drawn from frame 600
// to show in frame 700's count, and then come the tests on the game's own
// draws: one every 300 frames from frame 900, on for 180 frames, 2 draws
// logged 60 frames in and the back buffer read 100 frames in.
//
// The back buffer is only read when asked because of what a read costs in
// Vita3K. Up to PC run H1 frames 300 and 700 were read in every run, and the
// menu went from 43 frames a second to 12 after frame 300 and to 6 after
// frame 700, with a frame of 250 ms in every three: the two reads touched
// two of the three display buffers, and from then on every frame drawn into
// a buffer the game had once read was slow (the emulator, it seems, then
// keeps the buffer's memory up to date for the game). The read told nothing
// there either: it counted no lit pixel while the window showed the menu.
// Without the reads (PC run H2) the same menu start drew 5135 frames in
// 120 s where it had drawn 1174.
// On a console it costs its own time only and counts what is shown (run K:
// 480359 of 522240 pixels lit).
//
// Runs H to J: the menu's translated vertex shader computed a position
// whose every w term was missing. A shader written here in its shape (the
// guess, kGuessShader; its program came out byte for byte the menu's) did
// the same once translated: its first statement was
//   r0 = (v0.xyzx * k8.xxxy + k8.yyyx);   with k8 = (1, 0, 0, 0)
// and the console's compiler folded the w component, x * 0 + 1, to 0. The
// translator now writes that statement another way (WithoutZeroProducts in
// gxm_cg_translate.cpp). These tests compile the guess's translation as it
// is now, as it was, and with that statement written other ways, to see
// which spellings the compiler gets right; then they draw the game's own
// program in green.
constexpr uint64_t kDiagnosedFrame = 300, kReadBackFrames[] = {300, 700};
constexpr uint64_t kTestRectFirst = 600, kTestRectLast = 779;
constexpr uint64_t kTestsFirst = 900, kTestEvery = 300, kTestFrames = 180, kTestLogged = 60, kTestRead = 100;
struct DiagTest
{
    const char *name;
    uint8_t option;    // gxm::cg::Options::diagPosition for the vertex program
    uint8_t standIn;   // 1: the pass-through program, 2: the mimic
    uint8_t synthetic; // draws with the menu's vertex shader use: 1 the guess, 3 four dots, 4 eight dots, all
                       // translated like the game's; 2 the guess as plain Cg; 5 the guess's translation edited
    bool probe;        // TEXCOORD0 shown as the colour (else opaque green)
    int8_t edit;       // synthetic 5: which of kGuessEdits
};
// The lines of the guess's translation these replace. An edit starts from
// the translation as it was before the fix (kGuessMad), or as it is now.
constexpr char kGuessConstant[] = "    const float4 k8 = float4(1.0, 0.0, 0.0, 0.0);\n";
constexpr char kGuessMadNow[] =
    "    r0 = float4((v0.xyzx * float4(1.0, 1.0, 1.0, 0.0) + float4(0.0, 0.0, 0.0, 1.0)).xyz, (float4(0.0, 0.0, 0.0, 1.0)).w);\n";
constexpr char kGuessMad[] = "    r0 = (v0.xyzx * k8.xxxy + k8.yyyx);\n";
constexpr char kGuessRegister[] = "    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);\n";
constexpr char kGuessMain[] = "void main(";
struct GuessEdit
{
    const char *name;
    bool asNow; // no edit: the translator's text as it is
    const char *find[2];
    const char *replace[2];
    bool fastMath;
    int optimization;
};
constexpr GuessEdit kGuessEdits[] = {
    {"guess as the translator writes it now (the fix)", true, {nullptr, nullptr}, {nullptr, nullptr}, true, 3},
    {"guess as translated before (black in run J)", false, {nullptr, nullptr}, {nullptr, nullptr}, true, 3},
    {"guess: constant not const",
     false,
     {kGuessConstant, nullptr},
     {"    float4 k8 = float4(1.0, 0.0, 0.0, 0.0);\n", nullptr},
     true,
     3},
    {"guess: swizzles of the constant written as literals",
     false,
     {kGuessMad, nullptr},
     {"    r0 = (v0.xyzx * float4(1.0, 1.0, 1.0, 0.0) + float4(0.0, 0.0, 0.0, 1.0));\n", nullptr},
     true,
     3},
    {"guess: multiply and add in two statements",
     false,
     {kGuessMad, nullptr},
     {"    r0 = v0.xyzx * k8.xxxy;\n    r0 = r0 + k8.yyyx;\n", nullptr},
     true,
     3},
    {"guess: register not zeroed first", false, {kGuessRegister, nullptr}, {"    float4 r0;\n", nullptr}, true, 3},
    {"guess: constant static const outside main",
     false,
     {kGuessConstant, kGuessMain},
     {"", "static const float4 k8 = float4(1.0, 0.0, 0.0, 0.0);\nvoid main("},
     true,
     3},
    {"guess: constructors in place of swizzles",
     false,
     {kGuessMad, nullptr},
     {"    r0 = (v0.xyzx * float4(k8.x, k8.x, k8.x, k8.y) + float4(k8.y, k8.y, k8.y, k8.x));\n", nullptr},
     true,
     3},
    {"guess: multiply-add through a function",
     false,
     {kGuessMad, kGuessMain},
     {"    r0 = kisak_mad(v0.xyzx, k8.xxxy, k8.yyyx);\n",
      "float4 kisak_mad(float4 a, float4 b, float4 m)\n{\n    return a * b + m;\n}\nvoid main("},
     true,
     3},
    {"guess: one component at a time",
     false,
     {kGuessMad, nullptr},
     {"    r0.x = v0.x * k8.x + k8.y;\n    r0.y = v0.y * k8.x + k8.y;\n    r0.z = v0.z * k8.x + k8.y;\n"
      "    r0.w = v0.x * k8.y + k8.x;\n",
      nullptr},
     true,
     3},
    {"guess as before, compiled without fast maths", false, {nullptr, nullptr}, {nullptr, nullptr}, false, 3},
    {"guess as before, compiled at optimisation level 2", false, {nullptr, nullptr}, {nullptr, nullptr}, true, 2},
    {"guess as before, compiled at optimisation level 0", false, {nullptr, nullptr}, {nullptr, nullptr}, true, 0},
    {"guess: literals, compiled without fast maths",
     false,
     {kGuessMad, nullptr},
     {"    r0 = (v0.xyzx * float4(1.0, 1.0, 1.0, 0.0) + float4(0.0, 0.0, 0.0, 1.0));\n", nullptr},
     false,
     3},
};
constexpr int kGuessEditCount = int(sizeof(kGuessEdits) / sizeof(kGuessEdits[0]));
static_assert(kGuessEditCount <= kMaxGuessEdits, "DrawState::diagEdited is too short");
// The edits in order, then these.
constexpr DiagTest kDiagTests[] = {
    {"the game's own programs, green", 0, 0, 0, false, -1},
    {"pass-through", 0, 1, 0, false, -1},
};
constexpr int kDiagTestCount = kGuessEditCount + int(sizeof(kDiagTests) / sizeof(kDiagTests[0]));
// Test t (from 0).
constexpr DiagTest DiagTestAt(int t)
{
    return t < kGuessEditCount ? DiagTest{kGuessEdits[t].name, 0, 0, 5, false, int8_t(t)} : kDiagTests[t - kGuessEditCount];
}
// The test of this frame, from 1; 0 for none.
int DiagModeFor(uint64_t frame, uint64_t *into = nullptr)
{
    if (frame < kTestsFirst)
        return 0;
    const uint64_t test = (frame - kTestsFirst) / kTestEvery;
    const uint64_t at = (frame - kTestsFirst) % kTestEvery;
    if (test >= uint64_t(kDiagTestCount) || at >= kTestFrames)
        return 0;
    if (into)
        *into = at;
    return int(test) + 1;
}

// The guess at the menu's vertex shader, from what the run logs say of it
// (vs_3_0, 11 instructions, constants c0 to c7, position, colour and
// texture coordinate in and out): the position with w 1 through the rows
// c4 to c7, then through c0 to c3.
//   def c8, 1, 0, 0, 0 / dcl_position v0 / dcl_color v1 / dcl_texcoord v2
//   dcl_position o0 / dcl_color o1 / dcl_texcoord o2
//   mad r0, v0.xyzx, c8.xxxy, c8.yyyx
//   dp4 r1.x, r0, c4 (.y c5, .z c6, .w c7) / dp4 o0.x, r1, c0 (.y c1, .z c2, .w c3)
//   mov o1, v1 / mov o2, v2
constexpr uint32_t kGuessShader[] = {
    0xfffe0300, 0x05000051, 0xa00f0008, 0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x0200001f, 0x80000000,
    0x900f0000, 0x0200001f, 0x8000000a, 0x900f0001, 0x0200001f, 0x80000005, 0x900f0002, 0x0200001f, 0x80000000,
    0xe00f0000, 0x0200001f, 0x8000000a, 0xe00f0001, 0x0200001f, 0x80000005, 0xe00f0002, 0x04000004, 0x800f0000,
    0x90240000, 0xa0400008, 0xa0150008, 0x03000009, 0x80010001, 0x80e40000, 0xa0e40004, 0x03000009, 0x80020001,
    0x80e40000, 0xa0e40005, 0x03000009, 0x80040001, 0x80e40000, 0xa0e40006, 0x03000009, 0x80080001, 0x80e40000,
    0xa0e40007, 0x03000009, 0xe0010000, 0x80e40001, 0xa0e40000, 0x03000009, 0xe0020000, 0x80e40001, 0xa0e40001,
    0x03000009, 0xe0040000, 0x80e40001, 0xa0e40002, 0x03000009, 0xe0080000, 0x80e40001, 0xa0e40003, 0x02000001,
    0xe00f0001, 0x90e40001, 0x02000001, 0xe00f0002, 0x90e40002, 0x0000ffff,
};
// The same inputs and outputs; the position straight through c0 to c3.
//   dp4 o0.x, v0, c0 (.y c1, .z c2, .w c3) / mov o1, v1 / mov o2, v2
constexpr uint32_t kFourDotsShader[] = {
    0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x8000000a, 0x900f0001, 0x0200001f, 0x80000005,
    0x900f0002, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x8000000a, 0xe00f0001, 0x0200001f, 0x80000005,
    0xe00f0002, 0x03000009, 0xe0010000, 0x90e40000, 0xa0e40000, 0x03000009, 0xe0020000, 0x90e40000, 0xa0e40001,
    0x03000009, 0xe0040000, 0x90e40000, 0xa0e40002, 0x03000009, 0xe0080000, 0x90e40000, 0xa0e40003, 0x02000001,
    0xe00f0001, 0x90e40001, 0x02000001, 0xe00f0002, 0x90e40002, 0x0000ffff,
};
// The guess without its first instruction: w from the vertex, not set to 1.
//   dp4 r1.x, v0, c4 (.y c5, .z c6, .w c7) / dp4 o0.x, r1, c0 (.y c1, .z c2, .w c3) / mov o1, v1 / mov o2, v2
constexpr uint32_t kEightDotsShader[] = {
    0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x8000000a, 0x900f0001, 0x0200001f, 0x80000005,
    0x900f0002, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x8000000a, 0xe00f0001, 0x0200001f, 0x80000005,
    0xe00f0002, 0x03000009, 0x80010001, 0x90e40000, 0xa0e40004, 0x03000009, 0x80020001, 0x90e40000, 0xa0e40005,
    0x03000009, 0x80040001, 0x90e40000, 0xa0e40006, 0x03000009, 0x80080001, 0x90e40000, 0xa0e40007, 0x03000009,
    0xe0010000, 0x80e40001, 0xa0e40000, 0x03000009, 0xe0020000, 0x80e40001, 0xa0e40001, 0x03000009, 0xe0040000,
    0x80e40001, 0xa0e40002, 0x03000009, 0xe0080000, 0x80e40001, 0xa0e40003, 0x02000001, 0xe00f0001, 0x90e40001,
    0x02000001, 0xe00f0002, 0x90e40002, 0x0000ffff,
};
// The guess as plain Cg, for a D3DCOLOR colour and a two-float coordinate.
constexpr char kGuessPlainCg[] =
    "// KisakCOD: diagnostics\nuniform float4 c[8];\n"
    "void main(float4 in_v0, float4 in_v1, float2 in_v2, out float4 out_POSITION : POSITION,\n"
    "          out float4 out_COLOR0 : COLOR0, out float4 out_TEXCOORD0 : TEXCOORD0)\n{\n"
    "    float4 p = float4(in_v0.xyz, 1.0);\n"
    "    float4 w = float4(dot(p, c[4]), dot(p, c[5]), dot(p, c[6]), dot(p, c[7]));\n"
    "    out_POSITION = float4(dot(w, c[0]), dot(w, c[1]), dot(w, c[2]), dot(w, c[3]));\n"
    "    out_COLOR0 = in_v1.zyxw;\n"
    "    out_TEXCOORD0 = float4(in_v2, 0.0, 1.0);\n}\n";

namespace
{
SceGxmContext *Gxm()
{
    return gxm::Context();
}

// The frame being filled, counted from 1 (GpuProgress stamps).
uint64_t Stamp(const DrawState &d)
{
    return d.frame + 1;
}

void PollGpu(DrawState &d)
{
    if (d.notifications)
        d.progress.Poll([&d](uint32_t word) { return d.notifications[word]; });
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

// A program's size and parameters, for the run diagnostics.
std::string DescribeProgram(const SceGxmProgram *program)
{
    char part[160];
    const unsigned parameters = sceGxmProgramGetParameterCount(program);
    std::snprintf(part, sizeof(part), "%u bytes, %u parameters, uniform buffer %u bytes:",
                  unsigned(sceGxmProgramGetSize(program)), parameters,
                  unsigned(sceGxmProgramGetDefaultUniformBufferSize(program)));
    std::string line = part;
    for (unsigned i = 0; i < parameters && i < 24; ++i)
    {
        const SceGxmProgramParameter *p = sceGxmProgramGetParameter(program, i);
        if (!p)
            continue;
        const char *name = sceGxmProgramParameterGetName(p);
        std::snprintf(part, sizeof(part), " %s(cat %u type %u x%u [%u] cont %u res %u)", name ? name : "?",
                      unsigned(sceGxmProgramParameterGetCategory(p)), unsigned(sceGxmProgramParameterGetType(p)),
                      sceGxmProgramParameterGetComponentCount(p), sceGxmProgramParameterGetArraySize(p),
                      sceGxmProgramParameterGetContainerIndex(p), sceGxmProgramParameterGetResourceIndex(p));
        line += part;
    }
    return line;
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
        variant.depthUniforms |= variant.depth[i] != nullptr;
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
           (options.farPlaneInside ? 0x5a5a000000000000ull : 0) ^ (uint64_t(options.diagPosition) * 0x9e3779b97f4a7c15ull);
}

// The shader's program for these options. It is asked for on first use and
// made on the program cache's thread (the default one was asked for when the
// shader was created); the first call that finds it ready registers it.
// Null while it is being made, with waiting set: the draw is left out of
// this frame and its program moves to the front of the queue, so no frame
// waits for the card or the compiler. Null as well when it cannot be had
// (the cache's failures are logged once a frame, LogProgramFailures).
Variant *GetVariant(Device &device, Shader &shader, const gxm::cg::Options &options, bool &waiting)
{
    const uint64_t key = VariantKey(options);
    DrawState &d = *device.draw;
    Variant *found = nullptr;
    for (const auto &variant : shader.variants)
        if (variant->key == key)
        {
            found = variant.get();
            break;
        }
    if (!found)
    {
        std::unique_ptr<Variant> variant(new Variant);
        variant->key = key;
        if (key == 0)
            variant->request = shader.request;
        else
        {
            const gxm::ShaderStage stage = shader.pixel ? gxm::ShaderStage::Fragment : gxm::ShaderStage::Vertex;
            std::string cg, error;
            if (gxm::cg::Emit(shader.parsed, options, cg, error))
            {
                char label[64];
                std::snprintf(label, sizeof(label), "%s shader variant %016llx", shader.pixel ? "pixel" : "vertex",
                              (unsigned long long)key);
                variant->request = gxm::RequestProgram(std::move(cg), stage, label);
            }
            else if (d.variantFailures++ < 20)
                Log(device, "%s shader variant %016llx was not translated: %s", shader.pixel ? "pixel" : "vertex",
                    (unsigned long long)key, error.c_str());
        }
        if (variant->request)
            variant->cacheKey = variant->request->key;
        else
            variant->failed = true;
        ++d.variantsMade;
        shader.variants.push_back(std::move(variant));
        found = shader.variants.back().get();
    }
    if (found->request)
    {
        switch (found->request->state.load(std::memory_order_acquire))
        {
        case gxm::ProgramState::Waiting:
            gxm::HurryProgram(found->request);
            waiting = true;
            return nullptr;
        case gxm::ProgramState::Ready:
            // A copy: the default variant's request stays with its shader.
            found->gxp = found->request->program;
            Register(device, *found, "shader variant");
            break;
        case gxm::ProgramState::Failed:
            found->failed = true;
            if (key != 0)
                ++d.variantFailures;
            break;
        }
        found->request.reset();
    }
    return found->failed ? nullptr : found;
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

// Run diagnostics: whether this draw's vertex shader and vertex layout are
// the menu's, as far as the reflection tells (kGuessShader).
bool MenuShaped(const Shader &shader, const InputPlan &plan)
{
    const gxm::cg::Reflection &r = shader.parsed.reflection;
    if (r.instructions != 11 || r.floatConstants != 8 || r.relativeConstants || r.inputs.size() != 3)
        return false;
    static const uint8_t usage[3] = {d3d::DECLUSAGE_POSITION, d3d::DECLUSAGE_COLOR, d3d::DECLUSAGE_TEXCOORD};
    for (const gxm::cg::VertexInput &input : r.inputs)
        if (input.reg > 2 || input.usage != usage[input.reg] || input.usageIndex != 0)
            return false;
    return plan.options.absentInputs == 0 && plan.options.bgraInputs == 2 && plan.options.inputComponents[0] == 0 &&
           plan.options.inputComponents[2] == 2;
}

template <class T> void Append(std::string &key, const T &value)
{
    key.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

// The table's place for a key. The caller compares the key that is there.
Pipeline &PipelineSlot(DrawState &d, const PipelineKey &key)
{
    // Heap addresses differ above their low alignment bits.
    uint32_t hash = uint32_t(reinterpret_cast<uintptr_t>(key.vs) >> 4) * 0x9e3779b1u;
    hash ^= uint32_t(reinterpret_cast<uintptr_t>(key.ps) >> 4) * 0x85ebca6bu;
    hash ^= uint32_t(reinterpret_cast<uintptr_t>(key.decl) >> 4) * 0xc2b2ae35u;
    hash ^= (uint32_t(key.alphaFunc) << 1 | key.index32) * 0x27d4eb2fu;
    return d.pipelines[(hash >> 16) & (kPipelines - 1)];
}

// Every kept combination goes when a program, a shader or a vertex layout
// does: a table entry points at them, and a new object may get the address
// of a dead one.
void ForgetPipelines(DrawState &d)
{
    std::memset(static_cast<void *>(d.pipelines), 0, sizeof(d.pipelines));
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
    {
        // Refused before: every draw that asks again is a draw left out.
        if (!found->second.program)
            CountSkip(device, SkipFragmentProgram);
        return found->second.program ? &found->second : nullptr;
    }
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

    // Targets take their memory when a scene first draws to them.
    for (Surface *surface : {color, depth})
        if (surface && surface->kind == SurfaceKind::TextureLevel && surface->texture)
            EnsureTargetMemory(device, *surface->texture);

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
    else if (color && color->kind == SurfaceKind::TextureLevel && color->texture->deferredBytes)
    {
        // No GPU memory for it now (EnsureTargetMemory said so, and tries
        // again). The scene opens on the depth surface alone, so that clears
        // of the depth still happen: the renderer clears a frame's depth
        // while its depth copy (the float-Z target) is the colour target, and
        // a frame drawn on depth nobody cleared is black.
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
    const uint64_t from = Micros();
    const int result =
        sceGxmBeginScene(Gxm(), 0, target, nullptr, nullptr, sync, colorSurface, depthSurface);
    device.frameTime.SceneStart(Micros() - from);
    if (result < 0)
    {
        if (device.draw->scenes == 0 || device.draw->skipped[SkipNoScene] == 0)
            Log(device, "sceGxmBeginScene %ux%u (colour %s, depth %s) failed (0x%08x)", width, height,
                colorSurface ? "yes" : "no", depthSurface ? "yes" : "no", unsigned(result));
        return false;
    }
    d.sceneOpen = true;
    d.sceneDraws = 0;
    d.sceneColor = color;
    d.sceneDepth = depth;
    d.sceneWidth = width;
    d.sceneHeight = height;
    d.boundVertex = nullptr;
    d.boundFragment = nullptr;
    d.fixedValid = false;
    d.unitBound = 0;
    d.vertexUniformsLive = false;
    d.fragmentUniformsLive = false;
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
    sceGxmSetFrontFragmentProgramEnable(context, SceGxmFragmentProgramMode(state.fragmentMode));
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

// False when the draw samples an unfilled colour target and must not happen.
bool BindTextures(Device &device, const Variant &pixel, void *uniforms)
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
        else if (Unfilled(*texture))
            return false;
        else if (texture->sampleable)
        {
            view = texture->sampled;
            texture->drawnWait = d.gpuWaits;
            texture->drawnStamp = Stamp(d);
            if (texture->desc.kind == TextureKind::Volume)
                depth = float(texture->desc.depth);
        }
        else if (texture->deferredBytes && !texture->format->depth)
            view = d.black; // a render target nothing has drawn to yet
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
        const bool linearMip = ss[d3d::SAMP_MIPFILTER] == d3d::TEXF_LINEAR;
        // Everything the five calls below take from the sampler state.
        const uint32_t bits = uint32_t(linearMin) | uint32_t(linearMag) << 1 | uint32_t(linearMip) << 2 |
                              (ss[d3d::SAMP_ADDRESSU] & 0xffu) << 8 | (ss[d3d::SAMP_ADDRESSV] & 0xffu) << 16;
        // Its own view, not a stand-in's: the one kept with the texture serves.
        const bool own = d.reuse && texture && texture->sampleable;
        if (own && texture->viewBits == bits && std::memcmp(&texture->viewOf, &view, sizeof(view)) == 0)
            view = texture->view;
        else
        {
            const SceGxmTexture plainView = view;
            sceGxmTextureSetMagFilter(&view, linearMag ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
            sceGxmTextureSetMinFilter(&view, linearMin ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
            sceGxmTextureSetMipFilter(&view, linearMip ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                       : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
            sceGxmTextureSetUAddrMode(&view, AddressMode(ss[d3d::SAMP_ADDRESSU]));
            sceGxmTextureSetVAddrMode(&view, AddressMode(ss[d3d::SAMP_ADDRESSV]));
            ++d.textureViewsMade;
            if (own)
            {
                texture->viewOf = plainView;
                texture->view = view;
                texture->viewBits = bits;
            }
        }
        const unsigned unit = unsigned(pixel.textureUnit[i]);
        ++d.textureBinds;
        if (d.reuse && unit < SCE_GXM_MAX_TEXTURE_UNITS && (d.unitBound & (1u << unit)) &&
            std::memcmp(&d.unitView[unit], &view, sizeof(view)) == 0)
            ++d.textureBindsLeftOut;
        else
        {
            sceGxmSetFragmentTexture(Gxm(), unit, &view);
            if (unit < SCE_GXM_MAX_TEXTURE_UNITS)
            {
                d.unitView[unit] = view;
                d.unitBound |= 1u << unit;
            }
        }
        if (pixel.depth[i] && uniforms)
            sceGxmSetUniformDataF(uniforms, pixel.depth[i], 0, 1, &depth);
    }
    return true;
}

// The colour target of the open scene, when it is a texture.
Texture *SceneTexture(const DrawState &d)
{
    return d.sceneColor && d.sceneColor->kind == SurfaceKind::TextureLevel ? d.sceneColor->texture : nullptr;
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

// One program a target format, with no blend info. A clear that keeps the
// colour draws with the fragment program switched off instead of a colour
// mask: a console refuses a program with blend info on a float target
// (SCE_GXM_ERROR_UNSUPPORTED; Vita3K makes it), and that was the clear of
// depth and stencil before every float-Z pass.
SceGxmFragmentProgram *ClearFragmentProgram(Device &device)
{
    DrawState &d = *device.draw;
    const FragmentProgramEntry *entry = GetFragmentProgram(device, d.clearFragment, d.clearVertex, nullptr);
    return entry ? entry->program : nullptr;
}

void SetPrograms(Device &device, const SceGxmVertexProgram *vertex, const SceGxmFragmentProgram *fragment)
{
    DrawState &d = *device.draw;
    if (d.boundVertex == vertex && d.boundFragment == fragment)
        return;
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
    // A reserved default uniform buffer is its program's until the program
    // is set anew. Both end here when one program changes: the pair is what
    // other sceGxm code is known to rely on.
    d.vertexUniformsLive = false;
    d.fragmentUniformsLive = false;
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
    ForgetPipelines(d);
    d.vertexUniformsLive = false;
    d.fragmentUniformsLive = false;
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

// Run diagnostic (swizzle-all-textures): uncompressed 2D textures that are
// not dynamic are swizzled too, so that a console run can compare the GPU's
// time with them read from the linear layout.
bool swizzleAll = false;
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
    if (gxm::ProgramCacheMarker("diag-state-census"))
    {
        d.census = true;
        Log(device, "diag: diag-state-census is there: each new draw state is logged once (census lines)");
    }
    if (gxm::ProgramCacheMarker("gpu-scene-times"))
    {
        d.sceneTimes = true;
        Log(device, "diag: gpu-scene-times is there: the CPU waits for the GPU after every scene, and the GPU's "
                    "time is logged by scene every 60 frames (frames are slower with it)");
    }
    if (gxm::ProgramCacheMarker("gpu-draw-times"))
    {
        d.drawTimes = true;
        Log(device, "diag: gpu-draw-times is there: every draw is a scene of its own and is waited for, and the "
                    "GPU's time is logged by program pair every 60 frames or 10 s (frames are much slower with it)");
    }
    gfx_gxm::g_depthPrepass = !gxm::ProgramCacheMarker("no-depth-prepass");
    if (!gfx_gxm::g_depthPrepass)
        Log(device, "diag: no-depth-prepass is there: the scene's depth is drawn first only when r_depthPrepass, "
                    "depth of field or soft particles ask for it");
    if (gxm::ProgramCacheMarker("swizzle-all-textures"))
    {
        swizzleAll = true;
        Log(device, "diag: swizzle-all-textures is there: uncompressed 2D textures that are not dynamic are "
                    "swizzled like the compressed ones");
    }
    d.notifications = sceGxmGetNotificationRegion();
    if (gxm::ProgramCacheMarker("no-frame-overlap"))
    {
        d.overlap = false;
        Log(device, "diag: no-frame-overlap is there: the CPU waits for the whole GPU at every Present, so the two "
                    "take turns (frames are slower with it)");
    }
    else if (!d.notifications)
    {
        d.overlap = false;
        Log(device, "sceGxm gave no notification region: the CPU waits for the whole GPU at every Present");
    }
    if (gxm::ProgramCacheMarker("no-draw-reuse"))
    {
        d.reuse = false;
        Log(device, "diag: no-draw-reuse is there: every draw finds its programs, sets its textures and writes its "
                    "uniform buffers anew");
    }
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
    if (!gxm::GetProgram("// KisakCOD: diagnostics\nuniform float4 c[4];\n"
                         "void main(float4 in_position, out float4 out_POSITION : POSITION)\n{\n"
                         "    out_POSITION = float4(dot(in_position, c[0]), dot(in_position, c[1]), "
                         "dot(in_position, c[2]), dot(in_position, c[3]));\n}\n",
                         gxm::ShaderStage::Vertex, d.diagMimic.gxp, log))
        Log(device, "diag: the mimic program did not compile: %s", log.substr(0, log.find('\n')).c_str());
    if (Register(device, d.diagMimic, "diag mimic program"))
    {
        d.diagMimicPosition = sceGxmProgramFindParameterByName(d.diagMimic.program(), "in_position");
        if (!d.diagMimicPosition || !d.diagMimic.constants)
            d.diagMimic.failed = true;
        Log(device, "diag: mimic program %s", DescribeProgram(d.diagMimic.program()).c_str());
    }
    if (!gxm::GetProgram("// KisakCOD: diagnostics\nfloat4 main(float4 in_TEXCOORD0 : TEXCOORD0) : COLOR\n{\n"
                         "    return float4(in_TEXCOORD0.xyz, 1.0);\n}\n",
                         gxm::ShaderStage::Fragment, d.diagProbe.gxp, log))
        Log(device, "diag: the probe program did not compile: %s", log.substr(0, log.find('\n')).c_str());
    Register(device, d.diagProbe, "diag probe program");
    const struct
    {
        const uint32_t *words;
        size_t bytes;
    } synthetic[3] = {{kGuessShader, sizeof(kGuessShader)},
                      {kFourDotsShader, sizeof(kFourDotsShader)},
                      {kEightDotsShader, sizeof(kEightDotsShader)}};
    for (int i = 0; i < 3; ++i)
    {
        std::string reason;
        d.diagSynthetic[i].translated = gxm::cg::Parse(synthetic[i].words, synthetic[i].bytes, d.diagSynthetic[i].parsed, reason);
        if (!d.diagSynthetic[i].translated)
            Log(device, "diag: synthetic shader %d not translated: %s", i + 1, reason.c_str());
    }
    d.diagTests = gxm::ProgramCacheMarker("run-tests");
    d.diagReadBack = d.diagTests || gxm::ProgramCacheMarker("read-back");
    Log(device,
        "diag: draw tests %s, back buffer read at frames 300 and 700 %s (files named run-tests and read-back in the "
        "program cache folder turn them on)",
        d.diagTests ? "on" : "off", d.diagReadBack ? "on" : "off");
    if (!d.diagTests)
        return true;
    // The guess translated for the menu's vertex layout (a D3DCOLOR colour,
    // a two-float coordinate), then each edit of it.
    gxm::cg::Options menuOptions;
    menuOptions.bgraInputs = 2;
    menuOptions.inputComponents[2] = 2;
    std::string guess, reason;
    if (!d.diagSynthetic[0].translated || !gxm::cg::Emit(d.diagSynthetic[0].parsed, menuOptions, guess, reason))
        Log(device, "diag: the guess was not translated: %s", reason.c_str());
    // As it was before the translator kept zero products out.
    std::string before = guess;
    if (const size_t at = before.find(kGuessMadNow); at != std::string::npos)
        before.replace(at, std::strlen(kGuessMadNow), kGuessMad);
    else if (!guess.empty())
        Log(device, "diag: the guess's translation lacks the statement these tests are about");
    for (int i = 0; i < kGuessEditCount && !guess.empty(); ++i)
    {
        const GuessEdit &edit = kGuessEdits[i];
        std::string cg = edit.asNow ? guess : before;
        bool found = true;
        for (int e = 0; e < 2; ++e)
            if (edit.find[e])
            {
                const size_t at = cg.find(edit.find[e]);
                if (at == std::string::npos)
                    found = false;
                else
                    cg.replace(at, std::strlen(edit.find[e]), edit.replace[e]);
            }
        if (!edit.fastMath || edit.optimization != 3)
            cg = "// KisakCOD: diagnostics, fast maths " + std::to_string(int(edit.fastMath)) + ", optimisation " +
                 std::to_string(edit.optimization) + "\n" + cg;
        Variant &variant = d.diagEdited[i];
        variant.cacheKey = gxm::ProgramCacheKey(cg, gxm::ShaderStage::Vertex);
        if (!found)
            Log(device, "diag: guess edit %d: the line to replace is not in the translation", i);
        else if (!gxm::GetProgram(cg, gxm::ShaderStage::Vertex, variant.gxp, log, edit.fastMath, edit.optimization))
            Log(device, "diag: guess edit %d did not compile: %s", i, log.substr(0, log.find('\n')).c_str());
        if (Register(device, variant, "diag guess edit"))
            Log(device, "diag: guess edit %d, cache file %016llx: %s", i, (unsigned long long)variant.cacheKey,
                DescribeProgram(variant.program()).c_str());
    }
    d.diagPlain.cacheKey = gxm::ProgramCacheKey(kGuessPlainCg, gxm::ShaderStage::Vertex);
    if (!gxm::GetProgram(kGuessPlainCg, gxm::ShaderStage::Vertex, d.diagPlain.gxp, log))
        Log(device, "diag: the plain guess did not compile: %s", log.substr(0, log.find('\n')).c_str());
    if (Register(device, d.diagPlain, "diag plain guess"))
        Log(device, "diag: plain guess, cache file %016llx: %s", (unsigned long long)d.diagPlain.cacheKey,
            DescribeProgram(d.diagPlain.program()).c_str());
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
    if (swizzleAll && desc.kind == TextureKind::Tex2D && !(desc.usage & d3d::USAGE_DYNAMIC))
        return true;
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
        if (!AllocateMemory(device, texture.swizzled, bytes, 4096, d3d::POOL_DEFAULT, true,
                            TextureUse(desc, *texture.format), texture.name.c_str()))
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
        const MemoryUse use = memory.use;
        const uint32_t bytes = memory.bytes;
        Defer(device, [kind, data, use, bytes] { FreeGpuMemory(kind, data, use, bytes); });
        memory = Memory{};
        return;
    }
    FreeMemory(memory);
}

size_t FreeRetiredNow(Device &device)
{
    DrawState *d = device.draw;
    if (!d || d->deferred.empty())
        return 0;
    FinishGpu(device);
    // Moved out first, in case the work defers more.
    auto work = std::move(d->deferred);
    d->deferred.clear();
    for (auto &[frame, item] : work)
        item();
    return work.size();
}

std::string SurfaceName(const Surface *surface)
{
    if (!surface)
        return "none";
    if (surface->kind == SurfaceKind::BackBuffer)
        return "back buffer";
    if (surface->kind == SurfaceKind::FrontBuffer)
        return "front buffer";
    if (!surface->texture)
        return "?";
    std::string name = surface->texture->name.empty() ? "unnamed" : surface->texture->name;
    if (surface->level)
        name += " level " + std::to_string(surface->level);
    return name;
}

// gpu-scene-times: the scene just sent to the GPU is waited for, and the wait
// is added to its targets' line.
void TimeScene(Device &device, const Surface *color, const Surface *depth)
{
    DrawState &d = *device.draw;
    const uint64_t from = Micros();
    gxm::Finish();
    const uint64_t took = Micros() - from;
    ++d.gpuWaits;
    ++d.waitsBy[size_t(WaitCause::Other)];
    d.progress.Finished(Stamp(d) - 1);
    char size[32];
    std::snprintf(size, sizeof(size), " %ux%u", d.sceneWidth, d.sceneHeight);
    const std::string name = SurfaceName(color) + " + depth " + SurfaceName(depth) + size;
    DrawState::SceneTime *entry = nullptr;
    for (auto &e : d.sceneTimeList)
        if (e.name == name)
            entry = &e;
    if (!entry)
    {
        if (d.sceneTimeList.size() >= 32)
            return;
        d.sceneTimeList.push_back({name});
        entry = &d.sceneTimeList.back();
    }
    entry->micros += took;
    ++entry->scenes;
    entry->draws += d.sceneDraws;
    d.sceneDraws = 0;
}

// gpu-draw-times: the draw just made ends its scene and is waited for.
void TimeDraw(Device &device, const Variant &vertex, const Variant &pixel, uint32_t indexCount)
{
    DrawState &d = *device.draw;
    const std::string target = SurfaceName(d.sceneColor);
    const bool timeScene = d.sceneTimes;
    d.sceneTimes = false; // this wait is the draw's, not the scene's
    EndScene(device);
    d.sceneTimes = timeScene;
    const uint64_t from = Micros();
    gxm::Finish();
    const uint64_t took = Micros() - from;
    ++d.gpuWaits;
    ++d.waitsBy[size_t(WaitCause::Other)];
    d.progress.Finished(Stamp(d) - 1);
    d.drawTimesFastest = std::min(d.drawTimesFastest, took);
    DrawState::PassTime *pass = nullptr;
    for (auto &e : d.passTimeList)
        if (e.pass == gfx_gxm::g_drawPass && e.surface == gfx_gxm::g_drawSurface)
            pass = &e;
    if (!pass && d.passTimeList.size() < 128)
    {
        DrawState::PassTime fresh;
        fresh.pass = gfx_gxm::g_drawPass;
        fresh.surface = gfx_gxm::g_drawSurface;
        d.passTimeList.push_back(fresh);
        pass = &d.passTimeList.back();
    }
    if (pass)
    {
        pass->micros += took;
        ++pass->draws;
        pass->indices += indexCount;
        pass->slowest = std::max(pass->slowest, took);
    }
    DrawState::DrawTime *entry = nullptr;
    for (auto &e : d.drawTimeList)
        if (e.vertexKey == vertex.cacheKey && e.pixelKey == pixel.cacheKey && e.target == target)
            entry = &e;
    if (!entry)
    {
        if (d.drawTimeList.size() >= 512)
            return;
        DrawState::DrawTime fresh;
        fresh.vertexKey = vertex.cacheKey;
        fresh.pixelKey = pixel.cacheKey;
        fresh.target = target;
        d.drawTimeList.push_back(fresh);
        entry = &d.drawTimeList.back();
    }
    entry->micros += took;
    ++entry->draws;
    entry->indices += indexCount;
    entry->slowest = std::max(entry->slowest, took);
}

// Every 60 frames (or 10 s): the twelve program pairs that took the GPU
// longest.
void LogDrawTimes(Device &device)
{
    DrawState &d = *device.draw;
    // Every 60 frames, or every 10 s when frames are that slow.
    if (!d.drawTimesSince)
        d.drawTimesSince = Micros();
    if (++d.drawTimesFrames < 60 && Micros() - d.drawTimesSince < 10000000)
        return;
    d.drawTimesSince = Micros();
    std::sort(d.drawTimeList.begin(), d.drawTimeList.end(),
              [](const DrawState::DrawTime &a, const DrawState::DrawTime &b) { return a.micros > b.micros; });
    const double frames = double(d.drawTimesFrames);
    uint64_t total = 0, draws = 0;
    for (const auto &e : d.drawTimeList)
    {
        total += e.micros;
        draws += e.draws;
    }
    Log(device, "diag: gpu by draw, frames %llu to %llu: %.2f ms a frame for %.1f draws a frame, %u program pairs, "
        "fastest single draw %.3f ms (mostly the scene's own cost)",
        (unsigned long long)(d.frame - d.drawTimesFrames), (unsigned long long)d.frame, total / 1000.0 / frames,
        draws / frames, unsigned(d.drawTimeList.size()), d.drawTimesFastest == ~0ull ? 0.0 : d.drawTimesFastest / 1000.0);
    for (size_t i = 0; i < d.drawTimeList.size() && i < 12; ++i)
    {
        const auto &e = d.drawTimeList[i];
        Log(device, "diag: gpu by draw: %.2f ms a frame, %.1f draws a frame, slowest %.2f ms, %llu indices a "
            "draw: vs %016llx ps %016llx into %s",
            e.micros / 1000.0 / frames, e.draws / frames, e.slowest / 1000.0,
            (unsigned long long)(e.draws ? e.indices / e.draws : 0), (unsigned long long)e.vertexKey,
            (unsigned long long)e.pixelKey, e.target.c_str());
    }
    // Every pass and kind of surface, most first. "less floor" takes off the
    // fastest draw's time for each draw: about what the draws would cost
    // without a scene each.
    std::sort(d.passTimeList.begin(), d.passTimeList.end(),
              [](const DrawState::PassTime &a, const DrawState::PassTime &b) { return a.micros > b.micros; });
    const double floor = d.drawTimesFastest == ~0ull ? 0.0 : double(d.drawTimesFastest);
    for (size_t i = 0; i < d.passTimeList.size() && i < 32; ++i)
    {
        const auto &e = d.passTimeList[i];
        const double lessFloor = std::max(0.0, double(e.micros) - floor * double(e.draws));
        Log(device, "diag: gpu by pass: %.2f ms a frame (%.2f less floor), %.1f draws a frame, slowest %.2f ms, "
            "%llu indices a draw: %s / %s",
            e.micros / 1000.0 / frames, lessFloor / 1000.0 / frames, e.draws / frames, e.slowest / 1000.0,
            (unsigned long long)(e.draws ? e.indices / e.draws : 0), e.pass ? e.pass : "unnamed",
            e.surface ? e.surface : "-");
    }
    d.passTimeList.clear();
    d.drawTimeList.clear();
    d.drawTimesFrames = 0;
    d.drawTimesFastest = ~0ull;
}

// Every 60 frames: the GPU's time by scene target, most first.
void LogSceneTimes(Device &device)
{
    DrawState &d = *device.draw;
    if (++d.sceneTimesFrames < 60)
        return;
    std::sort(d.sceneTimeList.begin(), d.sceneTimeList.end(),
              [](const DrawState::SceneTime &a, const DrawState::SceneTime &b) { return a.micros > b.micros; });
    const double frames = double(d.sceneTimesFrames);
    uint64_t total = 0;
    for (const auto &e : d.sceneTimeList)
        total += e.micros;
    Log(device, "diag: gpu by scene, frames %llu to %llu: %.2f ms a frame in all",
        (unsigned long long)(d.frame - d.sceneTimesFrames), (unsigned long long)d.frame, total / 1000.0 / frames);
    for (const auto &e : d.sceneTimeList)
        Log(device, "diag: gpu by scene: %.2f ms a frame, %.2f scenes and %.1f draws a frame: %s",
            e.micros / 1000.0 / frames, e.scenes / frames, e.draws / frames, e.name.c_str());
    d.sceneTimeList.clear();
    d.sceneTimesFrames = 0;
}

void EndScene(Device &device)
{
    DrawState *d = device.draw;
    if (!d || !d->sceneOpen)
        return;
    d->sceneOpen = false;
    const Surface *color = d->sceneColor, *depth = d->sceneDepth;
    d->sceneColor = d->sceneDepth = nullptr;
    // The frame's last scene tells when the GPU is done with the frame.
    SceGxmNotification done{};
    const bool notify = d->notifyScene && d->overlap && d->notifications;
    d->notifyScene = false;
    if (notify)
    {
        uint32_t value = 0;
        const uint32_t word = d->progress.Submit(Stamp(*d), value);
        d->notifications[word] = 0;
        done.address = d->notifications + word;
        done.value = value;
    }
    const uint64_t from = Micros();
    const int result = sceGxmEndScene(Gxm(), nullptr, notify ? &done : nullptr);
    device.frameTime.SceneEnd(Micros() - from);
    if (result < 0)
        Log(device, "sceGxmEndScene failed (0x%08x)", unsigned(result));
    if (d->sceneTimes)
        TimeScene(device, color, depth);
}

void EndFrameScene(Device &device)
{
    if (DrawState *d = device.draw)
        d->notifyScene = d->sceneOpen;
    EndScene(device);
}

void FinishGpu(Device &device, WaitCause cause)
{
    EndScene(device);
    const uint64_t from = Micros();
    gxm::Finish();
    device.frameTime.Wait(Micros() - from);
    if (DrawState *d = device.draw)
    {
        ++d->gpuWaits;
        ++d->waitsBy[size_t(cause)];
        // Every frame before this one is drawn; what this frame has drawn so
        // far is too, which gpuWaits tells (GpuDoneWith).
        d->progress.Finished(Stamp(*d) - 1);
    }
}

// The pacing wait. The CPU may fill frame N while the GPU draws frame N - 1,
// and no more: a second frame of lead would only add a frame of delay between
// the controls and the picture. So before frame N starts, frame N - 2 must be
// drawn. That is also what lets a buffer discarded every other frame be
// written in place, and what makes memory retired three frames ago free.
// A GPU-bound game spends most of its frame here while the GPU draws; a
// CPU-bound one finds the frame drawn and does not wait.
void PaceFrame(Device &device)
{
    constexpr uint64_t kPollMicros = 250;
    constexpr uint64_t kTimeoutMicros = 2000 * 1000;
    constexpr uint32_t kTimeoutsAllowed = 3;
    DrawState &d = *device.draw;
    if (!d.overlap)
    {
        FinishGpu(device, WaitCause::EveryFrame);
        return;
    }
    const uint64_t next = Stamp(d); // the frame about to be filled
    if (next < 3)
        return;
    const uint64_t wanted = next - 2;
    PollGpu(d);
    if (d.progress.Done(wanted))
        return;
    const uint64_t from = Micros();
    bool timedOut = false;
    while (!d.progress.Done(wanted) && d.progress.Expected(wanted))
    {
        if (Micros() - from > kTimeoutMicros)
        {
            timedOut = true;
            break;
        }
        sceKernelDelayThread(kPollMicros);
        PollGpu(d);
    }
    if (d.progress.Done(wanted))
    {
        const uint64_t took = Micros() - from;
        device.frameTime.Pace(took);
        ++d.paceWaits;
        d.paceMicros += took;
        return;
    }
    // No notification will say so (the frame ended without a scene, or the
    // word never changed): wait for everything.
    FinishGpu(device, WaitCause::Pace);
    if (timedOut && ++d.paceTimeouts >= kTimeoutsAllowed)
    {
        d.overlap = false;
        Log(device,
            "the GPU's notification for a frame's last scene did not come in %u ms, %u times: from here on the CPU "
            "waits for the whole GPU at every Present (as with no-frame-overlap)",
            unsigned(kTimeoutMicros / 1000), kTimeoutsAllowed);
    }
}

void LogWaitCauses(Device &device)
{
    static const char *const kNames[size_t(WaitCause::Count)] = {
        "other (shutdown, memory, diagnostics)", "a buffer lock without a copy to write",
        "a texture rewrite without a second piece", "the CPU reading what the GPU drew",
        "pacing without a notification",          "every frame (no overlap)"};
    DrawState *d = device.draw;
    if (!d)
        return;
    std::string line;
    for (size_t i = 0; i < size_t(WaitCause::Count); ++i)
    {
        const uint64_t count = d->waitsBy[i] - d->waitsLogged[i];
        d->waitsLogged[i] = d->waitsBy[i];
        if (!count)
            continue;
        char part[96];
        std::snprintf(part, sizeof(part), "%s%llu x %s", line.empty() ? "" : ", ", (unsigned long long)count,
                      kNames[i]);
        line += part;
    }
    if (!line.empty())
        Log(device, "waits for the whole GPU since the last such line: %s", line.c_str());
}

void FrameDone(Device &device)
{
    DrawState &d = *device.draw;
    ++d.frame;
    d.ringUsed = 0;
    if (d.sceneTimes)
        LogSceneTimes(device);
    if (d.drawTimes)
        LogDrawTimes(device);
    uint64_t into = 0;
    const int mode = d.diagTests ? DiagModeFor(d.frame, &into) : 0;
    if (d.frame == kDiagnosedFrame)
        d.diagnosedDraws = 8;
    else if (mode && into == kTestLogged)
        d.diagnosedDraws = 2;
    if (mode != d.diagMode)
        Log(device, "diag: frame %llu, test %d of %d: %s", (unsigned long long)d.frame, mode, kDiagTestCount,
            mode ? DiagTestAt(mode - 1).name : "none");
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

// The engine locks its dynamic buffers with DISCARD every frame, and the GPU
// still reads what the last frame put in them: the CPU fills a frame while
// the GPU draws the one before (PaceFrame). So a discarded buffer changes
// memory with a kept piece of the same size and use that the GPU is done
// with, and its own goes to the kept ones until the GPU is done with it too.
// One kept piece serves every buffer of a kind that is discarded once a
// frame: the engine's two skinned vertex caches, which it fills in turn, go
// round three pieces of memory, not four. The pieces are kept, not freed and
// asked for again: the main pool has no room for memory that waits to be
// freed. A discard that finds no piece and no room for a new one waits for
// the whole GPU, which makes the CPU and the GPU take turns for that frame;
// the log counts those (LogWaitCauses).
//
// Locks come from two threads, never at once: the back end's as it draws,
// and the game's between two frames, when the back end is idle and the GPU
// has the frame just sent (the skinned caches, the pretessellated indices).
namespace
{
// How many pieces of one size and use are kept at most. Large buffers are
// few and discarded once a frame; of the small ones (a quad for each light
// or view) many can be discarded in one frame.
size_t KeptOfAKind(uint32_t bytes)
{
    constexpr uint32_t kLarge = 256u << 10;
    return bytes >= kLarge ? 3 : std::max<size_t>(4, std::min<size_t>(64, kLarge / std::max(bytes, 1u)));
}

bool GpuDoneWith(const DrawState &d, uint64_t drawnWait, uint64_t drawnStamp)
{
    return drawnWait != d.gpuWaits || d.progress.Done(drawnStamp);
}

bool SameKind(const Memory &kept, const GpuBuffer &buffer)
{
    return kept.gpu && kept.bytes == buffer.memory.bytes && kept.use == buffer.memory.use;
}
} // namespace

void Rename(Device &device, GpuBuffer &buffer)
{
    DrawState &d = *device.draw;
    PollGpu(d);
    if (GpuDoneWith(d, buffer.drawnWait, buffer.drawnStamp))
    {
        buffer.drawn = false;
        ++d.renamesSpared;
        return;
    }
    size_t ofThisKind = 0;
    for (DrawState::KeptCopy &kept : d.kept)
    {
        if (!SameKind(kept.memory, buffer))
            continue;
        ++ofThisKind;
        if (!GpuDoneWith(d, kept.drawnWait, kept.drawnStamp))
            continue;
        std::swap(kept.memory, buffer.memory);
        kept.drawnWait = buffer.drawnWait;
        kept.drawnStamp = buffer.drawnStamp;
        buffer.drawn = false;
        ++d.renameSwaps;
        ++device.stats.dynamicRenames;
        return;
    }
    Memory fresh;
    const bool dynamic = (buffer.usage & d3d::USAGE_DYNAMIC) != 0;
    if (ofThisKind >= KeptOfAKind(buffer.memory.bytes) ||
        !AllocateRenamed(fresh, buffer.memory.bytes, 16, dynamic ? gxm::MemoryKind::Main : gxm::MemoryKind::Cdram,
                         buffer.memory.use))
    {
        FinishGpu(device, WaitCause::Rename);
        buffer.drawn = false;
        ++d.renameWaits;
        return;
    }
    d.copyBytes += fresh.bytes;
    d.kept.push_back(DrawState::KeptCopy{std::move(buffer.memory), buffer.drawnWait, buffer.drawnStamp});
    buffer.memory = std::move(fresh);
    buffer.drawn = false;
    ++d.renames;
    ++device.stats.dynamicRenames;
}

// Buffers go at shutdown and at a renderer restart, with all their kind: the
// kept pieces of that kind go too, and are made again if a buffer still
// needs one.
void RetireCopies(Device &device, GpuBuffer &buffer)
{
    DrawState *d = device.draw;
    if (!d || !buffer.memory.gpu)
        return;
    for (size_t i = 0; i < d->kept.size();)
    {
        if (!SameKind(d->kept[i].memory, buffer))
        {
            ++i;
            continue;
        }
        d->copyBytes -= std::min(d->copyBytes, d->kept[i].memory.bytes);
        Retire(device, d->kept[i].memory);
        d->kept.erase(d->kept.begin() + i);
    }
}

void RetireCopies(Device &device, Texture &texture)
{
    if (device.draw && texture.spare.gpu)
        device.draw->copyBytes -= std::min(device.draw->copyBytes, texture.spare.bytes);
    Retire(device, texture.spare);
}

void MakeWritable(Device &device, Texture &texture)
{
    DrawState *d = device.draw;
    if (!d || !texture.memory.gpu || texture.swizzledLayout || !texture.sampleable || texture.levels.size() != 1 ||
        (texture.desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL)))
        return;
    PollGpu(*d);
    if (GpuDoneWith(*d, texture.drawnWait, texture.drawnStamp))
        return;
    if (!texture.spare.gpu)
    {
        // Beside the first piece if it can be, else in the other pool.
        const gxm::MemoryKind other =
            texture.memory.kind == gxm::MemoryKind::Cdram ? gxm::MemoryKind::Main : gxm::MemoryKind::Cdram;
        if (AllocateRenamed(texture.spare, texture.memory.bytes, 256, texture.memory.kind, texture.memory.use) ||
            AllocateRenamed(texture.spare, texture.memory.bytes, 256, other, texture.memory.use))
        {
            texture.spareWait = texture.spareStamp = 0;
            d->copyBytes += texture.spare.bytes;
        }
    }
    if (!texture.spare.gpu || !GpuDoneWith(*d, texture.spareWait, texture.spareStamp))
    {
        FinishGpu(device, WaitCause::TextureCopy);
        return;
    }
    std::swap(texture.memory, texture.spare);
    texture.spareWait = texture.drawnWait;
    texture.spareStamp = texture.drawnStamp;
    texture.drawnStamp = 0;
    // The view draws bind points at the memory: the kept one no longer
    // matches `sampled` and is made anew by the next draw.
    sceGxmTextureSetData(&texture.sampled, texture.memory.data);
    ++d->textureSwaps;
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
    SceGxmFragmentProgram *fragment = ClearFragmentProgram(device);
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
    state.fragmentMode = writeColor ? SCE_GXM_FRAGMENT_PROGRAM_ENABLED : SCE_GXM_FRAGMENT_PROGRAM_DISABLED;
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
        if (Texture *target = writeColor ? SceneTexture(d) : nullptr)
            target->unfilled = false;
    }
}

namespace
{
// ---- Run diagnostics -------------------------------------------------------------------------
//
// Stage 2d's first runs drew a black screen with no errors. These log what a
// few draws ask for and what lands in the back buffer, so a run tells a
// display problem from a draw-state one:
// - frame 300 and each test (kDiagTests): the first draws' state, the first
//   vertex's position and the first vertex shader constants;
// - when asked for (a file named read-back or run-tests), frames 300 and
//   700, and each test: what the back buffer holds before it is shown;
// - with the tests on, frames 600 to 779: a magenta rectangle (the top-left
//   quarter) drawn through the clear path after the game's frame, so it
//   shows on screen for three seconds and in frame 700's count.


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
    Log(device, "diag frame %llu draw: vs c4 %g %g %g %g, c5 %g %g %g %g, c6 %g %g %g %g, c7 %g %g %g %g",
        (unsigned long long)d.frame, c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23], c[24], c[25], c[26],
        c[27], c[28], c[29], c[30], c[31]);
    {
        const gxm::cg::Reflection &r = device.vertexShader->parsed.reflection;
        std::string inputs, varyings;
        char part[48];
        for (const gxm::cg::VertexInput &input : r.inputs)
        {
            std::snprintf(part, sizeof(part), " v%u=usage %u.%u mask 0x%x", unsigned(input.reg), unsigned(input.usage),
                          unsigned(input.usageIndex), unsigned(input.mask));
            inputs += part;
        }
        for (const gxm::cg::Varying &varying : r.varyings)
        {
            std::snprintf(part, sizeof(part), " %s%u", gxm::cg::SemanticName(varying.semantic), unsigned(varying.index));
            varyings += part;
        }
        Log(device, "diag frame %llu draw: vs %u instructions, relative constants %u; inputs:%s; writes POSITION and:%s",
            (unsigned long long)d.frame, unsigned(r.instructions), unsigned(r.relativeConstants), inputs.c_str(),
            varyings.c_str());
    }
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

// Run diagnostic (diag-state-census): the D3D9 state of each new
// combination a draw brings, in numbers, so that a run can say which state
// a group of draws (decals, say) is drawn with. Formats print as their
// D3DFORMAT number, or as four letters for a FourCC.
void FormatName(uint32_t format, char *out, size_t size)
{
    if (format > 0xffff)
        std::snprintf(out, size, "%c%c%c%c", char(format), char(format >> 8), char(format >> 16), char(format >> 24));
    else
        std::snprintf(out, size, "%u", format);
}

void LogCensus(Device &device, const Shader &vs, const Shader &ps, const Variant &vertex, const Variant &pixel,
               bool blended, const SceGxmBlendInfo &blend)
{
    DrawState &d = *device.draw;
    const uint32_t *rs = device.renderStates;
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(uint64_t(uintptr_t(&vs)));
    mix(uint64_t(uintptr_t(&ps)));
    mix(uint64_t(uintptr_t(device.decl)));
    mix(uint64_t(uintptr_t(&vertex)));
    mix(uint64_t(uintptr_t(&pixel)));
    static const uint32_t states[] = {d3d::RS_ALPHABLENDENABLE, d3d::RS_SRCBLEND, d3d::RS_DESTBLEND, d3d::RS_BLENDOP,
                                      d3d::RS_SEPARATEALPHABLENDENABLE, d3d::RS_SRCBLENDALPHA, d3d::RS_DESTBLENDALPHA,
                                      d3d::RS_BLENDOPALPHA, d3d::RS_COLORWRITEENABLE, d3d::RS_ALPHATESTENABLE,
                                      d3d::RS_ALPHAFUNC, d3d::RS_SRGBWRITEENABLE, d3d::RS_ZFUNC, d3d::RS_ZWRITEENABLE,
                                      d3d::RS_CULLMODE, d3d::RS_STENCILENABLE};
    for (uint32_t state : states)
        mix(rs[state]);
    mix(uint64_t(d.color.format));
    for (int i = 0; i < 16; ++i)
    {
        if (pixel.textureUnit[i] < 0)
            continue;
        const Texture *texture = device.textures[i];
        mix(texture ? uint64_t(texture->desc.format) : 0);
        mix(device.samplerStates[i][d3d::SAMP_SRGBTEXTURE]);
        mix(device.samplerStates[i][d3d::SAMP_ADDRESSU]);
    }
    if (!d.censusSeen.insert(key).second || d.censusLines >= 600)
        return;
    const uint32_t n = d.censusLines++;
    std::string decl;
    char part[160];
    for (const VertexElement &e : device.decl->elements)
    {
        std::snprintf(part, sizeof(part), " %u/%u/t%u/u%u.%u", unsigned(e.stream), unsigned(e.offset), unsigned(e.type),
                      unsigned(e.usage), unsigned(e.usageIndex));
        decl += part;
    }
    Log(device,
        "census %u (frame %llu, %llu drawn): vs %016llx (%u instructions, %u constants) ps %016llx (%u instructions, "
        "%u constants); target format 0x%08x%s; decl%s",
        n, (unsigned long long)d.frame, (unsigned long long)d.drawn, (unsigned long long)vertex.cacheKey,
        unsigned(vs.parsed.reflection.instructions), unsigned(vs.parsed.reflection.floatConstants),
        (unsigned long long)pixel.cacheKey, unsigned(ps.parsed.reflection.instructions),
        unsigned(ps.parsed.reflection.floatConstants), unsigned(d.color.format), d.color.noAlpha ? " (no alpha)" : "",
        decl.c_str());
    Log(device,
        "census %u state: blend %u src %u dst %u op %u, separate %u src %u dst %u op %u, write mask 0x%x, srgb write %u; "
        "alpha test %u func %u ref %u; z func %u write %u; cull %u; stencil %u; gxm blend %s (mask 0x%x, colour %u %u %u, "
        "alpha %u %u %u)",
        n, rs[d3d::RS_ALPHABLENDENABLE], rs[d3d::RS_SRCBLEND], rs[d3d::RS_DESTBLEND], rs[d3d::RS_BLENDOP],
        rs[d3d::RS_SEPARATEALPHABLENDENABLE], rs[d3d::RS_SRCBLENDALPHA], rs[d3d::RS_DESTBLENDALPHA],
        rs[d3d::RS_BLENDOPALPHA], rs[d3d::RS_COLORWRITEENABLE], rs[d3d::RS_SRGBWRITEENABLE],
        rs[d3d::RS_ALPHATESTENABLE], rs[d3d::RS_ALPHAFUNC], rs[d3d::RS_ALPHAREF], rs[d3d::RS_ZFUNC],
        rs[d3d::RS_ZWRITEENABLE], rs[d3d::RS_CULLMODE], rs[d3d::RS_STENCILENABLE], blended ? "on" : "off",
        unsigned(blend.colorMask), unsigned(blend.colorFunc), unsigned(blend.colorSrc), unsigned(blend.colorDst),
        unsigned(blend.alphaFunc), unsigned(blend.alphaSrc), unsigned(blend.alphaDst));
    std::string textures;
    for (int i = 0; i < 16; ++i)
    {
        if (pixel.textureUnit[i] < 0)
            continue;
        const Texture *texture = device.textures[i];
        const uint32_t *ss = device.samplerStates[i];
        char format[16] = "none";
        if (texture)
            FormatName(uint32_t(texture->desc.format), format, sizeof(format));
        std::snprintf(part, sizeof(part), " s%d(%s %ux%u kind %u levels %u; filter %u %u %u address %u %u srgb %u)", i,
                      format, texture ? texture->desc.width : 0u, texture ? texture->desc.height : 0u,
                      texture ? unsigned(texture->desc.kind) : 0u, texture ? unsigned(texture->levels.size()) : 0u,
                      ss[d3d::SAMP_MINFILTER], ss[d3d::SAMP_MAGFILTER], ss[d3d::SAMP_MIPFILTER],
                      ss[d3d::SAMP_ADDRESSU], ss[d3d::SAMP_ADDRESSV], ss[d3d::SAMP_SRGBTEXTURE]);
        textures += part;
    }
    Log(device, "census %u textures:%s", n, textures.empty() ? " none" : textures.c_str());
}

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
        ib->drawnWait = d.gpuWaits;
        ib->drawnStamp = Stamp(d);
    }
    else if (indexCount > kSequentialIndices)
    {
        CountSkip(device, SkipPrimitive, "a draw without indices of more than 60000 vertices");
        return;
    }

    // Programs.
    const int diag = d.diagMode;
    const DiagTest thisTest = DiagTestAt(diag ? diag - 1 : 0);
    const DiagTest *test = diag ? &thisTest : nullptr;
    uint32_t strides[kMaxStreams];
    for (uint32_t s = 0; s < kMaxStreams; ++s)
        strides[s] = device.streams[s].stride;
    if (upVertices)
        strides[0] = upStride;
    gxm::cg::Options pixelOptions;
    if (device.renderStates[d3d::RS_ALPHATESTENABLE])
    {
        const uint32_t func = device.renderStates[d3d::RS_ALPHAFUNC];
        if (func >= d3d::CMP_NEVER && func < d3d::CMP_ALWAYS)
            pixelOptions.alphaFunc = gxm::cg::AlphaFunc(func);
    }
    // The programs this combination last drew with (Pipeline), unless a
    // run diagnostic wants to see or change how they are found.
    const bool plain = d.reuse && !test && !d.diagnosedDraws;
    PipelineKey pipelineKey;
    Pipeline *pipeline = nullptr;
    bool known = false;
    if (plain)
    {
        std::memset(&pipelineKey, 0, sizeof(pipelineKey));
        pipelineKey.vs = vs;
        pipelineKey.ps = ps;
        pipelineKey.decl = device.decl;
        std::memcpy(pipelineKey.strides, strides, sizeof(strides));
        pipelineKey.alphaFunc = uint8_t(pixelOptions.alphaFunc);
        pipelineKey.index32 = index32;
        pipeline = &PipelineSlot(d, pipelineKey);
        known = std::memcmp(&pipeline->key, &pipelineKey, sizeof(pipelineKey)) == 0;
        ++d.pipelineLookups;
        d.pipelineHits += known;
    }
    InputPlan plan;
    if (!known)
        PlanInputs(device, *vs, *device.decl, plan);
    // The shader whose program this draw runs: the game's, or in some tests
    // one written here for draws with the menu's vertex shader.
    Shader *vsUsed = vs;
    Variant *vertex = known ? pipeline->vertex : nullptr;
    if (test)
    {
        plan.options.diagPosition = test->option;
        if (test->synthetic && MenuShaped(*vs, plan))
        {
            if (test->synthetic == 2)
                vertex = d.diagPlain.failed ? nullptr : &d.diagPlain;
            else if (test->synthetic == 5)
                vertex = test->edit < 0 || d.diagEdited[test->edit].failed ? nullptr : &d.diagEdited[test->edit];
            else if (Shader &synthetic = d.diagSynthetic[test->synthetic == 1 ? 0 : test->synthetic - 2];
                     synthetic.translated)
                vsUsed = &synthetic;
        }
    }
    // Both programs are asked for before either is waited on.
    bool waiting = false;
    if (!vertex)
        vertex = GetVariant(device, *vsUsed, plan.options, waiting);
    Variant *pixel = known ? pipeline->pixel : GetVariant(device, *ps, pixelOptions, waiting);
    if (!vertex || !pixel)
    {
        if (waiting)
        {
            // Not a failure: the draw comes back once its program is made.
            ++device.shaderCounts.drawsWaited;
            device.shaderCounts.waitedThisFrame = true;
        }
        else
            CountSkip(device, SkipNoProgram);
        return;
    }
    bool diagPixel = false;
    if (test)
    {
        bool texcoord = false;
        for (const gxm::cg::Varying &varying : vsUsed->parsed.reflection.varyings)
            texcoord |= varying.semantic == gxm::cg::Semantic::Texcoord && varying.index == 0;
        if (test->probe && texcoord && !d.diagProbe.failed)
            pixel = &d.diagProbe;
        else if (!d.diagGreen.failed)
            pixel = &d.diagGreen;
        diagPixel = pixel == &d.diagProbe || pixel == &d.diagGreen;
    }
    // A stand-in vertex program for this draw.
    Variant *standIn = nullptr;
    const SceGxmProgramParameter *standInPosition = nullptr;
    std::map<uint32_t, SceGxmVertexProgram *> *standInPrograms = nullptr;
    if (test && test->standIn == 1 && !d.diagPass.failed)
    {
        standIn = &d.diagPass;
        standInPosition = d.diagPassPosition;
        standInPrograms = &d.diagPassPrograms;
    }
    else if (test && test->standIn == 2 && !d.diagMimic.failed)
    {
        standIn = &d.diagMimic;
        standInPosition = d.diagMimicPosition;
        standInPrograms = &d.diagMimicPrograms;
    }
    const bool passThrough = standIn != nullptr;
    if (!OpenScene(device))
    {
        CountSkip(device, SkipNoScene);
        return;
    }
    if (!d.hasColor)
    {
        if (const Texture *wanted = SceneTexture(d); wanted && wanted->deferredBytes)
            CountSkip(device, SkipNoTargetMemory);
        else
            NotYet(device, "drawing with no colour target (depth only; skipped)");
        return;
    }
    const VertexProgramEntry *vertexProgram =
        known ? pipeline->vertexProgram : GetVertexProgram(device, *vertex, *device.decl, plan, index32, strides);
    if (!vertexProgram)
        return;
    SceGxmBlendInfo blend{};
    const bool blended = !diagPixel && MakeBlend(device, d.color.noAlpha, blend);
    uint32_t blendBits = 0;
    if (blended)
        std::memcpy(&blendBits, &blend, sizeof(blendBits));
    const FragmentProgramEntry *fragmentProgram =
        known && pipeline->blended == blended && pipeline->blendBits == blendBits &&
                pipeline->output == uint32_t(d.color.output)
            ? pipeline->fragmentProgram
            : GetFragmentProgram(device, *pixel, passThrough ? *standIn : *vertex, blended ? &blend : nullptr);
    if (!fragmentProgram)
        return;
    if (plain)
    {
        pipeline->key = pipelineKey;
        pipeline->vertex = vertex;
        pipeline->pixel = pixel;
        pipeline->vertexProgram = vertexProgram;
        pipeline->fragmentProgram = fragmentProgram;
        pipeline->blendBits = blendBits;
        pipeline->output = uint32_t(d.color.output);
        pipeline->blended = blended;
    }

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
        vb->drawnWait = d.gpuWaits;
        vb->drawnStamp = Stamp(d);
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

    // Run-diagnostic tests: a stand-in vertex program fed the game's
    // position stream.
    const SceGxmVertexProgram *vertexGxm = vertexProgram->program;
    const void *drawIndices = indexData;
    bool drawIndex32 = index32;
    uint8_t drawStreams = vertexProgram->streamCount;
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
            SceGxmVertexProgram *&program = (*standInPrograms)[key];
            if (!program)
            {
                SceGxmVertexAttribute attribute{};
                attribute.streamIndex = 0;
                attribute.offset = position->offset;
                attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                attribute.componentCount = components;
                attribute.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(standInPosition));
                SceGxmVertexStream stream{uint16_t(stride), drawIndex32 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                                                                        : SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
                const int made = sceGxmShaderPatcherCreateVertexProgram(gxm::ShaderPatcher(), standIn->id,
                                                                        &attribute, 1, &stream, 1, &program);
                if (made < 0)
                {
                    Log(device, "diag: a stand-in vertex program could not be made (0x%08x)", unsigned(made));
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
    // The buffers the last draw reserved still hold this draw's constants
    // when the programs and the constants are the last draw's.
    const bool ownProgram = vertexGxm == vertexProgram->program;
    const bool vertexKept = plain && ownProgram && d.vertexUniformsLive &&
                            d.vertexUniformsVersion == device.vsConstantsVersion;
    d.uniformWrites += vertex->uniforms && ownProgram;
    d.uniformWritesLeftOut += vertex->uniforms && vertexKept;
    if (vertex->uniforms && ownProgram && !vertexKept)
    {
        void *uniforms = nullptr;
        const int reserved = sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        // As many as the program declares (a test's shader may declare fewer than the game's).
        const uint32_t count = vertex->constants
                                   ? std::min<uint32_t>(256u, sceGxmProgramParameterGetArraySize(vertex->constants))
                                   : 0u;
        int set = 0;
        if (vertex->constants && count && reserved >= 0)
            set = sceGxmSetUniformDataF(uniforms, vertex->constants, 0, count * 4u, device.vsConstants.data());
        if ((reserved < 0 || set < 0) && d.uniformFailures++ < 5)
            Log(device, "vertex constants not set: reserve 0x%08x, set 0x%08x (%u constants)", unsigned(reserved),
                unsigned(set), unsigned(count));
        d.vertexUniformsLive = reserved >= 0 && set >= 0;
        d.vertexUniformsVersion = device.vsConstantsVersion;
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
    if (d.diagnosedDraws)
    {
        // What the vertex program takes, and what this draw feeds it.
        std::string line;
        char part[160];
        Log(device, "diag frame %llu draw: vertex program (cache file %016llx%s) %s", (unsigned long long)d.frame,
            (unsigned long long)vertex->cacheKey,
            vertex == &d.diagPlain ? ", the plain guess"
            : vsUsed != vs || (test && test->synthetic == 5 && test->edit >= 0 && vertex == &d.diagEdited[test->edit])
                ? ", a synthetic shader"
                : "",
            DescribeProgram(vertex->program()).c_str());
        line.clear();
        for (int reg = 0; reg < 16; ++reg)
        {
            const VertexElement *element = plan.element[reg];
            if (!element && !vertex->inputs[reg])
                continue;
            std::snprintf(part, sizeof(part), " v%d(%s, %s", reg, vertex->inputs[reg] ? "in program" : "NOT in program",
                          element ? "" : "no element)");
            line += part;
            if (element)
            {
                std::snprintf(part, sizeof(part), "stream %u offset %u type %u -> format 0x%x x%u)",
                              unsigned(element->stream), unsigned(element->offset), unsigned(element->type),
                              unsigned(plan.format[reg]), unsigned(plan.components[reg]));
                line += part;
            }
        }
        Log(device, "diag frame %llu draw: %u streams (stride %u), absent 0x%x bgra 0x%x, inputs:%s",
            (unsigned long long)d.frame, unsigned(vertexProgram->streamCount),
            vertexProgram->streamCount ? unsigned(strides[vertexProgram->streams[0]]) : 0u,
            unsigned(plan.options.absentInputs), unsigned(plan.options.bgraInputs), line.c_str());
    }
    if (standIn == &d.diagMimic && vertexGxm != vertexProgram->program)
    {
        void *uniforms = nullptr;
        const int reserved = sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        const int set = reserved >= 0
                            ? sceGxmSetUniformDataF(uniforms, d.diagMimic.constants, 0, 16, device.vsConstants.data())
                            : 0;
        if ((reserved < 0 || set < 0) && d.uniformFailures++ < 5)
            Log(device, "diag: mimic constants not set: reserve 0x%08x, set 0x%08x", unsigned(reserved), unsigned(set));
    }
    void *pixelUniforms = nullptr;
    // Not kept for a program that takes a texture's depth as a uniform
    // (BindTextures writes those).
    const uint32_t alphaRef = device.renderStates[d3d::RS_ALPHAREF] & 0xff;
    const bool pixelKept = plain && d.fragmentUniformsLive && !pixel->depthUniforms &&
                           d.fragmentUniformsVersion == device.psConstantsVersion &&
                           (!pixel->alphaRef || d.fragmentAlphaRef == alphaRef);
    d.uniformWrites += pixel->uniforms;
    d.uniformWritesLeftOut += pixel->uniforms && pixelKept;
    if (pixel->uniforms && !pixelKept)
    {
        const int reserved = sceGxmReserveFragmentDefaultUniformBuffer(context, &pixelUniforms);
        const uint16_t count = ps->parsed.reflection.floatConstants;
        if (pixel->constants && count)
            sceGxmSetUniformDataF(pixelUniforms, pixel->constants, 0, count * 4u, device.psConstants.data());
        if (pixel->alphaRef)
        {
            const float ref = float(alphaRef) / 255.0f;
            sceGxmSetUniformDataF(pixelUniforms, pixel->alphaRef, 0, 1, &ref);
        }
        d.fragmentUniformsLive = reserved >= 0 && pixelUniforms != nullptr;
        d.fragmentUniformsVersion = device.psConstantsVersion;
        d.fragmentAlphaRef = alphaRef;
    }
    if (d.census)
        LogCensus(device, *vs, *ps, *vertex, *pixel, blended, blend);
    Texture *const target = SceneTexture(d);
    if (!BindTextures(device, *pixel, pixelUniforms))
    {
        CountSkip(device, SkipUnfilledSource);
        if (target)
            target->unfilled = true;
        return;
    }
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
    ++d.sceneDraws;
    if (target)
        target->unfilled = false;
    if (d.drawTimes)
        TimeDraw(device, *vertex, *pixel, indexCount);
}

void Copy(Device &device, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear)
{
    DrawState &d = *device.draw;
    if (!src || !dst || src == dst)
        return;
    // A copy that cannot happen leaves its destination unfilled.
    Texture *const dstTexture = dst->kind == SurfaceKind::TextureLevel ? dst->texture : nullptr;
    const auto skip = [&] {
        CountSkip(device, SkipCopy);
        if (dstTexture)
            dstTexture->unfilled = true;
    };
    // The source as a texture: the back buffer, or one level of a texture.
    SceGxmTexture view;
    uint32_t width = 0, height = 0;
    int result = -1;
    if (src->kind == SurfaceKind::TextureLevel && src->texture)
    {
        EnsureTargetMemory(device, *src->texture);
        if (Unfilled(*src->texture))
        {
            skip();
            return;
        }
    }
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
        skip();
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
    {
        skip();
        return;
    }
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
    d.unitBound = 0; // a unit's texture changed behind BindTextures
    sceGxmSetVertexStream(context, 0, quad);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, d.sequential, 4);
    ++d.copies;
    if (dstTexture)
        dstTexture->unfilled = false;
}

void DiagnoseFrame(Device &device)
{
    DrawState *d = device.draw;
    if (!d)
        return;
    // The frame being drawn is d->frame (FrameDone counts it after Present).
    if (d->diagTests && d->frame >= kTestRectFirst && d->frame <= kTestRectLast)
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
    // Only when asked for: reading a display buffer makes Vita3K slow for
    // the rest of the run (above).
    if (!d->diagReadBack)
        return;
    uint64_t into = 0;
    bool readBack = d->diagTests && DiagModeFor(d->frame, &into) && into == kTestRead;
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
        "corner %08x; grid x 120 480 840 at y 68: %08x %08x %08x, y 272: %08x %08x %08x, y 476: %08x %08x %08x",
        (unsigned long long)d->frame, d->diagMode, lit, total, litTopLeft, magenta, green, sum[0] / total, sum[1] / total, sum[2] / total,
        at(480, 272), at(240, 136), at(720, 136), at(240, 408), at(720, 408), at(2, 2), at(120, 68), at(480, 68),
        at(840, 68), at(120, 272), at(480, 272), at(840, 272), at(120, 476), at(480, 476), at(840, 476));
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
    // What the draws did not do again (Pipeline, BindTextures, the uniform
    // buffers in Draw).
    Log(device,
        "draw work left out: %llu of %llu draws found their programs in the table; of %llu texture binds %llu were "
        "set already and %llu set their filters anew; of %llu uniform buffers %llu were the last draw's",
        (unsigned long long)d->pipelineHits, (unsigned long long)d->pipelineLookups,
        (unsigned long long)d->textureBinds, (unsigned long long)d->textureBindsLeftOut,
        (unsigned long long)d->textureViewsMade, (unsigned long long)d->uniformWrites,
        (unsigned long long)d->uniformWritesLeftOut);
    for (int i = 0; i < SkipCount; ++i)
        if (d->skipped[i])
            Log(device, "draws skipped: %llu x %s", (unsigned long long)d->skipped[i], kSkipNames[i]);
    Log(device,
        "buffer discards: %llu wrote in place (the GPU was done with the buffer), %llu took a kept piece of memory, "
        "%llu made a new one and %llu waited for the whole GPU (none free and no room); %llu texture rewrites took "
        "the second piece; kept pieces and second pieces hold %u KB",
        (unsigned long long)d->renamesSpared, (unsigned long long)d->renameSwaps, (unsigned long long)d->renames,
        (unsigned long long)d->renameWaits, (unsigned long long)d->textureSwaps, d->copyBytes >> 10);
    Log(device,
        "frames overlap: %s; %llu of %llu frames waited for the frame before last, %.1f ms each on average; %u "
        "notifications did not come",
        d->overlap ? "yes (the CPU fills a frame while the GPU draws the one before)" : "no",
        (unsigned long long)d->paceWaits, (unsigned long long)d->frame,
        d->paceWaits ? d->paceMicros / 1000.0 / double(d->paceWaits) : 0.0, d->paceTimeouts);
}
} // namespace gfx_vk::legacy
