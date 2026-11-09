#pragma once

// The GXM Legacy back end's own types, shared by its files:
//   gxm_legacy.cpp       the device, resources, state, copies, frames;
//   gxm_legacy_draw.cpp  scenes, programs, vertex streams, textures as
//                        sceGxm sees them, and the draws.
// See gxm_legacy.cpp for what the back end does.

#if !defined(KISAK_GFX_GXM)
#error "gxm_legacy_internal.h is the PS Vita's Legacy back end (KISAK_GFX_GXM)"
#endif

#include <gfx_vk/legacy/vk_legacy.h>

#include <gfx_gxm/gxm_display.h>
#include <gfx_gxm/gxm_memory.h>
#include <gfx_gxm/shader/gxm_cg_translate.h>

#include <psp2/gxm.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gfx_vk::legacy
{
namespace gxm = gfx_gxm;

// ---- Formats -----------------------------------------------------------------------------

struct FormatInfo
{
    d3d::Format d3d;
    uint8_t blockBytes; // per texel, or per 4x4 block
    bool compressed;
    bool depth;
    bool renderTarget; // a colour format sceGxm can render to
};

const FormatInfo *LookupFormat(d3d::Format format);

inline uint32_t LevelSize(uint32_t size, uint32_t level)
{
    return std::max(1u, size >> level);
}

inline uint32_t FullChain(uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t size = std::max({width, height, depth});
    uint32_t levels = 1;
    while (size > 1)
    {
        size >>= 1;
        ++levels;
    }
    return levels;
}

inline uint32_t AlignUp(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

// Rows of a level as LockRect sees them: texel rows, or rows of 4x4 blocks.
inline uint32_t Rows(const FormatInfo &format, uint32_t height)
{
    return format.compressed ? std::max(1u, (height + 3) / 4) : height;
}

// Bytes per row. Colour formats sceGxm may render to keep rows a multiple
// of 32 texels, which colour surfaces want; the rest are packed.
inline uint32_t Pitch(const FormatInfo &format, uint32_t width, bool renderTarget)
{
    if (format.compressed)
        return std::max(1u, (width + 3) / 4) * format.blockBytes;
    if (renderTarget)
        return AlignUp(width, 32) * format.blockBytes;
    return width * format.blockBytes;
}

// ---- Objects -----------------------------------------------------------------------------

struct Level
{
    uint32_t offset = 0;
    uint32_t pitch = 0;
    uint32_t slicePitch = 0;
    uint32_t width = 0, height = 0, depth = 1;
    bool locked = false;
};

// Where a resource's bytes are: a GPU pool, or the C heap for SYSTEMMEM and
// SCRATCH resources (which the GPU never reads).
struct Memory
{
    uint8_t *data = nullptr;
    gxm::MemoryKind kind = gxm::MemoryKind::Cdram;
    bool gpu = false;
    std::vector<uint8_t> host;
};

struct Texture
{
    TextureDesc desc{};
    const FormatInfo *format = nullptr;
    uint32_t faces = 1;
    std::vector<Level> levels; // face * levels + level
    Memory memory;
    std::vector<std::unique_ptr<Surface>> surfaces; // GetSurfaceLevel views, same index
    std::string name;
    // How sceGxm samples it (filters and addressing are set per draw);
    // sampleable false: draws bind a stand-in (cube and depth textures).
    SceGxmTexture sampled;
    bool sampleable = false;
    // Depth formats: the depth-stencil surface scenes use.
    SceGxmDepthStencilSurface depthSurface;
};

enum class SurfaceKind : uint8_t
{
    TextureLevel, // a level of a texture (standalone surfaces own theirs)
    BackBuffer,   // whichever display buffer the frame draws into
    FrontBuffer,  // the one shown last
};

struct Surface
{
    SurfaceKind kind = SurfaceKind::TextureLevel;
    Texture *texture = nullptr;
    uint32_t face = 0;
    uint32_t level = 0;
    bool standalone = false;
};

struct GpuBuffer
{
    uint32_t size = 0;
    uint32_t usage = 0;
    d3d::Pool pool = d3d::POOL_DEFAULT;
    d3d::Format indexFormat = d3d::FMT_UNKNOWN;
    Memory memory;
    bool locked = false;
    bool drawn = false; // read by a draw since its memory was last renamed
    std::string name;
};
struct VertexBuffer : GpuBuffer
{
};
struct IndexBuffer : GpuBuffer
{
};

// One compiled program of a shader for one set of translator options,
// registered with the shader patcher on first use. Draws pick the variant
// their state needs (gxm_legacy_draw.cpp).
struct Variant
{
    uint64_t key = 0; // the options, packed (VariantKey)
    std::vector<uint8_t> gxp; // never resized once registered
    SceGxmShaderPatcherId id = nullptr;
    bool failed = false; // did not compile or register; draws with it skip
    // What the draws write, found once by name.
    const SceGxmProgramParameter *constants = nullptr; // "c"
    const SceGxmProgramParameter *alphaRef = nullptr;  // "kisak_alpha_ref"
    const SceGxmProgramParameter *depth[16] = {};      // "kisak_depth_s<n>"
    const SceGxmProgramParameter *inputs[16] = {};     // "in_v<n>"
    int8_t textureUnit[16];                            // sampler s<n>'s unit, -1 none
    bool uniforms = false; // has a default uniform buffer

    const SceGxmProgram *program() const { return reinterpret_cast<const SceGxmProgram *>(gxp.data()); }
};

struct Shader
{
    std::vector<uint8_t> bytecode;
    bool pixel = false;
    // Translated (cg::Parse) and its default variant compiled: no alpha
    // test, no D3DCOLOR inputs. Draw state picks other variants later.
    bool translated = false;
    gfx_gxm::cg::Shader parsed;
    std::vector<uint8_t> program; // the default variant; empty when it did not compile
    std::vector<std::unique_ptr<Variant>> variants;
};

struct VertexDecl
{
    std::vector<VertexElement> elements;
};

struct Query
{
    QueryKind kind = QueryKind::Event;
    bool issued = false;
};

struct DrawState; // gxm_legacy_draw.cpp

struct Device
{
    DeviceDesc desc;
    std::string error;
    DeviceStats stats{};
    uint32_t width = gxm::kDisplayWidth, height = gxm::kDisplayHeight;
    Surface backBuffer, frontBuffer;
    bool detached = false;

    uint32_t renderStates[d3d::RS_COUNT] = {};
    uint32_t samplerStates[20][d3d::SAMP_COUNT] = {};
    Texture *textures[20] = {};
    Shader *vertexShader = nullptr;
    Shader *pixelShader = nullptr;
    VertexDecl *decl = nullptr;
    struct Stream
    {
        VertexBuffer *buffer = nullptr;
        uint32_t offset = 0, stride = 0;
    } streams[kMaxStreams];
    IndexBuffer *indices = nullptr;
    Surface *renderTargets[kMaxRenderTargets] = {};
    Surface *depthStencil = nullptr;
    Viewport viewport{};
    Rect scissor{};
    std::vector<float> vsConstants, psConstants;

    // Things the back end does not do yet, each logged once.
    std::vector<const char *> notYet;
    bool fillFailureLogged = false;
    // Shader translation and compilation, for the summary lines.
    struct ShaderCounts
    {
        uint32_t created = 0, refused = 0, compiled = 0, failed = 0;
        uint32_t loggedFailures = 0;
        uint64_t translateMicros = 0;
        // Each reason the translator gave, with how many shaders it refused.
        std::vector<std::pair<std::string, uint32_t>> refusals;
    } shaderCounts;
    uint32_t live = 0; // resources not yet destroyed
    DrawState *draw = nullptr;
};

// Where a colour surface's texels are.
struct Target
{
    uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, pitch = 0;
    const FormatInfo *format = nullptr;
    bool display = false;
};

// ---- Shared helpers (gxm_legacy.cpp) ------------------------------------------------------

void Log(Device &device, const char *format, ...) __attribute__((format(printf, 2, 3)));
// Logs what is not done yet, once per distinct text (a string literal).
void NotYet(Device &device, const char *what);
uint64_t Micros();
// GPU memory: the preferred pool, then the other one. SYSTEMMEM and SCRATCH
// resources stay in the C heap.
bool AllocateMemory(Device &device, Memory &memory, uint32_t bytes, uint32_t align, d3d::Pool pool, bool preferCdram,
                    const char *name);
void FreeMemory(Memory &memory);
// The display's raw texel for a D3DCOLOR: the display buffers are A8B8G8R8.
uint32_t DisplayColor(uint32_t color);
bool Describe(const Surface *surface, Target &out);

// ---- Drawing (gxm_legacy_draw.cpp) --------------------------------------------------------

bool InitDraw(Device &device, std::string &error);
void ShutdownDraw(Device &device); // after the GPU is idle
// Lays out a new texture's levels as sceGxm samples them and returns the
// bytes it needs; SetUpTexture then makes its sampled view or depth surface
// once the memory is there.
uint32_t LayoutTexture(Texture &texture);
void SetUpTexture(Device &device, Texture &texture);
// GPU memory the GPU may still read: freed once the frames using it are done.
void Retire(Device &device, Memory &memory);
// The open scene, if any, goes to the GPU; Finish also waits for it.
void EndScene(Device &device);
void FinishGpu(Device &device);
// After each Present: frees what the GPU is done with.
void FrameDone(Device &device);
// Drops programs made from a shader or declaration about to go.
void ForgetShader(Device &device, Shader &shader);
void ForgetDecl(Device &device, VertexDecl &decl);
// DISCARD on a buffer a draw read: fresh memory, the old retired.
void Rename(Device &device, GpuBuffer &buffer);
void ClearTargets(Device &device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
                  uint32_t stencil);
void Draw(Device &device, d3d::PrimitiveType type, int32_t baseVertex, uint32_t startIndex, uint32_t primitiveCount,
          bool indexed, const void *upVertices, uint32_t upStride);
void LogDrawSummary(Device &device);
} // namespace gfx_vk::legacy
