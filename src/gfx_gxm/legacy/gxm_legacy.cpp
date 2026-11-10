// The Legacy adapter's API (src/gfx_vk/legacy/vk_legacy.h) on the PS Vita's
// sceGxm, for the D3D9 front (src/gfx_vk/d3d9) in the Vita client.
//
// Stage 2, step 2d (docs/vita-client.md): drawing.
// - The device owns the screen (gxm_display.h) and the GPU memory pools.
// - Resources live in GPU-mapped memory with D3D9's lock semantics; DISCARD
//   renames a buffer a draw read.
// - Shaders are translated to Cg (shader/gxm_cg_translate.h) and compiled,
//   through the program cache (gxm_program_cache.h).
// - Clears, draws and StretchRect happen in sceGxm scenes
//   (gxm_legacy_draw.cpp).
// - Present ends the scene and shows the back buffer.
//
// Texture levels are stored as sceGxm reads a linear texture, which is also
// a layout D3D9's LockRect can hand out: rows of whole multiples of 8 texels
// (blocks of 4x4 for DXT). LockRect hands out the GPU memory itself.
//
// One thread at a time calls everything; the front's lock sees to that.

#if !defined(KISAK_GFX_GXM)
#error "gxm_legacy.cpp is the PS Vita's Legacy back end (KISAK_GFX_GXM)"
#endif

#include "gxm_legacy_internal.h"

#include <gfx_vk/legacy/vk_legacy.h>

