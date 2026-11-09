// The Legacy adapter's API (src/gfx_vk/legacy/vk_legacy.h) on the PS Vita's
// sceGxm, for the D3D9 front (src/gfx_vk/d3d9) in the Vita client.
//
// Stage 2, step 2b (docs/vita-client.md): everything but drawing.
// - The device owns the screen (gxm_display.h) and the GPU memory pools.
// - Resources live in GPU-mapped memory with D3D9's lock semantics.
// - Clears fill colour targets with sceGxmTransferFill.
// - Present shows the back buffer.
// - Shaders and vertex declarations are kept but not compiled yet.
// - Draws are counted and skipped.
// This lets the client run its menus, with real resource memory, on the
// Vita. The next step translates the shaders and draws.
//
// Texture levels are stored row by row (blocks of 4x4 for DXT), exactly as
// D3D9 locks them, and LockRect hands out the GPU memory itself. The layouts
// sceGxm samples (swizzled DXT, strides in multiples of 8 texels) come with
// drawing. That step decides whether textures get converted on Unlock.
//
// One thread at a time calls everything; the front's lock sees to that.

#if !defined(KISAK_GFX_GXM)
#error "gxm_legacy.cpp is the PS Vita's Legacy back end (KISAK_GFX_GXM)"
#endif

#include <gfx_vk/legacy/vk_legacy.h>

#include <gfx_gxm/gxm_display.h>
#include <gfx_gxm/gxm_memory.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace gfx_vk::legacy
{
namespace
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

const FormatInfo *LookupFormat(d3d::Format format)
{
    for (const FormatInfo &info : kFormats)
        if (info.d3d == format)
            return &info;
    return nullptr;
}

uint32_t LevelSize(uint32_t size, uint32_t level)
{
    return std::max(1u, size >> level);
}

uint32_t FullChain(uint32_t width, uint32_t height, uint32_t depth)
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

uint32_t AlignUp(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

// Rows of a level as LockRect sees them: texel rows, or rows of 4x4 blocks.
uint32_t Rows(const FormatInfo &format, uint32_t height)
{
    return format.compressed ? std::max(1u, (height + 3) / 4) : height;
}

// Bytes per row. Colour formats sceGxm may render to keep rows a multiple
// of 32 texels, which colour surfaces want; the rest are packed.
uint32_t Pitch(const FormatInfo &format, uint32_t width, bool renderTarget)
{
    if (format.compressed)
        return std::max(1u, (width + 3) / 4) * format.blockBytes;
    if (renderTarget)
        return AlignUp(width, 32) * format.blockBytes;
    return width * format.blockBytes;
}
} // namespace

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
    std::string name;
};
struct VertexBuffer : GpuBuffer
{
};
struct IndexBuffer : GpuBuffer
{
};

struct Shader
{
    std::vector<uint8_t> bytecode;
    bool pixel = false;
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
    uint32_t live = 0; // resources not yet destroyed
};

namespace
{
void Log(Device &device, const char *format, ...)
{
    if (!device.desc.log)
        return;
    char line[512];
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

// GPU memory: the preferred pool, then the other one. SYSTEMMEM and SCRATCH
// resources stay in the C heap.
bool AllocateMemory(Device &device, Memory &memory, uint32_t bytes, uint32_t align, d3d::Pool pool, bool preferCdram,
                    const char *name)
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
            return true;
        }
    }
    Log(device, "out of GPU memory for %s (%u KB); %s", name ? name : "a resource", bytes >> 10,
        gxm::DescribeMemory().c_str());
    return false;
}

void FreeMemory(Memory &memory)
{
    if (memory.gpu)
        gxm::Free(memory.kind, memory.data);
    memory = Memory{};
}

// The display's raw texel for a D3DCOLOR: the display buffers are A8B8G8R8.
uint32_t DisplayColor(uint32_t color)
{
    return (color & 0xff00ff00u) | ((color >> 16) & 0xffu) | ((color & 0xffu) << 16);
}

