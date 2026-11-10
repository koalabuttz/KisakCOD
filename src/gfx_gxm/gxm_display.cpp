#include "gxm_display.h"

// psp2/display.h checks that its enums are one or two bytes, as VitaSDK's
// libraries are built (-fshort-enums); KisakCOD's are four (vita-clang.cmake).
// The display calls here pass enums by value and use no structure with an
// enum field, so this file is built without VitaSDK's size checks.
#if defined(ENABLE_VITASDK_BUILD_ASSERT)
#error "build gxm_display.cpp with -UENABLE_VITASDK_BUILD_ASSERT (see the comment above)"
#endif
#include <psp2/display.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gfx_gxm
{
namespace
{
// SetBeforeFlip's hook. Its own variable: the thread that sets it is not the
// one that draws.
std::atomic<BeforeFlip> g_beforeFlip{nullptr};

// What the display queue hands its callback: the buffer to show.
struct DisplayData
{
    void *address;
};

struct State
{
    bool initialized = false; // sceGxmInitialize ran
    bool memory = false;
    bool vsync = true;
    void (*log)(void *, const char *) = nullptr;
    void *logUser = nullptr;

    void *hostMem = nullptr;
    void *vdmRing = nullptr;
    void *vertexRing = nullptr;
    void *fragmentRing = nullptr;
    void *fragmentUsseRing = nullptr;
    SceGxmContext *context = nullptr;
    SceGxmRenderTarget *renderTarget = nullptr;
    SceGxmShaderPatcher *patcher = nullptr;

    void *displayData[kDisplayBuffers] = {};
    SceGxmColorSurface displaySurface[kDisplayBuffers];
    SceGxmSyncObject *displaySync[kDisplayBuffers] = {};
    void *depthData = nullptr;
    SceGxmDepthStencilSurface depthSurface;

    struct SizedTarget
    {
        uint32_t width, height;
        SceGxmRenderTarget *target;
    };
    std::vector<SizedTarget> targets; // RenderTargetFor

    uint32_t back = 0;
    uint32_t front = kDisplayBuffers - 1;
    bool inScene = false;
    DisplayStats stats{};
};
State g;

void Log(const char *format, ...)
{
    if (!g.log)
        return;
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g.log(g.logUser, line);
}

bool Fail(std::string &error, const char *what, int result)
{
    char line[160];
    std::snprintf(line, sizeof(line), "gxm: %s failed (0x%08x)", what, static_cast<unsigned>(result));
    error = line;
    return false;
}

void DisplayCallback(const void *callbackData)
{
    const DisplayData *data = static_cast<const DisplayData *>(callbackData);
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    frame.base = data->address;
    frame.pitch = kDisplayStride;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = kDisplayWidth;
    frame.height = kDisplayHeight;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (g.vsync)
        sceDisplayWaitVblankStart();
}

// The shader patcher's memory: host structures from the C heap, its GPU
// buffers and programs' code from the pools.
void *PatcherHostAlloc(void *, SceSize size)
{
    return std::malloc(size);
}
void PatcherHostFree(void *, void *memory)
{
    std::free(memory);
}
void *PatcherBufferAlloc(void *, SceSize size)
{
    return Allocate(MemoryKind::Main, size, 16);
}
void PatcherBufferFree(void *, void *memory)
{
    Free(MemoryKind::Main, memory);
}
void *PatcherVertexUsseAlloc(void *, SceSize size, unsigned int *offset)
{
    void *memory = Allocate(MemoryKind::VertexUsse, size, 16);
    if (memory)
        *offset = UsseOffset(MemoryKind::VertexUsse, memory);
    return memory;
}
void PatcherVertexUsseFree(void *, void *memory)
{
    Free(MemoryKind::VertexUsse, memory);
}
void *PatcherFragmentUsseAlloc(void *, SceSize size, unsigned int *offset)
{
    void *memory = Allocate(MemoryKind::FragmentUsse, size, 16);
    if (memory)
        *offset = UsseOffset(MemoryKind::FragmentUsse, memory);
    return memory;
}
void PatcherFragmentUsseFree(void *, void *memory)
{
    Free(MemoryKind::FragmentUsse, memory);
}

bool CreateContext(std::string &error)
{
    g.hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    g.vdmRing = Allocate(MemoryKind::Main, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4096);
    g.vertexRing = Allocate(MemoryKind::Main, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4096);
    g.fragmentRing = Allocate(MemoryKind::Main, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4096);
    g.fragmentUsseRing = Allocate(MemoryKind::FragmentUsse, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, 4096);
    if (!g.hostMem || !g.vdmRing || !g.vertexRing || !g.fragmentRing || !g.fragmentUsseRing)
    {
        error = "gxm: no memory for the context's ring buffers";
        return false;
    }
    SceGxmContextParams params{};
    params.hostMem = g.hostMem;
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = g.vdmRing;
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = g.vertexRing;
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = g.fragmentRing;
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferMem = g.fragmentUsseRing;
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = UsseOffset(MemoryKind::FragmentUsse, g.fragmentUsseRing);
    const int result = sceGxmCreateContext(&params, &g.context);
    return result >= 0 || Fail(error, "sceGxmCreateContext", result);
}

bool CreateSurfaces(std::string &error)
{
    SceGxmRenderTargetParams target{};
    target.flags = 0;
    target.width = kDisplayWidth;
    target.height = kDisplayHeight;
    target.scenesPerFrame = 1;
    target.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    target.multisampleLocations = 0;
    target.driverMemBlock = -1;
    int result = sceGxmCreateRenderTarget(&target, &g.renderTarget);
    if (result < 0)
        return Fail(error, "sceGxmCreateRenderTarget", result);

    const uint32_t displayBytes = kDisplayStride * kDisplayHeight * 4;
    for (uint32_t i = 0; i < kDisplayBuffers; ++i)
    {
        g.displayData[i] = Allocate(MemoryKind::Cdram, displayBytes, 256 * 1024);
        if (!g.displayData[i])
        {
            error = "gxm: no CDRAM for the display buffers";
            return false;
        }
        std::memset(g.displayData[i], 0, displayBytes); // black until the first frame
        result = sceGxmColorSurfaceInit(&g.displaySurface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                        SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                        SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kDisplayWidth, kDisplayHeight,
                                        kDisplayStride, g.displayData[i]);
        if (result < 0)
            return Fail(error, "sceGxmColorSurfaceInit", result);
        result = sceGxmSyncObjectCreate(&g.displaySync[i]);
        if (result < 0)
            return Fail(error, "sceGxmSyncObjectCreate", result);
    }

    // Depth and stencil, interleaved, in the GPU's tiles.
    const uint32_t depthWidth = (kDisplayWidth + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const uint32_t depthHeight = (kDisplayHeight + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    g.depthData = Allocate(MemoryKind::Cdram, depthWidth * depthHeight * 4, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    if (!g.depthData)
    {
        error = "gxm: no CDRAM for the depth-stencil surface";
        return false;
    }
    result = sceGxmDepthStencilSurfaceInit(&g.depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                           SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, depthWidth, g.depthData, nullptr);
    return result >= 0 || Fail(error, "sceGxmDepthStencilSurfaceInit", result);
}

bool CreatePatcher(std::string &error)
{
    SceGxmShaderPatcherParams params{};
    params.hostAllocCallback = PatcherHostAlloc;
    params.hostFreeCallback = PatcherHostFree;
    params.bufferAllocCallback = PatcherBufferAlloc;
    params.bufferFreeCallback = PatcherBufferFree;
    params.vertexUsseAllocCallback = PatcherVertexUsseAlloc;
    params.vertexUsseFreeCallback = PatcherVertexUsseFree;
    params.fragmentUsseAllocCallback = PatcherFragmentUsseAlloc;
    params.fragmentUsseFreeCallback = PatcherFragmentUsseFree;
    const int result = sceGxmShaderPatcherCreate(&params, &g.patcher);
    return result >= 0 || Fail(error, "sceGxmShaderPatcherCreate", result);
}
} // namespace

bool InitDisplay(const DisplayDesc &desc, std::string &error)
{
    if (g.initialized)
    {
        error = "gxm: the display is already initialized";
        return false;
    }
    g.vsync = desc.vsync;
    g.log = desc.log;
    g.logUser = desc.logUser;

    SceGxmInitializeParams params{};
    params.flags = 0;
    params.displayQueueMaxPendingCount = kDisplayBuffers - 1;
    params.displayQueueCallback = DisplayCallback;
    params.displayQueueCallbackDataSize = sizeof(DisplayData);
    params.parameterBufferSize = desc.parameterBufferBytes;
    const int result = sceGxmInitialize(&params);
    if (result < 0)
        return Fail(error, "sceGxmInitialize", result);
    g.initialized = true;

    if (!InitMemory(desc.memory, error))
    {
        ShutdownDisplay();
        return false;
    }
    g.memory = true;
    if (!CreateContext(error) || !CreateSurfaces(error) || !CreatePatcher(error))
    {
        ShutdownDisplay();
        return false;
    }
    g.back = 0;
    g.front = kDisplayBuffers - 1;
    Log("gxm: display %ux%u, %u buffers, vsync %s, parameter buffer %u KB", kDisplayWidth, kDisplayHeight,
        kDisplayBuffers, g.vsync ? "on" : "off", desc.parameterBufferBytes >> 10);
    return true;
}

void ShutdownDisplay()
{
    if (!g.initialized)
        return;
    if (g.context)
    {
        if (g.inScene)
            sceGxmEndScene(g.context, nullptr, nullptr);
        sceGxmFinish(g.context);
    }
    sceGxmDisplayQueueFinish();
    if (g.patcher)
        sceGxmShaderPatcherDestroy(g.patcher);
    for (uint32_t i = 0; i < kDisplayBuffers; ++i)
        if (g.displaySync[i])
            sceGxmSyncObjectDestroy(g.displaySync[i]);
    for (const State::SizedTarget &sized : g.targets)
        sceGxmDestroyRenderTarget(sized.target);
    if (g.renderTarget)
        sceGxmDestroyRenderTarget(g.renderTarget);
    if (g.context)
        sceGxmDestroyContext(g.context);
    std::free(g.hostMem);
    // The pools go whole; nothing needs freeing piece by piece.
    if (g.memory)
        ShutdownMemory();
    sceGxmTerminate();
    void (*log)(void *, const char *) = g.log;
    void *logUser = g.logUser;
    g = State{};
    if (log)
        log(logUser, "gxm: display shut down");
}

bool DisplayReady()
{
    return g.initialized && g.patcher;
}

SceGxmContext *Context()
{
    return g.context;
}

SceGxmShaderPatcher *ShaderPatcher()
{
    return g.patcher;
}

void *BackBufferData()
{
    return g.displayData[g.back];
}

void *FrontBufferData()
{
    return g.displayData[g.front];
}

const SceGxmColorSurface *BackBufferSurface()
{
    return &g.displaySurface[g.back];
}

SceGxmSyncObject *BackBufferSync()
{
    return g.displaySync[g.back];
}

SceGxmRenderTarget *RenderTargetFor(uint32_t width, uint32_t height)
{
    if (!DisplayReady() || !width || !height)
        return nullptr;
    for (const State::SizedTarget &sized : g.targets)
        if (sized.width == width && sized.height == height)
            return sized.target;
    SceGxmRenderTargetParams params{};
    params.flags = 0;
    params.width = static_cast<uint16_t>(width);
    params.height = static_cast<uint16_t>(height);
    // A frame may come back to the same target a few times (a render to
    // texture between two parts of the frame).
    params.scenesPerFrame = 4;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.multisampleLocations = 0;
    params.driverMemBlock = -1;
    SceGxmRenderTarget *target = nullptr;
    const int result = sceGxmCreateRenderTarget(&params, &target);
    if (result < 0)
    {
        Log("gxm: sceGxmCreateRenderTarget %ux%u failed (0x%08x)", width, height, static_cast<unsigned>(result));
        return nullptr;
    }
    g.targets.push_back(State::SizedTarget{width, height, target});
    return target;
}

const SceGxmDepthStencilSurface *DepthStencilSurface()
{
    return &g.depthSurface;
}

bool BeginScene()
{
    if (!DisplayReady() || g.inScene)
        return false;
    const int result = sceGxmBeginScene(g.context, 0, g.renderTarget, nullptr, nullptr, g.displaySync[g.back],
                                        &g.displaySurface[g.back], &g.depthSurface);
    if (result < 0)
    {
        Log("gxm: sceGxmBeginScene failed (0x%08x)", static_cast<unsigned>(result));
        return false;
    }
    g.inScene = true;
    ++g.stats.scenes;
    return true;
}

bool EndScene()
{
    if (!g.inScene)
        return false;
    g.inScene = false;
    const int result = sceGxmEndScene(g.context, nullptr, nullptr);
    if (result < 0)
    {
        Log("gxm: sceGxmEndScene failed (0x%08x)", static_cast<unsigned>(result));
        return false;
    }
    return true;
}

bool InScene()
{
    return g.inScene;
}

void SetBeforeFlip(BeforeFlip hook)
{
    g_beforeFlip.store(hook, std::memory_order_release);
}

bool Present()
{
    if (!DisplayReady())
        return false;
    if (g.inScene)
        EndScene();
    if (const BeforeFlip hook = g_beforeFlip.load(std::memory_order_acquire))
        hook(FlipInfo{g.displayData[g.back], g.depthData, g.displaySync[g.back]});
    // The heartbeat marks the end of a frame for the system (sceGxm's
    // samples call it once per displayed frame).
    sceGxmPadHeartbeat(&g.displaySurface[g.back], g.displaySync[g.back]);
    DisplayData data{g.displayData[g.back]};
    const int result = sceGxmDisplayQueueAddEntry(g.displaySync[g.front], g.displaySync[g.back], &data);
    if (result < 0)
    {
        Log("gxm: sceGxmDisplayQueueAddEntry failed (0x%08x)", static_cast<unsigned>(result));
        return false;
    }
    g.front = g.back;
    g.back = (g.back + 1) % kDisplayBuffers;
    ++g.stats.frames;
    return true;
}

void Finish()
{
    if (g.context)
        sceGxmFinish(g.context);
}

DisplayStats GetDisplayStats()
{
    return g.stats;
}
} // namespace gfx_gxm