#include <gfx_gxm/gxm_display.h>
#include <gfx_gxm/gxm_heap.h>
#include <gfx_gxm/gxm_memory.h>
#include <gfx_gxm/gxm_program_cache.h>
#include <gfx_gxm/shader/gxm_cg_translate.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace gfx_vk::legacy
{
// ---- Formats -----------------------------------------------------------------------------

namespace
{
const FormatInfo kFormats[] = {
    {d3d::FMT_A8R8G8B8, 4, false, false, true},
    {d3d::FMT_X8R8G8B8, 4, false, false, true},
    {d3d::FMT_A8B8G8R8, 4, false, false, true},
    {d3d::FMT_X8B8G8R8, 4, false, false, true},
    {d3d::FMT_R5G6B5, 2, false, false, true},
    {d3d::FMT_A1R5G5B5, 2, false, false, true},
    {d3d::FMT_X1R5G5B5, 2, false, false, true},
    {d3d::FMT_A4R4G4B4, 2, false, false, true},
    {d3d::FMT_A8, 1, false, false, false},
    {d3d::FMT_L8, 1, false, false, false},
    {d3d::FMT_A8L8, 2, false, false, false},
    {d3d::FMT_V8U8, 2, false, false, false},
    {d3d::FMT_G16R16, 4, false, false, false},
    {d3d::FMT_A16B16G16R16, 8, false, false, false},
    {d3d::FMT_R16F, 2, false, false, true},
    {d3d::FMT_G16R16F, 4, false, false, true},
    {d3d::FMT_A16B16G16R16F, 8, false, false, true},
    {d3d::FMT_R32F, 4, false, false, true},
    {d3d::FMT_G32R32F, 8, false, false, false},
    {d3d::FMT_A32B32G32R32F, 16, false, false, false},
    {d3d::FMT_DXT1, 8, true, false, false},
    {d3d::FMT_DXT2, 16, true, false, false},
    {d3d::FMT_DXT3, 16, true, false, false},
    {d3d::FMT_DXT4, 16, true, false, false},
    {d3d::FMT_DXT5, 16, true, false, false},
    // Depth formats all become sceGxm's packed S8D24.
    {d3d::FMT_D16, 4, false, true, false},
    {d3d::FMT_D24S8, 4, false, true, false},
    {d3d::FMT_D24X8, 4, false, true, false},
    {d3d::FMT_D24FS8, 4, false, true, false},
};

} // namespace

const FormatInfo *LookupFormat(d3d::Format format)
{
    for (const FormatInfo &info : kFormats)
        if (info.d3d == format)
            return &info;
    return nullptr;
}

void Log(Device &device, const char *format, ...)
{
    if (!device.desc.log)
        return;
    char line[1024];
    int prefix = std::snprintf(line, sizeof(line), "gxm legacy: ");
    va_list args;
    va_start(args, format);
    std::vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    device.desc.log(device.desc.logUser, line);
}

void NotYet(Device &device, const char *what)
{
    if (std::find(device.notYet.begin(), device.notYet.end(), what) != device.notYet.end())
        return;
    device.notYet.push_back(what);
    Log(device, "%s: not done by the GXM back end yet (logged once)", what);
}

uint64_t Micros()
{
    return sceKernelGetProcessTimeWide();
}

namespace
{
void LogShaderSummary(Device &device, const char *when); // with CreateShader
} // namespace

// GPU memory in use by what it is for, in each of the two pools it can be in.
namespace
{
std::atomic<uint32_t> g_useBytes[2][static_cast<int>(MemoryUse::Count)];
std::atomic<uint32_t> g_useCount[2][static_cast<int>(MemoryUse::Count)];
// Pieces this large are logged one by one as they are made, up to a count.
constexpr uint32_t kLoggedPieceBytes = 1u << 20;
uint32_t g_loggedPieces = 0;
constexpr uint32_t kLoggedPieces = 300;

const char *MemoryUseName(MemoryUse use)
{
    switch (use)
    {
    case MemoryUse::TextureCompressed:
        return "compressed textures";
    case MemoryUse::Texture:
        return "other textures";
    case MemoryUse::Cube:
        return "cubes";
    case MemoryUse::Volume:
        return "volumes";
    case MemoryUse::RenderTarget:
        return "render targets";
    case MemoryUse::DepthStencil:
        return "depth";
    case MemoryUse::VertexStatic:
        return "static vertices";
    case MemoryUse::VertexDynamic:
        return "dynamic vertices";
    case MemoryUse::IndexStatic:
        return "static indices";
    case MemoryUse::IndexDynamic:
        return "dynamic indices";
    default:
        return "?";
    }
}

void Account(gxm::MemoryKind kind, MemoryUse use, uint32_t bytes, bool taken)
{
    const int pool = kind == gxm::MemoryKind::Cdram ? 0 : 1;
    const int index = static_cast<int>(use);
    if (taken)
    {
        g_useBytes[pool][index] += bytes;
        ++g_useCount[pool][index];
    }
    else
    {
        g_useBytes[pool][index] -= bytes;
        --g_useCount[pool][index];
    }
}
} // namespace

MemoryUse TextureUse(const TextureDesc &desc, const FormatInfo &format)
{
    if (desc.usage & d3d::USAGE_DEPTHSTENCIL)
        return MemoryUse::DepthStencil;
    if (desc.usage & d3d::USAGE_RENDERTARGET)
        return MemoryUse::RenderTarget;
    if (desc.kind == TextureKind::Cube)
        return MemoryUse::Cube;
    if (desc.kind == TextureKind::Volume)
        return MemoryUse::Volume;
    return format.compressed ? MemoryUse::TextureCompressed : MemoryUse::Texture;
}

std::string DescribeMemoryUse()
{
    std::string out = "gpu memory by use (cdram + main MB, pieces):";
    for (int i = 0; i < static_cast<int>(MemoryUse::Count); ++i)
    {
        const uint32_t cdram = g_useBytes[0][i], main = g_useBytes[1][i];
        const uint32_t pieces = g_useCount[0][i] + g_useCount[1][i];
        if (!pieces)
            continue;
        char part[96];
        std::snprintf(part, sizeof(part), " %s %.1f + %.1f (%u);", MemoryUseName(static_cast<MemoryUse>(i)),
                      cdram / 1048576.0, main / 1048576.0, pieces);
        out += part;
    }
    return out;
}

// GPU memory: the preferred pool, then the other one. SYSTEMMEM and SCRATCH
// resources stay in the C heap.
bool AllocateMemory(Device &device, Memory &memory, uint32_t bytes, uint32_t align, d3d::Pool pool, bool preferCdram,
                    MemoryUse use, const char *name)
{
    if (pool == d3d::POOL_SYSTEMMEM || pool == d3d::POOL_SCRATCH)
    {
        memory.host.assign(bytes, 0);
        memory.data = memory.host.data();
        memory.gpu = false;
        return true;
    }
    const gxm::MemoryKind first = preferCdram ? gxm::MemoryKind::Cdram : gxm::MemoryKind::Main;
    const gxm::MemoryKind second = preferCdram ? gxm::MemoryKind::Main : gxm::MemoryKind::Cdram;
    for (const gxm::MemoryKind kind : {first, second})
    {
        if (void *data = gxm::Allocate(kind, bytes, align))
        {
            memory.data = static_cast<uint8_t *>(data);
            memory.kind = kind;
            memory.gpu = true;
            memory.use = use;
            memory.bytes = bytes;
            Account(kind, use, bytes, true);
            return true;
        }
    }
    Log(device, "out of GPU memory for %s (%s, %u KB); %s", name && *name ? name : "a resource", MemoryUseName(use),
        bytes >> 10, gxm::DescribeMemory().c_str());
    Log(device, "%s", DescribeMemoryUse().c_str());
    return false;
}

bool AllocateRenamed(Memory &memory, uint32_t bytes, uint32_t align, gxm::MemoryKind kind, MemoryUse use)
{
    void *data = gxm::Allocate(kind, bytes, align);
    if (!data)
        return false;
    memory.data = static_cast<uint8_t *>(data);
    memory.kind = kind;
    memory.gpu = true;
    memory.use = use;
    memory.bytes = bytes;
    Account(kind, use, bytes, true);
    return true;
}

void FreeGpuMemory(gxm::MemoryKind kind, void *data, MemoryUse use, uint32_t bytes)
{
    if (gxm::Free(kind, data))
        Account(kind, use, bytes, false);
}

void FreeMemory(Memory &memory)
{
    if (memory.gpu)
        FreeGpuMemory(memory.kind, memory.data, memory.use, memory.bytes);
    memory = Memory{};
}

// One line for a large piece of GPU memory as it is made.
void LogLargePiece(Device &device, const Memory &memory, const char *what)
{
    if (!memory.gpu || memory.bytes < kLoggedPieceBytes || g_loggedPieces >= kLoggedPieces)
        return;
    ++g_loggedPieces;
    Log(device, "gpu memory piece %u: %.1f MB of %s for %s, %s", g_loggedPieces, memory.bytes / 1048576.0,
        gxm::MemoryKindName(memory.kind), MemoryUseName(memory.use), what);
}

// The display's raw texel for a D3DCOLOR: the display buffers are A8B8G8R8.
uint32_t DisplayColor(uint32_t color)
{
    return (color & 0xff00ff00u) | ((color >> 16) & 0xffu) | ((color & 0xffu) << 16);
}

bool Describe(const Surface *surface, Target &out)
{
    out = Target{};
    if (!surface)
        return false;
    if (surface->kind == SurfaceKind::BackBuffer || surface->kind == SurfaceKind::FrontBuffer)
    {
        out.data = static_cast<uint8_t *>(surface->kind == SurfaceKind::BackBuffer ? gxm::BackBufferData()
                                                                                    : gxm::FrontBufferData());
        out.width = gxm::kDisplayWidth;
        out.height = gxm::kDisplayHeight;
        out.pitch = gxm::kDisplayStride * 4;
        out.format = LookupFormat(d3d::FMT_A8B8G8R8);
        out.display = true;
        return out.data != nullptr;
    }
    const Texture *texture = surface->texture;
    // Swizzled textures have no level in D3D9's layout to point at.
    if (!texture || !texture->memory.data)
        return false;
    const Level &level = texture->levels[surface->face * texture->desc.levels + surface->level];
    out.data = texture->memory.data + level.offset;
    out.width = level.width;
    out.height = level.height;
    out.pitch = level.pitch;
    out.format = texture->format;
    return true;
}

namespace
{
void SetDefaultState(Device &device)
{
    std::fill(std::begin(device.renderStates), std::end(device.renderStates), 0u);
    uint32_t *rs = device.renderStates;
    rs[d3d::RS_ZENABLE] = 1;
    rs[d3d::RS_FILLMODE] = d3d::FILL_SOLID;
    rs[d3d::RS_ZWRITEENABLE] = 1;
    rs[d3d::RS_SRCBLEND] = d3d::BLEND_ONE;
    rs[d3d::RS_DESTBLEND] = d3d::BLEND_ZERO;
    rs[d3d::RS_CULLMODE] = d3d::CULL_CCW;
    rs[d3d::RS_ZFUNC] = d3d::CMP_LESSEQUAL;
    rs[d3d::RS_ALPHAFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_STENCILFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILPASS] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_STENCILMASK] = 0xffffffffu;
    rs[d3d::RS_STENCILWRITEMASK] = 0xffffffffu;
    rs[d3d::RS_TEXTUREFACTOR] = 0xffffffffu;
    rs[d3d::RS_MULTISAMPLEANTIALIAS] = 1;
    rs[d3d::RS_COLORWRITEENABLE] = 0xf;
    rs[d3d::RS_COLORWRITEENABLE1] = 0xf;
    rs[d3d::RS_COLORWRITEENABLE2] = 0xf;
    rs[d3d::RS_COLORWRITEENABLE3] = 0xf;
    rs[d3d::RS_BLENDOP] = d3d::BLENDOP_ADD;
    rs[d3d::RS_CCW_STENCILFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILPASS] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_BLENDFACTOR] = 0xffffffffu;
    rs[d3d::RS_SRCBLENDALPHA] = d3d::BLEND_ONE;
    rs[d3d::RS_DESTBLENDALPHA] = d3d::BLEND_ZERO;
    rs[d3d::RS_BLENDOPALPHA] = d3d::BLENDOP_ADD;
    for (auto &sampler : device.samplerStates)
    {
        std::fill(std::begin(sampler), std::end(sampler), 0u);
        sampler[d3d::SAMP_ADDRESSU] = d3d::TADDRESS_WRAP;
        sampler[d3d::SAMP_ADDRESSV] = d3d::TADDRESS_WRAP;
        sampler[d3d::SAMP_ADDRESSW] = d3d::TADDRESS_WRAP;
        sampler[d3d::SAMP_MAGFILTER] = d3d::TEXF_POINT;
        sampler[d3d::SAMP_MINFILTER] = d3d::TEXF_POINT;
        sampler[d3d::SAMP_MIPFILTER] = d3d::TEXF_NONE;
        sampler[d3d::SAMP_MAXANISOTROPY] = 1;
    }
    std::fill(std::begin(device.textures), std::end(device.textures), nullptr);
    device.vertexShader = device.pixelShader = nullptr;
    device.decl = nullptr;
    for (auto &stream : device.streams)
        stream = Device::Stream{};
    device.indices = nullptr;
    std::fill(std::begin(device.renderTargets), std::end(device.renderTargets), nullptr);
    device.renderTargets[0] = &device.backBuffer;
    device.depthStencil = nullptr;
    device.viewport = Viewport{0, 0, device.width, device.height, 0.0f, 1.0f};
    device.scissor = Rect{0, 0, int32_t(device.width), int32_t(device.height)};
}