// A D3DCOLOR in a 16-bit format's bits; false for formats a fill cannot take.
bool Color16(d3d::Format format, uint32_t color, uint32_t &out)
{
    const uint32_t a = color >> 24, r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
    switch (format)
    {
    case d3d::FMT_R5G6B5:
        out = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
        return true;
    case d3d::FMT_A1R5G5B5:
    case d3d::FMT_X1R5G5B5:
        out = (a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
        return true;
    case d3d::FMT_A4R4G4B4:
        out = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
        return true;
    default:
        return false;
    }
}

// Where a colour surface's texels are.
struct Target
{
    uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, pitch = 0;
    const FormatInfo *format = nullptr;
    bool display = false;
};

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
    if (!texture)
        return false;
    const Level &level = texture->levels[surface->face * texture->desc.levels + surface->level];
    out.data = texture->memory.data + level.offset;
    out.width = level.width;
    out.height = level.height;
    out.pitch = level.pitch;
    out.format = texture->format;
    return true;
}

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
    sizes.main = 16u << 20;
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
    Log(*device, "%s", gxm::DescribeMemory().c_str());
    gxm::ShutdownDisplay();
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
    const uint32_t height = desc.kind == TextureKind::Cube ? desc.width : desc.height;
    texture->desc.height = height;

    uint32_t total = 0;
    texture->levels.resize(texture->faces * texture->desc.levels);
    for (uint32_t face = 0; face < texture->faces; ++face)
    {
        for (uint32_t i = 0; i < texture->desc.levels; ++i)
        {
            Level &level = texture->levels[face * texture->desc.levels + i];
            level.width = LevelSize(desc.width, i);
            level.height = LevelSize(height, i);
            level.depth = LevelSize(depth, i);
            level.pitch = Pitch(*format, level.width, renderTarget);
            level.slicePitch = level.pitch * Rows(*format, level.height);
            total = AlignUp(total, 16);
            level.offset = total;
            total += level.slicePitch * level.depth;
        }
    }
    // Textures and targets prefer CDRAM, the GPU's own memory.
    if (!AllocateMemory(*device, texture->memory, total, 256, desc.pool, true, desc.name))
        return nullptr;
    texture->surfaces.resize(texture->levels.size());
    ++device->live;
    return texture.release();
}

void DestroyTexture(Device *device, Texture *texture)
{
    if (!texture)
        return;
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
    FreeMemory(texture->memory);
    --device->live;
    delete texture;
}

const TextureDesc &Describe(const Texture *texture)
{
    return texture->desc;
}

bool LockRect(Device *device, Texture *texture, uint32_t face, uint32_t level, const Box *box, uint32_t,
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
    // Clears and copies by the GPU must be done before the CPU looks.
    if (texture->memory.gpu)
        sceGxmTransferFinish();
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
    lock.locked = true;
    out.bits = texture->memory.data + lock.offset + z * lock.slicePitch + y * lock.pitch + x * texture->format->blockBytes;
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
T *CreateBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, const char *name)
{
    if (!device || !bytes)
        return nullptr;
    std::unique_ptr<T> buffer(new T);
    buffer->size = bytes;
    buffer->usage = usage;
    buffer->pool = pool;
    buffer->name = name ? name : "";
    // Dynamic buffers are rewritten every frame: main memory, where CPU
    // writes are cheap. Static geometry goes to CDRAM, which main memory
    // cannot spare.
    const bool dynamic = (usage & d3d::USAGE_DYNAMIC) != 0;
    if (!AllocateMemory(*device, buffer->memory, bytes, 16, pool, !dynamic, name))
        return nullptr;
    ++device->live;
    return buffer.release();
}

void *LockBuffer(Device *device, GpuBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t)
{
    if (!device || !buffer || offset > buffer->size || (bytes && offset + bytes > buffer->size))
        return nullptr;
    // DISCARD and NOOVERWRITE need no new memory while nothing is drawn: the
    // GPU never reads a buffer yet. Drawing will rename discarded buffers.
    buffer->locked = true;
    return buffer->memory.data + offset;
}

void DestroyBuffer(Device *device, GpuBuffer *buffer)
{
    FreeMemory(buffer->memory);
    --device->live;
}
} // namespace