// The GPU memory to reserve: nearly all of CDRAM for textures and targets,
// and a fixed slice of main memory for geometry, which competes with the
// engine's heap and zones.
gxm::MemorySizes PoolSizes(Device &device)
{
    SceKernelFreeMemorySizeInfo system;
    system.size = sizeof(system);
    if (sceKernelGetFreeMemorySize(&system) < 0)
        system.size_user = 0, system.size_cdram = 64 << 20;
    gxm::MemorySizes sizes;
    const uint32_t margin = 4u << 20;
    sizes.cdram = system.size_cdram > int(margin + (16u << 20))
                      ? (uint32_t(system.size_cdram) - margin) & ~((256u << 10) - 1)
                      : 16u << 20;
    // 16 MB peaked at 15.1 in the menus before drawing; draws add the
    // transient ring and renamed buffers.
    sizes.main = 20u << 20;
    // For measuring in an emulator what a map would take if it fitted: a file
    // named big-gpu-pool beside the compiled programs adds 96 MB to the main
    // pool. A console has no such memory to give (the device is then not
    // created), and Vita3K reports more free memory than a console has.
    if (gxm::ProgramCacheMarker("big-gpu-pool"))
    {
        sizes.main += 96u << 20;
        Log(device, "diag: big-gpu-pool is there: the main pool is %u MB (for emulator measurements only)",
            sizes.main >> 20);
    }
    sizes.vertexUsse = 1u << 20;
    sizes.fragmentUsse = 1u << 20;
    Log(device, "system memory free before the GPU: %d MB main, %d MB CDRAM", system.size_user >> 20,
        system.size_cdram >> 20);
    return sizes;
}

void DisplayLog(void *user, const char *line)
{
    Device *device = static_cast<Device *>(user);
    if (device->desc.log)
        device->desc.log(device->desc.logUser, line);
}
} // namespace

// ---- The target reserve ------------------------------------------------------------------
//
// The renderer makes its targets at start-up, and they take their memory when
// they are first drawn to (EnsureTargetMemory). In a map that is after the
// map's textures and vertices have filled the pools: mp_crash in Vita3K (runs
// M and U) refused the two screen-sized targets a game frame needs with 4.5 MB
// of CDRAM free, none of it 2 MB in one piece. One of them is the copy of the
// scene that the post effects read, and the film colour pass then painted the
// world black behind the HUD; the other is the depth copy (R32F) that soft
// particles and depth of field read.
//
// So a piece of CDRAM is set aside at start-up for colour targets alone, with
// room for two screen-sized ones and the four of a quarter the size (glow,
// depth of field, blur). Colour targets take from it first; what does not fit
// there comes from the pools as before.
namespace
{
struct TargetReserve
{
    uint8_t *base = nullptr;
    gxm::Heap heap;
} g_targetReserve;

constexpr uint32_t kTargetAlign = 256;
constexpr uint64_t kTargetRetryMicros = 1000 * 1000;

uint32_t TargetReserveBytes()
{
    // As LayoutTexture lays out a 32-bit colour target: rows of whole 32 texels.
    const uint32_t full = AlignUp(gxm::kDisplayWidth, 32) * 4 * gxm::kDisplayHeight;
    const uint32_t quarter = AlignUp(gxm::kDisplayWidth / 4, 32) * 4 * (gxm::kDisplayHeight / 4);
    return AlignUp(2 * full + 4 * quarter, 4096);
}

void InitTargetReserve(Device &device)
{
    // For checking in an emulator what a full pool does to a frame: a file
    // named no-target-reserve beside the compiled programs leaves the reserve
    // out.
    if (gxm::ProgramCacheMarker("no-target-reserve"))
    {
        Log(device, "diag: no-target-reserve is there: render targets take what the pools have left");
        return;
    }
    const uint32_t bytes = TargetReserveBytes();
    g_targetReserve.base = static_cast<uint8_t *>(gxm::Allocate(gxm::MemoryKind::Cdram, bytes, 4096));
    if (!g_targetReserve.base)
    {
        Log(device, "no CDRAM for the target reserve (%u KB): render targets take what the pools have left",
            bytes >> 10);
        return;
    }
    g_targetReserve.heap.Reset(bytes);
    Log(device, "target reserve: %u KB of CDRAM kept for colour render targets", bytes >> 10);
}

bool TakeFromTargetReserve(Texture &texture, uint32_t bytes, MemoryUse use)
{
    if (!g_targetReserve.base)
        return false;
    const uint32_t offset = g_targetReserve.heap.Allocate(bytes, kTargetAlign);
    if (offset == gxm::Heap::kNoSpace)
        return false;
    Memory &memory = texture.memory;
    memory.data = g_targetReserve.base + offset;
    memory.kind = gxm::MemoryKind::Cdram;
    memory.gpu = true;
    memory.use = use;
    memory.bytes = bytes;
    Account(memory.kind, use, bytes, true);
    texture.reserved = true;
    return true;
}

// A reserved target's memory goes back to the reserve. The GPU must be idle.
void ReturnToTargetReserve(Texture &texture)
{
    Memory &memory = texture.memory;
    if (g_targetReserve.heap.Free(uint32_t(memory.data - g_targetReserve.base)))
        Account(memory.kind, memory.use, memory.bytes, false);
    memory = Memory{};
    texture.reserved = false;
}

// AllocateMemory for a target's later tries, which say nothing: the first
// refusal was logged with the pools' state.
bool TakeFromPoolsQuietly(Memory &memory, uint32_t bytes, MemoryUse use)
{
    for (const gxm::MemoryKind kind : {gxm::MemoryKind::Cdram, gxm::MemoryKind::Main})
        if (void *data = gxm::Allocate(kind, bytes, kTargetAlign))
        {
            memory.data = static_cast<uint8_t *>(data);
            memory.kind = kind;
            memory.gpu = true;
            memory.use = use;
            memory.bytes = bytes;
            Account(kind, use, bytes, true);
            return true;
        }
    return false;
}
} // namespace

// ---- The device ------------------------------------------------------------------------

Device *CreateDevice(Core *core, const DeviceDesc &desc, std::string &error)
{
    if (core)
    {
        error = "the GXM back end owns the display: CreateDevice takes no core";
        return nullptr;
    }
    if ((desc.width && desc.width != gxm::kDisplayWidth) || (desc.height && desc.height != gxm::kDisplayHeight))
    {
        char line[128];
        std::snprintf(line, sizeof(line), "the Vita's screen is %ux%u, not %ux%u", gxm::kDisplayWidth,
                      gxm::kDisplayHeight, desc.width, desc.height);
        error = line;
        return nullptr;
    }
    std::unique_ptr<Device> device(new Device);
    device->desc = desc;
    // The GXM back end reads shaderCachePath as a folder of compiled
    // programs (gxm_program_cache.h).
    gxm::SetProgramCacheFolder(desc.shaderCachePath);
    device->backBuffer.kind = SurfaceKind::BackBuffer;
    device->frontBuffer.kind = SurfaceKind::FrontBuffer;
    device->vsConstants.assign(256 * 4, 0.0f);
    device->psConstants.assign(224 * 4, 0.0f);

    gxm::DisplayDesc display;
    display.memory = PoolSizes(*device);
    // A frame's geometry beyond this is binned in partial renders: slower,
    // not wrong. The default 16 MB is a quarter of what the engine has left.
    display.parameterBufferBytes = 8u << 20;
    display.vsync = true;
    display.log = DisplayLog;
    display.logUser = device.get();
    if (!gxm::InitDisplay(display, error))
        return nullptr;
    if (!InitDraw(*device, error))
    {
        ShutdownDraw(*device);
        gxm::ShutdownDisplay();
        return nullptr;
    }
    InitTargetReserve(*device);
    Log(*device, "%s", gxm::DescribeMemory().c_str());
    SetDefaultState(*device);
    return device.release();
}

void DestroyDevice(Device *device)
{
    if (!device)
        return;
    if (device->live)
        Log(*device, "%u resources were not destroyed before the device", device->live);
    LogShaderSummary(*device, "in all");
    FinishGpu(*device);
    LogDrawSummary(*device);
    ShutdownDraw(*device);
    Log(*device, "%s", gxm::DescribeMemory().c_str());
    gxm::ShutdownDisplay(); // the pools go, the reserve with them
    g_targetReserve = TargetReserve{};
    delete device;
}

const char *DeviceError(const Device *device)
{
    return device && !device->error.empty() ? device->error.c_str() : nullptr;
}

const DeviceStats &Stats(const Device *device)
{
    return device->stats;
}

// No translated shaders yet, so no cache file.
bool LoadShaderCache(const char *, std::string &message)
{
    message = "the GXM back end has no shader cache yet";
    return false;
}
bool SaveShaderCache(const char *, std::string &message)
{
    message = "the GXM back end has no shader cache yet";
    return false;
}
void ClearShaderCache()
{
}
uint32_t ShaderCacheSize()
{
    return 0;
}

// ---- Textures and surfaces ---------------------------------------------------------------

bool FormatSupported(Device *, d3d::Format format, uint32_t usage)
{
    const FormatInfo *info = LookupFormat(format);
    if (!info)
        return false;
    if (usage & d3d::USAGE_DEPTHSTENCIL)
        return info->depth;
    if (usage & d3d::USAGE_RENDERTARGET)
        return info->renderTarget;
    return !info->depth;
}

namespace
{
void LogLargeTexture(Device &device, const Texture &texture)
{
    const TextureDesc &desc = texture.desc;
    char what[96];
    std::snprintf(what, sizeof(what), "%ux%ux%u, %u levels, format %u, usage 0x%x", desc.width, desc.height,
                  desc.kind == TextureKind::Cube ? 6u : desc.kind == TextureKind::Volume ? desc.depth : 1u,
                  desc.levels, unsigned(desc.format), desc.usage);
    LogLargePiece(device, texture.swizzledLayout ? texture.swizzled : texture.memory, what);
}
} // namespace

bool EnsureTargetMemory(Device &device, Texture &texture)
{
    if (!texture.deferredBytes)
        return texture.memory.data != nullptr;
    // A target that found no memory tries again, but not at every draw.
    if (texture.deferFailed && Micros() < texture.deferRetryAt)
        return false;
    const uint32_t bytes = texture.deferredBytes;
    const TextureDesc &desc = texture.desc;
    const MemoryUse use = TextureUse(desc, *texture.format);
    bool got = (desc.usage & d3d::USAGE_RENDERTARGET) && TakeFromTargetReserve(texture, bytes, use);
    if (!got)
        got = texture.deferFailed
                  ? TakeFromPoolsQuietly(texture.memory, bytes, use)
                  : AllocateMemory(device, texture.memory, bytes, kTargetAlign, desc.pool, true, use,
                                   texture.name.c_str());
    if (!got)
    {
        texture.deferFailed = true;
        texture.deferRetryAt = Micros() + kTargetRetryMicros;
        if (texture.deferRefusals++ == 0)
            Log(device,
                "target %ux%u (format %u, usage 0x%x) has no GPU memory: it tries again each second; until then "
                "draws into it are skipped, and so is what samples it",
                desc.width, desc.height, unsigned(desc.format), desc.usage);
        return false;
    }
    if (texture.deferRefusals)
        Log(device, "target %ux%u (format %u, usage 0x%x) has its GPU memory now, after %u refusals", desc.width,
            desc.height, unsigned(desc.format), desc.usage, texture.deferRefusals);
    texture.deferFailed = false;
    texture.deferRefusals = 0;
    std::memset(texture.memory.data, 0, bytes);
    texture.deferredBytes = 0;
    SetUpTexture(device, texture);
    LogLargeTexture(device, texture);
    return true;
}