VertexBuffer *CreateVertexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, const char *name)
{
    return CreateBuffer<VertexBuffer>(device, bytes, usage, pool, name);
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
    IndexBuffer *buffer = CreateBuffer<IndexBuffer>(device, bytes, usage, pool, name);
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
    ++device->stats.shaders;
    device->stats.shaderMicros += Micros() - begin;
    ++device->live;
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

namespace
{
void Fill(Device &device, const Target &target, const Rect &rect, uint32_t color)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    const uint32_t x = uint32_t(rect.left), y = uint32_t(rect.top);
    const uint32_t width = uint32_t(rect.right - rect.left), height = uint32_t(rect.bottom - rect.top);
    SceGxmTransferFormat format;
    uint32_t value = 0;
    if (target.format->blockBytes == 4 && !target.format->depth &&
        (target.display || target.format->d3d == d3d::FMT_A8R8G8B8 || target.format->d3d == d3d::FMT_X8R8G8B8))
    {
        format = SCE_GXM_TRANSFER_FORMAT_RAW32;
        // A8R8G8B8 is D3DCOLOR's own byte order; the display is A8B8G8R8.
        value = target.display ? DisplayColor(color) : color;
    }
    else if (target.format->blockBytes == 2 && Color16(target.format->d3d, color, value))
        format = SCE_GXM_TRANSFER_FORMAT_RAW16;
    else
    {
        NotYet(device, "Clear of a render target that is not 16- or 32-bit RGB");
        return;
    }
    const int result = sceGxmTransferFill(value, format, target.data, x, y, width, height, int32_t(target.pitch),
                                          nullptr, 0, nullptr);
    if (result < 0)
    {
        Log(device, "sceGxmTransferFill failed (0x%08x); clearing on the CPU", unsigned(result));
        const uint32_t bytes = target.format->blockBytes;
        for (uint32_t row = y; row < y + height; ++row)
        {
            uint8_t *line = target.data + size_t(row) * target.pitch + size_t(x) * bytes;
            for (uint32_t column = 0; column < width; ++column)
                std::memcpy(line + column * bytes, &value, bytes);
        }
    }
}
} // namespace

void Clear(Device *device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float, uint32_t)
{
    if (!device || device->detached)
        return;
    ++device->stats.clears;
    if (!(flags & d3d::CLEAR_TARGET))
        return; // depth and stencil: nothing reads them until drawing works
    Target target;
    if (!Describe(device->renderTargets[0], target))
        return;
    // D3D9 clears the viewport, cut by the scissor rect when the test is on,
    // and the rects within that.
    Rect area{int32_t(device->viewport.x), int32_t(device->viewport.y),
              int32_t(device->viewport.x + device->viewport.width),
              int32_t(device->viewport.y + device->viewport.height)};
    auto intersect = [](Rect a, const Rect &b) {
        a.left = std::max(a.left, b.left);
        a.top = std::max(a.top, b.top);
        a.right = std::min(a.right, b.right);
        a.bottom = std::min(a.bottom, b.bottom);
        return a;
    };
    area = intersect(area, Rect{0, 0, int32_t(target.width), int32_t(target.height)});
    if (device->renderStates[d3d::RS_SCISSORTESTENABLE])
        area = intersect(area, device->scissor);
    if (!rectCount || !rects)
        Fill(*device, target, area, color);
    else
        for (uint32_t i = 0; i < rectCount; ++i)
            Fill(*device, target, intersect(area, rects[i]), color);
}

void DrawIndexedPrimitive(Device *device, d3d::PrimitiveType, int32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
    if (device)
        ++device->stats.draws;
}

void DrawPrimitive(Device *device, d3d::PrimitiveType, uint32_t, uint32_t)
{
    if (device)
        ++device->stats.draws;
}

void DrawPrimitiveUP(Device *device, d3d::PrimitiveType, uint32_t, const void *, uint32_t)
{
    if (!device)
        return;
    ++device->stats.draws;
    device->streams[0] = Device::Stream{};
}

// ---- Copies, frames and queries ---------------------------------------------------------------

void StretchRect(Device *device, Surface *, const Rect *, Surface *, const Rect *, bool)
{
    if (device)
        NotYet(*device, "StretchRect");
}

bool GetRenderTargetData(Device *device, Surface *renderTarget, Surface *dst)
{
    Target source, destination;
    if (!device || !Describe(renderTarget, source) || !Describe(dst, destination))
        return false;
    if (source.width != destination.width || source.height != destination.height ||
        source.format->blockBytes != 4 || destination.format->blockBytes != 4)
    {
        Log(*device, "GetRenderTargetData: the surfaces differ in size or are not 32-bit");
        return false;
    }
    sceGxmTransferFinish();
    gxm::Finish();
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
    sceGxmTransferFinish();
    const uint32_t skip = dst->desc.width < src->desc.width ? FullChain(src->desc.width / dst->desc.width, 1, 1) - 1 : 0;
    for (uint32_t face = 0; face < dst->faces; ++face)
    {
        for (uint32_t level = 0; level < dst->desc.levels && level + skip < src->desc.levels; ++level)
        {
            const Level &from = src->levels[face * src->desc.levels + level + skip];
            const Level &to = dst->levels[face * dst->desc.levels + level];
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
    // Clears go through the transfer queue; the display queue waits only for
    // scenes, so the fills must be done before the buffer is shown.
    sceGxmTransferFinish();
    if (!gxm::Present())
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

void WaitIdle(Device *)
{
    sceGxmTransferFinish();
    gxm::Finish();
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