Texture *CreateTexture(Device *device, const TextureDesc &desc)
{
    const FormatInfo *format = LookupFormat(desc.format);
    if (!device || !format || !desc.width || !desc.height)
        return nullptr;
    const bool renderTarget = (desc.usage & d3d::USAGE_RENDERTARGET) != 0;
    if ((renderTarget && !format->renderTarget) || (format->depth && !(desc.usage & d3d::USAGE_DEPTHSTENCIL)))
    {
        Log(*device, "CreateTexture: format %u cannot be used that way (usage 0x%x)", unsigned(desc.format),
            desc.usage);
        return nullptr;
    }
    std::unique_ptr<Texture> texture(new Texture);
    texture->desc = desc;
    texture->format = format;
    texture->name = desc.name ? desc.name : "";
    texture->faces = desc.kind == TextureKind::Cube ? 6 : 1;
    const uint32_t depth = desc.kind == TextureKind::Volume ? desc.depth : 1;
    if (!texture->desc.levels)
        texture->desc.levels = FullChain(desc.width, desc.kind == TextureKind::Cube ? desc.width : desc.height, depth);
    texture->desc.height = desc.kind == TextureKind::Cube ? desc.width : desc.height;
    texture->levels.resize(texture->faces * texture->desc.levels);
    const uint32_t total = LayoutTexture(*texture);
    // Textures and targets prefer CDRAM, the GPU's own memory. Swizzled
    // textures (cubes, block-compressed) have only their swizzled copy, which
    // SetUpTexture makes.
    texture->swizzledLayout = SwizzledTexture(texture->desc, *format);
    const bool target = (desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL)) != 0;
    if (target && !texture->swizzledLayout && desc.pool != d3d::POOL_SYSTEMMEM && desc.pool != d3d::POOL_SCRATCH)
        texture->deferredBytes = total; // EnsureTargetMemory, at its first use
    else
    {
        if (!texture->swizzledLayout && !AllocateMemory(*device, texture->memory, total, 256, desc.pool, true,
                                                        TextureUse(texture->desc, *format), desc.name))
            return nullptr;
        SetUpTexture(*device, *texture);
        LogLargeTexture(*device, *texture);
    }
    texture->surfaces.resize(texture->levels.size());
    ++device->live;
    return texture.release();
}

void DestroyTexture(Device *device, Texture *texture)
{
    if (!texture)
        return;
    // A scene may be drawing into it.
    if (texture->desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL))
        EndScene(*device);
    for (Texture *&bound : device->textures)
        if (bound == texture)
            bound = nullptr;
    for (const auto &surface : texture->surfaces)
    {
        if (!surface)
            continue;
        for (Surface *&target : device->renderTargets)
            if (target == surface.get())
                target = nullptr;
        if (device->depthStencil == surface.get())
            device->depthStencil = nullptr;
    }
    if (texture->reserved)
    {
        // Targets go at a renderer restart or at shutdown: waiting for the GPU
        // here costs nothing that matters.
        FinishGpu(*device);
        ReturnToTargetReserve(*texture);
    }
    Retire(*device, texture->memory);
    Retire(*device, texture->swizzled);
    --device->live;
    delete texture;
}

const TextureDesc &Describe(const Texture *texture)
{
    return texture->desc;
}

bool LockRect(Device *device, Texture *texture, uint32_t face, uint32_t level, const Box *box, uint32_t flags,
              LockedRect &out)
{
    out = LockedRect{};
    if (!device || !texture || face >= texture->faces || level >= texture->desc.levels)
        return false;
    Level &lock = texture->levels[face * texture->desc.levels + level];
    if (lock.locked)
    {
        Log(*device, "LockRect: %s level %u is already locked", texture->name.c_str(), level);
        return false;
    }
    // What the GPU draws into must be done before the CPU looks.
    if (texture->memory.gpu && (texture->desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL)))
        FinishGpu(*device);
    uint32_t x = 0, y = 0, z = 0;
    if (box && box->right)
    {
        x = box->left;
        y = box->top;
        z = box->front;
        if (texture->format->compressed)
        {
            x /= 4;
            y /= 4;
        }
    }
    if (!EnsureTargetMemory(*device, *texture) && !texture->swizzledLayout)
        return false;
    lock.locked = true;
    uint8_t *base = texture->memory.data + lock.offset;
    if (texture->swizzledLayout)
    {
        // A copy in D3D9's layout, filled from the GPU's copy only when the
        // caller may read what it does not write.
        lock.scratch.assign(size_t(lock.slicePitch) * lock.depth, 0);
        base = lock.scratch.data();
        if ((box && box->right) || (flags & d3d::LOCK_READONLY))
            ReadSwizzled(*texture, face, level, base, lock.pitch);
        lock.readOnly = (flags & d3d::LOCK_READONLY) != 0;
    }
    out.bits = base + z * lock.slicePitch + y * lock.pitch + x * texture->format->blockBytes;
    out.pitch = lock.pitch;
    out.slicePitch = lock.slicePitch;
    return true;
}

void UnlockRect(Device *device, Texture *texture, uint32_t face, uint32_t level)
{
    if (!device || !texture || face >= texture->faces || level >= texture->desc.levels)
        return;
    Level &lock = texture->levels[face * texture->desc.levels + level];
    if (!lock.locked)
        return;
    lock.locked = false;
    if (texture->swizzledLayout)
    {
        if (!lock.readOnly)
            WriteSwizzled(*texture, face, level, lock.scratch.data(), lock.pitch);
        std::vector<uint8_t>().swap(lock.scratch);
    }
    ++device->stats.textureUploads;
    device->stats.uploadBytes += uint64_t(lock.slicePitch) * lock.depth;
}

Surface *GetSurfaceLevel(Device *device, Texture *texture, uint32_t face, uint32_t level)
{
    if (!device || !texture || face >= texture->faces || level >= texture->desc.levels)
        return nullptr;
    std::unique_ptr<Surface> &surface = texture->surfaces[face * texture->desc.levels + level];
    if (!surface)
    {
        surface.reset(new Surface);
        surface->texture = texture;
        surface->face = face;
        surface->level = level;
    }
    return surface.get();
}

namespace
{
Surface *Standalone(Device *device, uint32_t width, uint32_t height, d3d::Format format, uint32_t usage,
                    d3d::Pool pool, const char *name)
{
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.levels = 1;
    desc.usage = usage;
    desc.format = format;
    desc.pool = pool;
    desc.name = name;
    Texture *texture = CreateTexture(device, desc);
    if (!texture)
        return nullptr;
    Surface *surface = GetSurfaceLevel(device, texture, 0, 0);
    surface->standalone = true;
    return surface;
}
} // namespace

Surface *CreateRenderTarget(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name)
{
    return Standalone(device, width, height, format, d3d::USAGE_RENDERTARGET, d3d::POOL_DEFAULT, name);
}

Surface *CreateDepthStencil(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name)
{
    return Standalone(device, width, height, format, d3d::USAGE_DEPTHSTENCIL, d3d::POOL_DEFAULT, name);
}

Surface *CreatePlainSurface(Device *device, uint32_t width, uint32_t height, d3d::Format format, d3d::Pool pool,
                            const char *name)
{
    return Standalone(device, width, height, format, 0, pool, name);
}

void DestroySurface(Device *device, Surface *surface)
{
    if (surface && surface->standalone)
        DestroyTexture(device, surface->texture);
}

Surface *BackBuffer(Device *device)
{
    return device ? &device->backBuffer : nullptr;
}

Surface *FrontBuffer(Device *device)
{
    return device ? &device->frontBuffer : nullptr;
}

void SurfaceSize(const Surface *surface, uint32_t &width, uint32_t &height, d3d::Format &format)
{
    width = height = 0;
    format = d3d::FMT_UNKNOWN;
    if (!surface)
        return;
    if (surface->kind != SurfaceKind::TextureLevel)
    {
        // The renderer sees the screen as X8R8G8B8, D3D9's usual back buffer.
        width = gxm::kDisplayWidth;
        height = gxm::kDisplayHeight;
        format = d3d::FMT_X8R8G8B8;
        return;
    }
    const Texture *texture = surface->texture;
    const Level &level = texture->levels[surface->face * texture->desc.levels + surface->level];
    width = level.width;
    height = level.height;
    format = texture->desc.format;
}

bool LockSurface(Device *device, Surface *surface, const Box *box, uint32_t flags, LockedRect &out)
{
    out = LockedRect{};
    if (!surface)
        return false;
    if (surface->kind != SurfaceKind::TextureLevel)
    {
        NotYet(*device, "LockSurface on the back or front buffer");
        return false;
    }
    return LockRect(device, surface->texture, surface->face, surface->level, box, flags, out);
}

void UnlockSurface(Device *device, Surface *surface)
{
    if (surface && surface->kind == SurfaceKind::TextureLevel)
        UnlockRect(device, surface->texture, surface->face, surface->level);
}

// ---- Buffers -------------------------------------------------------------------------------

namespace
{
template <class T>
T *CreateBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, bool vertices, const char *name)
{
    if (!device || !bytes)
        return nullptr;
    const MemoryUse use = vertices ? ((usage & d3d::USAGE_DYNAMIC) ? MemoryUse::VertexDynamic : MemoryUse::VertexStatic)
                                   : ((usage & d3d::USAGE_DYNAMIC) ? MemoryUse::IndexDynamic : MemoryUse::IndexStatic);
    std::unique_ptr<T> buffer(new T);
    buffer->size = bytes;
    buffer->usage = usage;
    buffer->pool = pool;
    buffer->name = name ? name : "";
    // Dynamic buffers are rewritten every frame: main memory, where CPU
    // writes are cheap. Static geometry goes to CDRAM, which main memory
    // cannot spare.
    const bool dynamic = (usage & d3d::USAGE_DYNAMIC) != 0;
    if (!AllocateMemory(*device, buffer->memory, bytes, 16, pool, !dynamic, use, name))
        return nullptr;
    {
        char what[48];
        std::snprintf(what, sizeof(what), "usage 0x%x", usage);
        LogLargePiece(*device, buffer->memory, what);
    }
    ++device->live;
    return buffer.release();
}

void *LockBuffer(Device *device, GpuBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    if (!device || !buffer || offset > buffer->size || (bytes && offset + bytes > buffer->size))
        return nullptr;
    // DISCARD on a buffer a draw read: fresh memory, so the GPU keeps what
    // it reads. NOOVERWRITE and plain locks write in place.
    if ((flags & d3d::LOCK_DISCARD) && buffer->drawn && buffer->memory.gpu)
        Rename(*device, *buffer);
    buffer->locked = true;
    return buffer->memory.data + offset;
}

void DestroyBuffer(Device *device, GpuBuffer *buffer)
{
    Retire(*device, buffer->memory);
    --device->live;
}
} // namespace

VertexBuffer *CreateVertexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, const char *name)
{
    return CreateBuffer<VertexBuffer>(device, bytes, usage, pool, true, name);
}

void DestroyVertexBuffer(Device *device, VertexBuffer *buffer)
{
    if (!buffer)
        return;
    for (auto &stream : device->streams)
        if (stream.buffer == buffer)
            stream = Device::Stream{};
    DestroyBuffer(device, buffer);
    delete buffer;
}

IndexBuffer *CreateIndexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Format format, d3d::Pool pool,
                               const char *name)
{
    IndexBuffer *buffer = CreateBuffer<IndexBuffer>(device, bytes, usage, pool, false, name);
    if (buffer)
        buffer->indexFormat = format;
    return buffer;
}

void DestroyIndexBuffer(Device *device, IndexBuffer *buffer)
{
    if (!buffer)
        return;
    if (device->indices == buffer)
        device->indices = nullptr;
    DestroyBuffer(device, buffer);
    delete buffer;
}

void *Lock(Device *device, VertexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    return LockBuffer(device, buffer, offset, bytes, flags);
}

void Unlock(Device *, VertexBuffer *buffer)
{
    if (buffer)
        buffer->locked = false;
}

void *Lock(Device *device, IndexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    return LockBuffer(device, buffer, offset, bytes, flags);
}

void Unlock(Device *, IndexBuffer *buffer)
{
    if (buffer)
        buffer->locked = false;
}

// ---- Shaders and declarations -------------------------------------------------------------

namespace
{
void LogShaderSummary(Device &device, const char *when)
{
    const Device::ShaderCounts &c = device.shaderCounts;
    const gxm::ProgramCacheStats &p = gxm::GetProgramCacheStats();
    Log(device,
        "shaders %s: %u created, %u refused by the translator (%.1f ms translating), %u programs ready, "
        "%u failed to compile; %u compiled in %.1f s, %u read from the card in %.1f ms, %u not written",
        when, c.created, c.refused, c.translateMicros / 1000.0, c.compiled, c.failed, p.compiled,
        p.compileMicros / 1e6, p.fromFiles, p.readMicros / 1000.0, p.writeFailures);
    size_t held = 0, peak = 0;
    uint32_t refused = 0;
    gxm::ShaderCompilerMemory(held, peak, refused);
    if (p.compiled || refused)
        Log(device,
            "shader compiler memory %s: %.1f MB of the heap held, %.1f MB at most, %u allocations refused; "
            "unloaded %u times, which gave back %.1f MB",
            when, held / 1048576.0, peak / 1048576.0, refused, p.compilerUnloads, p.compilerGaveBack / 1048576.0);
    if (std::strcmp(when, "in all") == 0)
        for (const auto &[reason, count] : c.refusals)
            Log(device, "shaders not translated: %u x %s", count, reason.c_str());
}

// Up to this many compile failures are logged one by one.
constexpr uint32_t kLoggedShaderFailures = 40;

void CompileDefaultVariant(Device &device, Shader &shader, uint64_t hash)
{
    Device::ShaderCounts &counts = device.shaderCounts;
    std::string reason;
    uint64_t begin = Micros();
    const bool parsed = gxm::cg::Parse(shader.bytecode.data(), shader.bytecode.size(), shader.parsed, reason);
    std::string cg;
    const bool emitted = parsed && gxm::cg::Emit(shader.parsed, gxm::cg::Options{}, cg, reason);
    counts.translateMicros += Micros() - begin;
    const gxm::cg::Reflection &r = shader.parsed.reflection;
    if (!emitted)
    {
        ++counts.refused;
        // One line per distinct reason; the summary counts them all.
        auto known = std::find_if(counts.refusals.begin(), counts.refusals.end(),
                                  [&](const auto &entry) { return entry.first == reason; });
        if (known != counts.refusals.end())
        {
            ++known->second;
            return;
        }
        counts.refusals.emplace_back(reason, 1u);
        Log(device, "shader %016llx (%s_%u_%u, %u tokens) not translated: %s (first of its kind)",
            (unsigned long long)hash, shader.pixel ? "ps" : "vs", r.major, r.minor,
            unsigned(shader.bytecode.size() / 4), reason.c_str());
        return;
    }
    shader.translated = true;
    shader.programCacheKey =
        gxm::ProgramCacheKey(cg, shader.pixel ? gxm::ShaderStage::Fragment : gxm::ShaderStage::Vertex);
    std::string log;
    if (!gxm::GetProgram(cg, shader.pixel ? gxm::ShaderStage::Fragment : gxm::ShaderStage::Vertex, shader.program,
                         log))
    {
        shader.program.clear();
        ++counts.failed;
        if (counts.loggedFailures++ < kLoggedShaderFailures)
        {
            // The compiler's first error is enough to find the cause; the
            // program itself stays on the card next to the cache.
            std::string first = log.substr(0, log.find('\n'));
            Log(device, "shader %016llx (%s_%u_%u) did not compile: %s", (unsigned long long)hash,
                shader.pixel ? "ps" : "vs", r.major, r.minor, first.c_str());
            if (device.desc.shaderCachePath && *device.desc.shaderCachePath)
            {
                char path[256];
                std::snprintf(path, sizeof(path), "%s/failed-%016llx.cg", device.desc.shaderCachePath,
                              (unsigned long long)hash);
                if (FILE *file = std::fopen(path, "wb"))
                {
                    std::fwrite(cg.data(), 1, cg.size(), file);
                    std::fputs("\n/* compiler log:\n", file);
                    std::fwrite(log.data(), 1, log.size(), file);
                    std::fputs("\n*/\n", file);
                    std::fclose(file);
                }
            }
        }
        return;
    }
    ++counts.compiled;
}
} // namespace

Shader *CreateShader(Device *device, const void *bytecode, size_t bytes, std::string &error)
{
    if (!device || !bytecode || bytes < 8)
    {
        error = "no shader bytecode";
        return nullptr;
    }
    const uint64_t begin = Micros();
    std::unique_ptr<Shader> shader(new Shader);
    const uint8_t *code = static_cast<const uint8_t *>(bytecode);
    shader->bytecode.assign(code, code + bytes);
    // The version token's high half: 0xFFFF pixel, 0xFFFE vertex.
    uint32_t version = 0;
    std::memcpy(&version, code, sizeof(version));
    shader->pixel = (version >> 16) == 0xffffu;
    // A shader that cannot be translated or compiled is still created, as
    // D3D9 would; draws with it are skipped. The log says why.
    CompileDefaultVariant(*device, *shader, gxm::cg::Hash(code, bytes));
    ++device->stats.shaders;
    device->stats.shaderMicros += Micros() - begin;
    ++device->live;
    if (++device->shaderCounts.created % 100 == 0)
        LogShaderSummary(*device, "so far");
    return shader.release();
}

void DestroyShader(Device *device, Shader *shader)
{
    if (!shader)
        return;
    if (device->vertexShader == shader)
        device->vertexShader = nullptr;
    if (device->pixelShader == shader)
        device->pixelShader = nullptr;
    ForgetShader(*device, *shader);
    --device->live;
    delete shader;
}

VertexDecl *CreateVertexDecl(Device *device, const VertexElement *elements, uint32_t count)
{
    if (!device || (count && !elements))
        return nullptr;
    VertexDecl *decl = new VertexDecl;
    decl->elements.assign(elements, elements + count);
    ++device->live;
    return decl;
}

void DestroyVertexDecl(Device *device, VertexDecl *decl)
{
    if (!decl)
        return;
    if (device->decl == decl)
        device->decl = nullptr;
    ForgetDecl(*device, *decl);
    --device->live;
    delete decl;
}

// ---- State ------------------------------------------------------------------------------------

void SetRenderState(Device *device, d3d::RenderState state, uint32_t value)
{
    if (device && state < d3d::RS_COUNT)
        device->renderStates[state] = value;
}

uint32_t GetRenderState(const Device *device, d3d::RenderState state)
{
    return device && state < d3d::RS_COUNT ? device->renderStates[state] : 0;
}

namespace
{
int SamplerSlot(uint32_t sampler)
{
    if (sampler < 16)
        return int(sampler);
    if (sampler >= kVertexSamplerBase && sampler < kVertexSamplerBase + 4)
        return int(16 + sampler - kVertexSamplerBase);
    return -1;
}
} // namespace

void SetSamplerState(Device *device, uint32_t sampler, d3d::SamplerState state, uint32_t value)
{
    const int slot = SamplerSlot(sampler);
    if (device && slot >= 0 && state < d3d::SAMP_COUNT)
        device->samplerStates[slot][state] = value;
}

void SetTexture(Device *device, uint32_t sampler, Texture *texture)
{
    const int slot = SamplerSlot(sampler);
    if (device && slot >= 0)
        device->textures[slot] = texture;
}

void SetVertexShader(Device *device, Shader *shader)
{
    if (device)
        device->vertexShader = shader;
}

void SetPixelShader(Device *device, Shader *shader)
{
    if (device)
        device->pixelShader = shader;
}

namespace
{
void SetConstants(std::vector<float> &constants, uint32_t start, const float *data, uint32_t count)
{
    if (!data || start * 4 >= constants.size())
        return;
    const size_t floats = std::min<size_t>(size_t(count) * 4, constants.size() - start * 4);
    std::memcpy(constants.data() + start * 4, data, floats * sizeof(float));
}
} // namespace

void SetVertexShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (device)
    {
        SetConstants(device->vsConstants, start, data, count);
        device->stats.constantBytes += uint64_t(count) * 16;
    }
}

void SetPixelShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count)
{
    if (device)
    {
        SetConstants(device->psConstants, start, data, count);
        device->stats.constantBytes += uint64_t(count) * 16;
    }
}

void SetVertexDecl(Device *device, VertexDecl *decl)
{
    if (device)
        device->decl = decl;
}

void SetStreamSource(Device *device, uint32_t stream, VertexBuffer *buffer, uint32_t offset, uint32_t stride)
{
    if (device && stream < kMaxStreams)
        device->streams[stream] = Device::Stream{buffer, offset, stride};
}

void SetIndices(Device *device, IndexBuffer *buffer)
{
    if (device)
        device->indices = buffer;
}

void SetViewport(Device *device, const Viewport &viewport)
{
    if (device)
        device->viewport = viewport;
}

void SetScissorRect(Device *device, const Rect &rect)
{
    if (device)
        device->scissor = rect;
}

void SetRenderTarget(Device *device, uint32_t index, Surface *surface)
{
    if (!device || index >= kMaxRenderTargets)
        return;
    device->renderTargets[index] = surface;
    if (index == 0 && surface)
    {
        uint32_t width = 0, height = 0;
        d3d::Format format = d3d::FMT_UNKNOWN;
        SurfaceSize(surface, width, height, format);
        device->viewport = Viewport{0, 0, width, height, 0.0f, 1.0f};
        device->scissor = Rect{0, 0, int32_t(width), int32_t(height)};
    }
}

void SetDepthStencilSurface(Device *device, Surface *surface)
{
    if (device)
        device->depthStencil = surface;
}

// ---- Drawing --------------------------------------------------------------------------------

void Clear(Device *device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
           uint32_t stencil)
{
    if (!device || device->detached)
        return;
    ++device->stats.clears;
    ClearTargets(*device, rectCount, rects, flags, color, z, stencil);
}

void DrawIndexedPrimitive(Device *device, d3d::PrimitiveType type, int32_t baseVertex, uint32_t, uint32_t,
                          uint32_t startIndex, uint32_t primitiveCount)
{
    if (!device || device->detached)
        return;
    ++device->stats.draws;
    Draw(*device, type, baseVertex, startIndex, primitiveCount, true, nullptr, 0);
}

void DrawPrimitive(Device *device, d3d::PrimitiveType type, uint32_t startVertex, uint32_t primitiveCount)
{
    if (!device || device->detached)
        return;
    ++device->stats.draws;
    Draw(*device, type, int32_t(startVertex), 0, primitiveCount, false, nullptr, 0);
}

void DrawPrimitiveUP(Device *device, d3d::PrimitiveType type, uint32_t primitiveCount, const void *vertices,
                     uint32_t stride)
{
    if (!device)
        return;
    ++device->stats.draws;
    if (!device->detached && vertices)
        Draw(*device, type, 0, 0, primitiveCount, false, vertices, stride);
    device->streams[0] = Device::Stream{};
}

// ---- Copies, frames and queries ---------------------------------------------------------------

void StretchRect(Device *device, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear)
{
    if (device && !device->detached)
        Copy(*device, src, srcRect, dst, dstRect, linear);
}

bool GetRenderTargetData(Device *device, Surface *renderTarget, Surface *dst)
{
    Target source, destination;
    if (!device)
        return false;
    for (Surface *surface : {renderTarget, dst})
        if (surface && surface->kind == SurfaceKind::TextureLevel && surface->texture)
            EnsureTargetMemory(*device, *surface->texture);
    if (!Describe(renderTarget, source) || !Describe(dst, destination))
        return false;
    if (source.width != destination.width || source.height != destination.height ||
        source.format->blockBytes != 4 || destination.format->blockBytes != 4)
    {
        Log(*device, "GetRenderTargetData: the surfaces differ in size or are not 32-bit");
        return false;
    }
    FinishGpu(*device);
    for (uint32_t y = 0; y < source.height; ++y)
    {
        const uint8_t *from = source.data + size_t(y) * source.pitch;
        uint8_t *to = destination.data + size_t(y) * destination.pitch;
        if (!source.display)
        {
            std::memcpy(to, from, size_t(source.width) * 4);
            continue;
        }
        // The display's A8B8G8R8 into D3D9's A8R8G8B8.
        for (uint32_t x = 0; x < source.width; ++x)
        {
            to[x * 4 + 0] = from[x * 4 + 2];
            to[x * 4 + 1] = from[x * 4 + 1];
            to[x * 4 + 2] = from[x * 4 + 0];
            to[x * 4 + 3] = from[x * 4 + 3];
        }
    }
    return true;
}

void UpdateTexture(Device *device, Texture *src, Texture *dst)
{
    if (!device || !src || !dst || src->desc.format != dst->desc.format || src->faces != dst->faces)
        return;
    const uint32_t skip = dst->desc.width < src->desc.width ? FullChain(src->desc.width / dst->desc.width, 1, 1) - 1 : 0;
    for (uint32_t face = 0; face < dst->faces; ++face)
    {
        for (uint32_t level = 0; level < dst->desc.levels && level + skip < src->desc.levels; ++level)
        {
            const Level &from = src->levels[face * src->desc.levels + level + skip];
            const Level &to = dst->levels[face * dst->desc.levels + level];
            if (!src->memory.data)
                return;
            if (dst->swizzledLayout)
            {
                WriteSwizzled(*dst, face, level, src->memory.data + from.offset, from.pitch);
                continue;
            }
            const uint32_t rows = Rows(*dst->format, to.height) * to.depth;
            const uint32_t bytes = std::min(from.pitch, to.pitch);
            for (uint32_t row = 0; row < rows; ++row)
                std::memcpy(dst->memory.data + to.offset + size_t(row) * to.pitch,
                            src->memory.data + from.offset + size_t(row) * from.pitch, bytes);
        }
    }
}

PresentResult Present(Device *device)
{
    if (!device)
        return PresentResult::Failed;
    if (device->detached)
        return PresentResult::Skipped;
    DiagnoseFrame(*device);
    EndScene(*device);
    const bool shown = gxm::Present();
    FrameDone(*device);
    gxm::ProgramCacheFrame();
    // Once a minute, beside the system's memory line (vita_stats.cpp).
    static uint64_t memoryLogged = 0;
    if (Micros() - memoryLogged > 60 * 1000 * 1000)
    {
        memoryLogged = Micros();
        Log(*device, "%s", gxm::DescribeMemory().c_str());
        Log(*device, "%s", DescribeMemoryUse().c_str());
    }
    if (!shown)
    {
        device->error = "sceGxm could not present";
        return PresentResult::Failed;
    }
    return PresentResult::Ok;
}

void DetachWindow(Device *device)
{
    if (device)
        device->detached = true;
}

bool ResizeBackBuffer(Device *device, uint32_t width, uint32_t height)
{
    if (!device)
        return false;
    if (width != gxm::kDisplayWidth || height != gxm::kDisplayHeight)
    {
        Log(*device, "the back buffer stays %ux%u, the Vita's screen (asked for %ux%u)", gxm::kDisplayWidth,
            gxm::kDisplayHeight, width, height);
        return false;
    }
    return true;
}

void ResetState(Device *device)
{
    if (device)
        SetDefaultState(*device);
}

void WaitIdle(Device *device)
{
    if (device)
        FinishGpu(*device);
}

Query *CreateQuery(Device *device, QueryKind kind)
{
    if (!device)
        return nullptr;
    Query *query = new Query;
    query->kind = kind;
    ++device->live;
    return query;
}

void DestroyQuery(Device *device, Query *query)
{
    if (!query)
        return;
    --device->live;
    delete query;
}

void IssueQuery(Device *, Query *query, bool)
{
    if (query)
        query->issued = true;
}

// Nothing is drawn yet, so every query is done at once; occlusion queries
// report one sample, as if what they cover were visible.
bool GetQueryData(Device *, Query *query, bool, uint32_t &result)
{
    if (!query)
        return false;
    result = 1;
    return true;
}
} // namespace gfx_vk::legacy
