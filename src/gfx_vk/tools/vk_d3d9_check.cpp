// KisakCOD-vulkan-d3d9-check: drives the Vulkan Legacy adapter's D3D9 front
// (src/gfx_vk/d3d9/vk_d3d9.h) through the D3D9 interfaces, as the renderer
// will, and checks what comes back: pixels read with GetRenderTargetData,
// reference counts, state getters, queries, Reset, Present and teardown.
//
// The adapter's own drawing is checked pixel by pixel by
// KisakCOD-vulkan-legacy-check; these scenes check the front's part: that
// every D3D9 call reaches the adapter with the right objects and values, that
// objects live exactly as long as D3D9 says, and that nothing the renderer
// calls falls through to an unsupported method. The shaders are the legacy
// check's (scripts/tools/vulkan/gen_legacy_check_shaders.py). Nothing of the
// engine is linked and no game file is read.
//
// Output is key=value lines on stdout: d3d9_check.scene=<name> lines with
// points and mismatches, counters, and d3d9_check=passed or
// d3d9_check=failed stage=<stage> last. Exit status 0 means every check
// passed, 1 a failure, 2 a usage error. See docs/vulkan-d3d9-front.md.

#include <gfx_vk/d3d9/vk_d3d9.h>
#include <gfx_vk/modern/vk_modern.h>

#include "vk_legacy_check_shaders.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// MSVC rejects sscanf as unsafe (C4996, an error under /WX). The formats here
// read numbers only, and for those sscanf_s takes the same arguments.
#ifdef _MSC_VER
#define KISAK_SCAN_NUMBERS sscanf_s
#else
#define KISAK_SCAN_NUMBERS std::sscanf
#endif

namespace
{
namespace front = gfx_vk::d3d9;

// Whether the back end times its passes (KISAK_VK_PASS_TIMESTAMPS=0: no).
bool PassesTimed()
{
    const char *value = SDL_getenv("KISAK_VK_PASS_TIMESTAMPS");
    return !(value && std::strcmp(value, "0") == 0);
}

namespace shaders = vk_legacy_check_shaders;

constexpr UINT kSize = 128;
constexpr int kTolerance = 2;
// Calls the scenes make on purpose to see them refused as unsupported.
constexpr uint64_t kExpectedUnsupported = 1;

struct Vertex
{
    float x, y, z;
    float u, v;
    uint32_t color; // D3DCOLOR
};
static_assert(sizeof(Vertex) == 24, "the declaration expects 24-byte vertices");

struct Options
{
    gfx_vk::Validation validation = gfx_vk::Validation::IfFound;
    int device = -1;
    std::string only;
    // --keep-draw-state 0: the bench scenes draw with front::Tuning's
    // keepDrawState off, to set their timings beside the default's.
    bool benchKeepsDrawState = true;
};

void Usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s [--validation off|auto|require] [--device N] [--scene NAME] [--keep-draw-state 0|1]\n",
                 argv0);
}

bool ParseOptions(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--validation" && next)
        {
            const std::string mode = next;
            if (mode == "off")
                options.validation = gfx_vk::Validation::Off;
            else if (mode == "auto")
                options.validation = gfx_vk::Validation::IfFound;
            else if (mode == "require")
                options.validation = gfx_vk::Validation::Required;
            else
                return false;
            ++i;
        }
        else if (arg == "--device" && next)
        {
            char *end = nullptr;
            const long value = std::strtol(next, &end, 10);
            if (!next[0] || *end || value < 0 || value > 63)
                return false;
            options.device = static_cast<int>(value);
            ++i;
        }
        else if (arg == "--scene" && next && next[0])
        {
            options.only = next;
            ++i;
        }
        else if (arg == "--keep-draw-state" && next && (!std::strcmp(next, "0") || !std::strcmp(next, "1")))
        {
            options.benchKeepsDrawState = next[0] == '1';
            ++i;
        }
        else
        {
            return false;
        }
    }
    return true;
}

int Failed(const char *stage, const std::string &reason)
{
    std::printf("d3d9_check=failed stage=%s\n", stage);
    std::printf("d3d9_check.reason=%s\n", reason.c_str());
    std::fflush(stdout);
    return 1;
}

// ---- The host: an SDL window --------------------------------------------------------------------

struct SdlHost
{
    SDL_Window *window = nullptr;
    const char *const *extensions = nullptr;
    Uint32 extensionCount = 0;
    gfx_vk::CoreStats finalStats{};
    bool coreDestroyed = false;
    // Under the mutex: what HostTuning hands the front at every Present, and
    // the log lines a scene asked to keep (the tuning scene), any thread's.
    std::mutex mutex;
    front::Tuning tuning{};
    std::vector<std::string> *logKept = nullptr;
    bool benchKeepsDrawState = true; // Options::benchKeepsDrawState
    // Host::fullscreen's calls, in order: the mode asked for, or windowed
    // (null) as all zeros with asked false. The front calls it under its lock.
    struct FullscreenCall
    {
        bool fullscreen;
        front::FullscreenMode mode;
    };
    std::vector<FullscreenCall> fullscreenCalls;
};

void LogLine(void *user, const char *line)
{
    std::printf("d3d9_check.log=%s\n", line);
    std::fflush(stdout);
    SdlHost *host = static_cast<SdlHost *>(user);
    const std::lock_guard<std::mutex> lock(host->mutex);
    if (host->logKept)
        host->logKept->push_back(line);
}

void HostTuning(void *user, front::Tuning &out)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    const std::lock_guard<std::mutex> lock(host->mutex);
    out = host->tuning;
}

bool CreateSurface(void *user, VkInstance instance, VkSurfaceKHR *surface)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    if (SDL_Vulkan_CreateSurface(host->window, instance, nullptr, surface))
        return true;
    std::printf("d3d9_check.log=SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    return false;
}

void GetPixelSize(void *user, uint32_t *width, uint32_t *height)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    int w = 0;
    int h = 0;
    if (!SDL_GetWindowSizeInPixels(host->window, &w, &h) || (SDL_GetWindowFlags(host->window) & SDL_WINDOW_MINIMIZED))
        w = h = 0;
    *width = static_cast<uint32_t>(w > 0 ? w : 0);
    *height = static_cast<uint32_t>(h > 0 ? h : 0);
}

front::D3D9Window WindowHandle(SdlHost &host)
{
    return reinterpret_cast<front::D3D9Window>(host.window);
}

bool HostWindowSystem(void *user, front::D3D9Window window, gfx_vk::WindowSystem &out)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    if (window != WindowHandle(*host))
        return false;
    out = gfx_vk::WindowSystem{};
    out.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    out.instanceExtensions = host->extensions;
    out.instanceExtensionCount = host->extensionCount;
    out.user = host;
    out.createSurface = CreateSurface;
    out.getPixelSize = GetPixelSize;
    return out.getInstanceProcAddr != nullptr;
}

// Two modes, so enumeration is checked against something known.
uint32_t HostModeCount(void *) { return 2; }

bool HostMode(void *, uint32_t index, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    if (index > 1)
        return false;
    width = index ? 1920 : 1280;
    height = index ? 1080 : 720;
    refreshRate = 60;
    return true;
}

bool HostCurrentMode(void *, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    return HostMode(nullptr, 1, width, height, refreshRate);
}

// A made-up place for the window on its display, so the windowed front buffer
// copy has an offset to get right.
constexpr int32_t kWindowOriginX = 16, kWindowOriginY = 8;
bool HostWindowOrigin(void *, front::D3D9Window, int32_t &x, int32_t &y)
{
    x = kWindowOriginX;
    y = kWindowOriginY;
    return true;
}

bool HostFullscreen(void *user, front::D3D9Window window, const front::FullscreenMode *mode)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    if (window != WindowHandle(*host))
        return false;
    host->fullscreenCalls.push_back({mode != nullptr, mode ? *mode : front::FullscreenMode{}});
    return true;
}

void HostCoreDestroyed(void *user, const gfx_vk::CoreStats &stats)
{
    SdlHost *host = static_cast<SdlHost *>(user);
    host->finalStats = stats;
    host->coreDestroyed = true;
}

void PumpEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
    }
}

// ---- Drawing helpers ------------------------------------------------------------------------------

uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

float ClipX(float edge, UINT size) { return (edge - 0.5f) / float(size) * 2.0f - 1.0f; }
float ClipY(float edge, UINT size) { return 1.0f - (edge - 0.5f) / float(size) * 2.0f; }

// A quad covering pixels x0..x1-1, y0..y1-1 of a size x size target, as four
// clockwise vertices (top-left, top-right, bottom-right, bottom-left).
void QuadCorners(Vertex out[4], float x0, float y0, float x1, float y1, float z, uint32_t color, UINT size = kSize)
{
    out[0] = Vertex{ClipX(x0, size), ClipY(y0, size), z, 0.0f, 0.0f, color};
    out[1] = Vertex{ClipX(x1, size), ClipY(y0, size), z, 1.0f, 0.0f, color};
    out[2] = Vertex{ClipX(x1, size), ClipY(y1, size), z, 1.0f, 1.0f, color};
    out[3] = Vertex{ClipX(x0, size), ClipY(y1, size), z, 0.0f, 1.0f, color};
}

void QuadList(Vertex out[6], float x0, float y0, float x1, float y1, float z, uint32_t color, UINT size = kSize)
{
    Vertex c[4];
    QuadCorners(c, x0, y0, x1, y1, z, color, size);
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = c[0];
    out[4] = c[2];
    out[5] = c[3];
}

struct SceneResult
{
    int points = 0;
    int mismatched = 0;
    std::string detail;
    std::string error;
};

template <class T>
void SafeRelease(T *&object)
{
    if (object)
        object->Release();
    object = nullptr;
}

class Check
{
public:
    SdlHost *host = nullptr;
    IDirect3D9 *d3d = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DVertexShader9 *vsBasic = nullptr;
    IDirect3DPixelShader9 *psColor = nullptr, *psTexture = nullptr, *psShadow = nullptr, *psVolume = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DSurface9 *readSurface = nullptr;
    IDirect3DSurface9 *backBuffer = nullptr;
    D3DPRESENT_PARAMETERS params{};
    std::vector<uint8_t> pixels; // B, G, R, A per pixel
    UINT readSize = kSize;
    SceneResult *result = nullptr;
    uint64_t presented = 0;

    bool CreateDevice(std::string &error);
    bool CreateObjects(std::string &error);
    void ReleaseObjects();

    // ---- helpers -----------------------------------------------------------------------------
    void Fail(const std::string &why)
    {
        if (result->error.empty())
            result->error = why;
    }

    bool Ok(HRESULT hr, const char *what)
    {
        if (SUCCEEDED(hr))
            return true;
        char line[160];
        std::snprintf(line, sizeof(line), "%s failed (0x%08x)", what, static_cast<unsigned>(hr));
        Fail(line);
        return false;
    }

    // A value the scene expects, as one point.
    template <class A, class B>
    void Equal(const char *name, A got, B want)
    {
        ++result->points;
        if (static_cast<long long>(got) == static_cast<long long>(want))
            return;
        ++result->mismatched;
        char line[200];
        std::snprintf(line, sizeof(line), " %s=%lld expected=%lld", name, static_cast<long long>(got),
                      static_cast<long long>(want));
        result->detail += line;
    }

    void Reset(uint32_t background = Argb(255, 0, 0, 0))
    {
        Ok(device->SetRenderTarget(0, backBuffer), "SetRenderTarget(back buffer)");
        for (DWORD i = 1; i < 4; ++i)
            device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(nullptr);
        const std::pair<D3DRENDERSTATETYPE, DWORD> states[] = {
            {D3DRS_ZENABLE, FALSE},          {D3DRS_ZWRITEENABLE, FALSE},  {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
            {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
            {D3DRS_CULLMODE, D3DCULL_NONE},  {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_COLORWRITEENABLE, 0xf},
        };
        for (const auto &state : states)
            Ok(device->SetRenderState(state.first, state.second), "SetRenderState");
        const std::pair<D3DSAMPLERSTATETYPE, DWORD> samplerStates[] = {
            {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
            {D3DSAMP_MIPFILTER, D3DTEXF_NONE},  {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
            {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
        };
        for (const auto &state : samplerStates)
            Ok(device->SetSamplerState(0, state.first, state.second), "SetSamplerState");
        device->SetTexture(0, nullptr);
        device->SetVertexShader(vsBasic);
        device->SetPixelShader(psColor);
        device->SetVertexDeclaration(decl);
        const float zero[4] = {0, 0, 0, 0};
        device->SetVertexShaderConstantF(0, zero, 1);
        Ok(device->Clear(0, nullptr, D3DCLEAR_TARGET, background, 1.0f, 0), "Clear");
    }

    void DrawQuad(float x0, float y0, float x1, float y1, float z, uint32_t color, UINT size = kSize)
    {
        Vertex quad[6];
        QuadList(quad, x0, y0, x1, y1, z, color, size);
        Ok(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex)), "DrawPrimitiveUP");
    }

    // Copies a render target into pixels through a SYSTEMMEM surface of its size.
    bool Read(IDirect3DSurface9 *target, UINT size)
    {
        if (size != readSize)
        {
            SafeRelease(readSurface);
            if (!Ok(device->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readSurface,
                                                        nullptr),
                    "CreateOffscreenPlainSurface"))
                return false;
            readSize = size;
        }
        if (!Ok(device->GetRenderTargetData(target, readSurface), "GetRenderTargetData"))
            return false;
        return ReadLocked(size);
    }

    bool Read() { return Read(backBuffer, kSize); }

    bool ReadLocked(UINT size)
    {
        D3DLOCKED_RECT locked{};
        if (!Ok(readSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect(read surface)"))
            return false;
        pixels.resize(size * size * 4);
        for (UINT y = 0; y < size; ++y)
            std::memcpy(&pixels[y * size * 4], static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, size * 4);
        readSurface->UnlockRect();
        return true;
    }

    void Expect(const char *name, UINT x, UINT y, int r, int g, int b, int a = -1)
    {
        const uint8_t *p = &pixels[(y * readSize + x) * 4];
        const int got[4] = {p[2], p[1], p[0], p[3]};
        const int want[4] = {r, g, b, a};
        bool ok = true;
        for (int c = 0; c < 4; ++c)
            ok = ok && (want[c] < 0 || std::abs(got[c] - want[c]) <= kTolerance);
        ++result->points;
        if (!ok)
        {
            ++result->mismatched;
            char line[160];
            std::snprintf(line, sizeof(line), " %s@%u,%u=%d,%d,%d,%d expected=%d,%d,%d,%d", name, x, y, got[0], got[1],
                          got[2], got[3], r, g, b, a);
            result->detail += line;
        }
    }

    // A texture whose level 0 is filled with texel(x, y) (A8R8G8B8 as 0xAARRGGBB).
    template <class F>
    IDirect3DTexture9 *FilledTexture(UINT width, UINT height, F texel)
    {
        IDirect3DTexture9 *texture = nullptr;
        if (!Ok(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr),
                "CreateTexture"))
            return nullptr;
        D3DLOCKED_RECT locked{};
        if (!Ok(texture->LockRect(0, &locked, nullptr, 0), "LockRect"))
        {
            texture->Release();
            return nullptr;
        }
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const uint32_t value = texel(x, y);
                std::memcpy(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + 4 * x, &value, 4);
            }
        texture->UnlockRect(0);
        return texture;
    }

    front::DeviceCounters Counters()
    {
        front::DeviceCounters counters{};
        front::Counters(device, counters);
        return counters;
    }

    // ---- scenes ------------------------------------------------------------------------------
    void Adapter();
    void States();
    void Draw();
    void RenderTarget();
    void ShadowMap();
    void ModelLighting();
    void DirtyUpdate();
    void References();
    void Threads();
    void Queries();
    void Occlusion();
    void FrameLoop();
    void GpuFeed();
    void Tuning();
    void SlowFrames();
    void KeptState();
    void Multisample();
    void ModernTaa();
    void Bench();
    void BenchRuns();
    void BenchFrames(bool runs);
    void ResetDevice();
    void Fullscreen();
    void Present();
};

// IDirect3D9: what the renderer asks before it creates the device.
void Check::Adapter()
{
    Equal("adapter_count", d3d->GetAdapterCount(), 1u);
    D3DADAPTER_IDENTIFIER9 id{};
    Ok(d3d->GetAdapterIdentifier(0, 0, &id), "GetAdapterIdentifier");
    Equal("identifier_named", id.Description[0] != 0, true);
    Equal("mode_count", d3d->GetAdapterModeCount(0, D3DFMT_X8R8G8B8), 2u);
    D3DDISPLAYMODE mode{};
    Ok(d3d->EnumAdapterModes(0, D3DFMT_X8R8G8B8, 1, &mode), "EnumAdapterModes");
    Equal("mode1_width", mode.Width, 1920u);
    Equal("mode1_format", static_cast<uint32_t>(mode.Format), static_cast<uint32_t>(D3DFMT_X8R8G8B8));
    Equal("mode2_refused", d3d->EnumAdapterModes(0, D3DFMT_X8R8G8B8, 2, &mode), D3DERR_INVALIDCALL);
    Ok(d3d->GetAdapterDisplayMode(0, &mode), "GetAdapterDisplayMode");
    Equal("current_height", mode.Height, 1080u);

    D3DCAPS9 caps{};
    Ok(d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps), "GetDeviceCaps");
    Equal("vs_3_0", caps.VertexShaderVersion, static_cast<DWORD>(D3DVS_VERSION(3, 0)));
    Equal("ps_3_0", caps.PixelShaderVersion, static_cast<DWORD>(D3DPS_VERSION(3, 0)));
    Equal("render_targets", caps.NumSimultaneousRTs, 4ul);

    struct FormatCase
    {
        const char *name;
        DWORD usage;
        D3DRESOURCETYPE type;
        D3DFORMAT format;
        HRESULT want;
    };
    const FormatCase formats[] = {
        {"tex_argb", 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8, D3D_OK},
        {"tex_dxt1", 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1, D3D_OK},
        {"rt_argb", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8, D3D_OK},
        {"rt_r32f", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F, D3D_OK},
        {"ds_d24s8", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8, D3D_OK},
        {"rt_dxt1", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_DXT1, D3DERR_NOTAVAILABLE},
        {"ds_argb", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8, D3DERR_NOTAVAILABLE},
    };
    for (const FormatCase &f : formats)
        Equal(f.name, d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.usage, f.type, f.format), f.want);
    Equal("msaa_none", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_NONE,
                                                       nullptr),
          D3D_OK);
    // 2, 4 and 8 samples are offered (the adapter makes a count the device
    // lacks with 4); nothing else is.
    DWORD qualities = 0;
    Equal("msaa_4x", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES,
                                                     &qualities),
          D3D_OK);
    Equal("msaa_4x_qualities", qualities, DWORD(1));
    Equal("msaa_8x", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_8_SAMPLES,
                                                     nullptr),
          D3D_OK);
    Equal("msaa_3x", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_3_SAMPLES,
                                                     nullptr),
          D3DERR_NOTAVAILABLE);
    Equal("msaa_nonmaskable", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
                                                              D3DMULTISAMPLE_NONMASKABLE, nullptr),
          D3DERR_NOTAVAILABLE);
    Equal("depth_match", d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_D24S8),
          D3D_OK);
}

// State round trips, defaults and refusals.
void Check::States()
{
    DWORD value = 0;
    // Defaults right after CreateDevice (no automatic depth surface).
    Ok(device->GetRenderState(D3DRS_ZENABLE, &value), "GetRenderState");
    Equal("default_zenable", value, static_cast<DWORD>(FALSE));
    Ok(device->GetRenderState(D3DRS_CULLMODE, &value), "GetRenderState");
    Equal("default_cull", value, static_cast<DWORD>(D3DCULL_CCW));
    Ok(device->GetSamplerState(0, D3DSAMP_ADDRESSU, &value), "GetSamplerState");
    Equal("default_address", value, static_cast<DWORD>(D3DTADDRESS_WRAP));
    D3DVIEWPORT9 viewport{};
    Ok(device->GetViewport(&viewport), "GetViewport");
    Equal("default_viewport", viewport.Width, kSize);

    Ok(device->SetRenderState(D3DRS_ALPHAREF, 77), "SetRenderState");
    Ok(device->GetRenderState(D3DRS_ALPHAREF, &value), "GetRenderState");
    Equal("alpharef", value, 77ul);
    Equal("bad_state", device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(210), 1), D3DERR_INVALIDCALL);
    Ok(device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR), "SetSamplerState(vertex)");
    Ok(device->GetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, &value), "GetSamplerState(vertex)");
    Equal("vertex_sampler", value, static_cast<DWORD>(D3DTEXF_LINEAR));
    Equal("bad_sampler", device->SetSamplerState(300, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR), D3DERR_INVALIDCALL);

    const float constants[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    float back[8] = {};
    Ok(device->SetVertexShaderConstantF(5, constants, 2), "SetVertexShaderConstantF");
    Ok(device->GetVertexShaderConstantF(5, back, 2), "GetVertexShaderConstantF");
    Equal("vs_constants", std::memcmp(constants, back, sizeof(back)), 0);
    Equal("vs_constant_range", device->SetVertexShaderConstantF(255, constants, 2), D3DERR_INVALIDCALL);
    Ok(device->SetPixelShaderConstantF(223, constants, 1), "SetPixelShaderConstantF");

    // Fixed function is not supported: the call is refused and counted.
    const uint64_t before = Counters().unsupportedCalls;
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    Equal("transform_refused", device->SetTransform(D3DTS_WORLD, &identity), D3DERR_INVALIDCALL);
    Equal("unsupported_counted", Counters().unsupportedCalls, before + 1);
    Ok(device->SetFVF(0), "SetFVF(0)");
    Ok(device->SetSoftwareVertexProcessing(FALSE), "SetSoftwareVertexProcessing(FALSE)");
}

// Buffers, declarations, shaders and textures through the device: an indexed
// draw from static buffers, a draw from memory, and a textured quad.
void Check::Draw()
{
    Reset(Argb(255, 0, 0, 64));

    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    if (!Ok(device->CreateVertexBuffer(4 * sizeof(Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr),
            "CreateVertexBuffer")
        || !Ok(device->CreateIndexBuffer(6 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                         &ib, nullptr),
               "CreateIndexBuffer"))
    {
        SafeRelease(vb);
        return;
    }
    void *data = nullptr;
    if (Ok(vb->Lock(0, 0, &data, 0), "Lock(vertex buffer)"))
    {
        QuadCorners(static_cast<Vertex *>(data), 0, 0, 64, 64, 0.5f, Argb(255, 255, 0, 0));
        vb->Unlock();
    }
    if (Ok(ib->Lock(0, 0, &data, 0), "Lock(index buffer)"))
    {
        const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(data, indices, sizeof(indices));
        ib->Unlock();
    }
    D3DVERTEXBUFFER_DESC vbDesc{};
    vb->GetDesc(&vbDesc);
    Equal("vb_size", vbDesc.Size, static_cast<UINT>(4 * sizeof(Vertex)));
    Ok(device->SetStreamSource(0, vb, 0, sizeof(Vertex)), "SetStreamSource");
    Ok(device->SetIndices(ib), "SetIndices");
    Ok(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2), "DrawIndexedPrimitive");

    // From memory: D3D9 leaves stream 0 unbound afterwards.
    DrawQuad(64, 0, 128, 32, 0.5f, Argb(255, 0, 255, 0));
    IDirect3DVertexBuffer9 *bound = nullptr;
    UINT offset = 1, stride = 1;
    Ok(device->GetStreamSource(0, &bound, &offset, &stride), "GetStreamSource");
    Equal("stream0_cleared", bound == nullptr && stride == 0, true);
    SafeRelease(bound);

    // A 2x2 texture, point sampled over a 64x64 quad: four 32x32 squares.
    const uint32_t colours[4] = {Argb(255, 255, 255, 0), Argb(255, 0, 255, 255), Argb(255, 255, 0, 255),
                                 Argb(255, 255, 255, 255)};
    IDirect3DTexture9 *texture = FilledTexture(2, 2, [&](UINT x, UINT y) { return colours[y * 2 + x]; });
    if (texture)
    {
        Ok(device->SetTexture(0, texture), "SetTexture");
        Ok(device->SetPixelShader(psTexture), "SetPixelShader");
        DrawQuad(0, 64, 64, 128, 0.5f, 0xffffffffu);
        IDirect3DBaseTexture9 *got = nullptr;
        Ok(device->GetTexture(0, &got), "GetTexture");
        Equal("get_texture", got == texture, true);
        SafeRelease(got);
    }

    // A vertex shader constant moves the indexed quad 64 pixels down and right.
    const float move[4] = {1.0f, -1.0f, 0.0f, 0.0f};
    device->SetPixelShader(psColor);
    device->SetVertexShaderConstantF(0, move, 1);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    Ok(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2), "DrawIndexedPrimitive(moved)");

    if (Read())
    {
        Expect("indexed", 32, 32, 255, 0, 0, 255);
        Expect("from_memory", 96, 16, 0, 255, 0);
        Expect("texel_00", 16, 80, 255, 255, 0);
        Expect("texel_10", 48, 80, 0, 255, 255);
        Expect("texel_01", 16, 112, 255, 0, 255);
        Expect("texel_11", 48, 112, 255, 255, 255);
        Expect("moved", 96, 96, 255, 0, 0);
        Expect("background", 96, 48, 0, 0, 64);
    }
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    device->SetTexture(0, nullptr);
    SafeRelease(texture);
    SafeRelease(vb);
    SafeRelease(ib);
}

// A render-target texture with a depth surface, sampled afterwards, and
// StretchRect from its level into the back buffer.
void Check::RenderTarget()
{
    Reset();
    IDirect3DTexture9 *texture = nullptr;
    IDirect3DSurface9 *level = nullptr, *depth = nullptr;
    if (!Ok(device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture,
                                  nullptr),
            "CreateTexture(render target)")
        || !Ok(texture->GetSurfaceLevel(0, &level), "GetSurfaceLevel")
        || !Ok(device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr),
               "CreateDepthStencilSurface"))
    {
        SafeRelease(level);
        SafeRelease(texture);
        return;
    }
    Ok(device->SetRenderTarget(0, level), "SetRenderTarget(level)");
    Ok(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    Equal("target_viewport", viewport.Width, 64ul);
    IDirect3DSurface9 *got = nullptr;
    Ok(device->GetRenderTarget(0, &got), "GetRenderTarget");
    Equal("get_render_target", got == level, true);
    SafeRelease(got);
    Ok(device->GetDepthStencilSurface(&got), "GetDepthStencilSurface");
    Equal("get_depth", got == depth, true);
    SafeRelease(got);

    Ok(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, Argb(255, 0, 0, 255), 1.0f, 0), "Clear(target)");
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    DrawQuad(0, 0, 32, 64, 0.25f, Argb(255, 255, 0, 0), 64);  // near, left half
    DrawQuad(0, 0, 64, 64, 0.75f, Argb(255, 0, 255, 0), 64);  // far, all: hidden on the left
    if (Read(level, 64))
    {
        Expect("near_red", 16, 32, 255, 0, 0);
        Expect("far_green", 48, 32, 0, 255, 0);
    }

    Reset();
    Ok(device->SetTexture(0, texture), "SetTexture(render target)");
    device->SetPixelShader(psTexture);
    DrawQuad(0, 0, 128, 128, 0.5f, 0xffffffffu);
    device->SetTexture(0, nullptr);
    RECT corner{0, 0, 32, 32};
    Ok(device->StretchRect(level, nullptr, backBuffer, &corner, D3DTEXF_POINT), "StretchRect");
    if (Read())
    {
        Expect("sampled_red", 32, 64, 255, 0, 0);
        Expect("sampled_green", 96, 64, 0, 255, 0);
        Expect("stretched_red", 8, 8, 255, 0, 0);
        Expect("stretched_green", 24, 8, 0, 255, 0);
    }
    SafeRelease(depth);
    SafeRelease(level);
    SafeRelease(texture);
}

// The renderer's sun shadow (R_SetShadowmapFormats_DX): a D24S8 texture made
// with D3DUSAGE_DEPTHSTENCIL is the depth target of the shadow pass, beside a
// R5G6B5 colour target, then sampled with linear filtering as a hardware
// shadow map, which compares the coordinate's z with the stored depth and
// returns 1 where z is at or in front of it, 0 behind.
void Check::ShadowMap()
{
    Reset();
    IDirect3DTexture9 *shadow = nullptr, *color = nullptr;
    IDirect3DSurface9 *shadowLevel = nullptr, *colorLevel = nullptr;
    if (!Ok(device->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &shadow, nullptr),
            "CreateTexture(D24S8 depth)")
        || !Ok(shadow->GetSurfaceLevel(0, &shadowLevel), "GetSurfaceLevel(depth)")
        || !Ok(device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT, &color,
                                     nullptr),
               "CreateTexture(R5G6B5 target)")
        || !Ok(color->GetSurfaceLevel(0, &colorLevel), "GetSurfaceLevel(colour)"))
    {
        SafeRelease(colorLevel);
        SafeRelease(color);
        SafeRelease(shadowLevel);
        SafeRelease(shadow);
        return;
    }
    D3DSURFACE_DESC desc{};
    shadowLevel->GetDesc(&desc);
    Equal("depth_usage", desc.Usage, static_cast<DWORD>(D3DUSAGE_DEPTHSTENCIL));

    // The shadow pass: depth 0.25 on the left half, the clear's 1.0 elsewhere.
    Ok(device->SetRenderTarget(0, colorLevel), "SetRenderTarget(colour)");
    Ok(device->SetDepthStencilSurface(shadowLevel), "SetDepthStencilSurface(depth texture)");
    Ok(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, Argb(255, 0, 0, 0), 1.0f, 0), "Clear(shadow)");
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    DrawQuad(0, 0, 32, 64, 0.25f, Argb(255, 255, 255, 255), 64);

    // Sampled with the reference depth from the colour's red.
    Reset();
    Ok(device->SetTexture(0, shadow), "SetTexture(depth)");
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetPixelShader(psShadow);
    DrawQuad(0, 0, 128, 64, 0.5f, Argb(255, 0x80, 0, 0));  // 0.5: behind 0.25, in front of 1.0
    DrawQuad(0, 64, 128, 128, 0.5f, Argb(255, 0x20, 0, 0)); // 0.125: in front of both
    device->SetTexture(0, nullptr);
    if (Read())
    {
        Expect("behind_shadowed", 32, 32, 0, 0, 0);
        Expect("in_front_lit", 96, 32, 255, 255, 255);
        Expect("nearer_lit", 32, 96, 255, 255, 255);
        Expect("nearer_lit_right", 96, 96, 255, 255, 255);
    }

    // The same shader and state with a colour texture between two draws of
    // the depth one: only the texture changes, so the pipeline kept from the
    // draw before has to follow it where depth compare is fixed per pipeline
    // (PipelineKey::depthSamplers).
    IDirect3DTexture9 *green = FilledTexture(4, 4, [](UINT, UINT) { return Argb(255, 0, 255, 0); });
    if (green)
    {
        Reset(Argb(255, 64, 64, 64));
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetPixelShader(psShadow);
        device->SetTexture(0, shadow);
        DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 0x80, 0, 0));
        device->SetTexture(0, green);
        DrawQuad(64, 0, 128, 64, 0.5f, Argb(255, 0x80, 0, 0));
        device->SetTexture(0, shadow);
        DrawQuad(0, 64, 64, 128, 0.5f, Argb(255, 0x80, 0, 0));
        device->SetTexture(0, nullptr);
        if (Read())
        {
            Expect("depth_then_colour_shadowed", 16, 32, 0, 0, 0);
            Expect("depth_then_colour_lit", 48, 32, 255, 255, 255);
            Expect("colour_between_depth", 96, 32, 0, 255, 0);
            Expect("depth_after_colour_shadowed", 16, 96, 0, 0, 0);
            Expect("depth_after_colour_lit", 48, 96, 255, 255, 255);
        }
        green->Release();
    }
    SafeRelease(colorLevel);
    SafeRelease(color);
    SafeRelease(shadowLevel);
    SafeRelease(shadow);
}

// The renderer's model lighting volume (RB_PatchModelLighting): a 256-wide,
// 4-deep A8R8G8B8 volume of 4x4x4-texel cells, a few of them rewritten each
// frame and every other cell expected to keep its texels. The renderer has two
// ways to do that, chosen by r_altModelLightingUpdate, and they must give the
// same texels:
//   0: one MANAGED volume, locked whole with no flags, cells written in place.
//   1: a SYSTEMMEM volume locked whole with D3DLOCK_NO_DIRTY_UPDATE, one
//      AddDirtyBox per cell written, then UpdateTexture into a DEFAULT volume
//      (made with usage 0, as Image_Create3DTexture_PC makes it), which is
//      the one that is drawn with.
// Two frames of patches go through both; after each, every cell is drawn
// slice by slice from both volumes and read back. As in D3D9, UpdateTexture
// copies only what is dirty: the whole new volume the first time, then only
// the cells given to AddDirtyBox. A cell written in the second frame without
// one stays out of the drawn volume, and that frame's update uploads two
// cells' bytes in one copy.
void Check::ModelLighting()
{
    Reset();
    constexpr UINT kWidth = 256, kHeight = 8, kDepth = 4, kCell = 4;
    IDirect3DVolumeTexture9 *managed = nullptr, *staging = nullptr, *shown = nullptr;
    if (!Ok(device->CreateVolumeTexture(kWidth, kHeight, kDepth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed,
                                        nullptr),
            "CreateVolumeTexture(managed)")
        || !Ok(device->CreateVolumeTexture(kWidth, kHeight, kDepth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                           &staging, nullptr),
               "CreateVolumeTexture(system memory)")
        || !Ok(device->CreateVolumeTexture(kWidth, kHeight, kDepth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shown,
                                           nullptr),
               "CreateVolumeTexture(default)"))
    {
        SafeRelease(staging);
        SafeRelease(managed);
        return;
    }

    struct Patch
    {
        UINT cell;     // the renderer's modelLightingIndex: 64 cells a row
        uint32_t seed; // the red of every texel written, 0 for a cell never written
    };
    // A cell's texel: red says which patch wrote it, green its x, blue its y and z.
    const auto texel = [](uint32_t seed, UINT x, UINT y, UINT z) {
        return Argb(255, seed, 30 + 70 * x, 15 + 60 * y + 15 * z);
    };
    // The renderer's own addressing and call order for one frame's patches.
    // stray: a cell written in the system memory volume with no AddDirtyBox.
    const auto patch = [&](bool alt, const Patch *patches, size_t count, const Patch *stray = nullptr) {
        IDirect3DVolumeTexture9 *texture = alt ? staging : managed;
        D3DLOCKED_BOX locked{};
        if (!Ok(texture->LockBox(0, &locked, nullptr, alt ? D3DLOCK_NO_DIRTY_UPDATE : 0), "LockBox"))
            return false;
        Equal("row_pitch", locked.RowPitch, kWidth * 4);
        Equal("slice_pitch", locked.SlicePitch, kWidth * 4 * kHeight);
        for (size_t i = 0; i < count; ++i)
        {
            const UINT x0 = kCell * (patches[i].cell & 0x3f);
            const UINT y0 = (patches[i].cell >> 4) & ~3u;
            if (alt)
            {
                D3DBOX dirty{};
                dirty.Left = x0;
                dirty.Right = x0 + kCell;
                dirty.Top = y0;
                dirty.Bottom = y0 + kCell;
                dirty.Front = 0;
                dirty.Back = kDepth;
                Ok(texture->AddDirtyBox(&dirty), "AddDirtyBox");
            }
            for (UINT z = 0; z < kDepth; ++z)
                for (UINT y = 0; y < kCell; ++y)
                    for (UINT x = 0; x < kCell; ++x)
                    {
                        const uint32_t value = texel(patches[i].seed, x, y, z);
                        std::memcpy(static_cast<uint8_t *>(locked.pBits) + z * locked.SlicePitch
                                        + (y0 + y) * locked.RowPitch + 4 * (x0 + x),
                                    &value, 4);
                    }
        }
        if (alt && stray)
        {
            const UINT x0 = kCell * (stray->cell & 0x3f);
            const UINT y0 = (stray->cell >> 4) & ~3u;
            for (UINT z = 0; z < kDepth; ++z)
                for (UINT y = 0; y < kCell; ++y)
                    for (UINT x = 0; x < kCell; ++x)
                    {
                        const uint32_t value = texel(stray->seed, x, y, z);
                        std::memcpy(static_cast<uint8_t *>(locked.pBits) + z * locked.SlicePitch
                                        + (y0 + y) * locked.RowPitch + 4 * (x0 + x),
                                    &value, 4);
                    }
        }
        if (!Ok(texture->UnlockBox(0), "UnlockBox"))
            return false;
        return !alt || Ok(device->UpdateTexture(staging, shown), "UpdateTexture");
    };
    // What one UpdateTexture uploaded: copies and bytes.
    const auto updateUploads = [&](const char *frame, const Patch *patches, size_t count, const Patch *stray,
                                   uint64_t wantBytes) {
        const front::DeviceCounters was = Counters();
        const bool done = patch(true, patches, count, stray);
        const front::DeviceCounters now = Counters();
        char name[64];
        std::snprintf(name, sizeof(name), "%s_update_uploads", frame);
        Equal(name, now.textureUploads - was.textureUploads, uint64_t(1));
        std::snprintf(name, sizeof(name), "%s_update_bytes", frame);
        Equal(name, now.textureUploadBytes - was.textureUploadBytes, wantBytes);
        return done;
    };
    constexpr uint64_t kVolumeBytes = uint64_t(kWidth) * kHeight * kDepth * 4;
    constexpr uint64_t kCellBytes = uint64_t(kCell) * kCell * kDepth * 4;
    // Draws the four watched cells from both volumes, one 16x16 block per
    // slice (each texel 4x4 pixels): the managed volume in columns 0 to 3, the
    // updated one in columns 4 to 7, a row per cell. Then checks every texel
    // of both against what the patches so far should have left.
    const auto drawAndExpect = [&](const char *frame, const Patch watched[4]) {
        Reset();
        device->SetPixelShader(psVolume);
        for (UINT path = 0; path < 2; ++path)
        {
            Ok(device->SetTexture(0, path ? static_cast<IDirect3DBaseTexture9 *>(shown) : managed), "SetTexture");
            for (UINT row = 0; row < 4; ++row)
            {
                const UINT x0 = kCell * (watched[row].cell & 0x3f);
                const UINT y0 = (watched[row].cell >> 4) & ~3u;
                for (UINT z = 0; z < kDepth; ++z)
                {
                    // The pixel shader takes the volume's third coordinate
                    // from the colour's red: the middle of slice z.
                    const uint32_t w = (2 * z + 1) * 255 / (2 * kDepth);
                    const float px = float((path * kDepth + z) * 16), py = float(row * 16);
                    Vertex quad[6];
                    QuadList(quad, px, py, px + 16, py + 16, 0.5f, Argb(255, w, 0, 0));
                    for (Vertex &vertex : quad)
                    {
                        vertex.u = (float(x0) + vertex.u * kCell) / kWidth;
                        vertex.v = (float(y0) + vertex.v * kCell) / kHeight;
                    }
                    Ok(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex)), "DrawPrimitiveUP");
                }
            }
        }
        device->SetTexture(0, nullptr);
        if (!Read())
            return;
        for (UINT path = 0; path < 2; ++path)
            for (UINT row = 0; row < 4; ++row)
                for (UINT z = 0; z < kDepth; ++z)
                    for (UINT y = 0; y < kCell; ++y)
                        for (UINT x = 0; x < kCell; ++x)
                        {
                            const uint32_t seed = watched[row].seed;
                            const uint32_t want = seed ? texel(seed, x, y, z) : 0;
                            char name[64];
                            std::snprintf(name, sizeof(name), "%s_%s_cell%u_%u%u%u", frame,
                                          path ? "updated" : "managed", watched[row].cell, x, y, z);
                            Expect(name, (path * kDepth + z) * 16 + x * 4 + 2, row * 16 + y * 4 + 2,
                                   int(want >> 16 & 0xff), int(want >> 8 & 0xff), int(want & 0xff));
                        }
    };

    const front::DeviceCounters before = Counters();
    // Frame 1: the first and the last cell of row 0.
    const Patch first[] = {{0, 60}, {63, 120}};
    // Frame 2: a cell of row 1, and cell 0 again. Cell 63 is not written and
    // must keep frame 1's texels; cell 70 is written only in the system memory
    // volume, with no AddDirtyBox, so it never reaches either drawn volume.
    const Patch second[] = {{69, 180}, {0, 240}};
    const Patch stray = {70, 99};
    bool ok = patch(false, first, 2) && updateUploads("frame1", first, 2, nullptr, kVolumeBytes);
    if (ok)
    {
        const Patch watched[4] = {{0, 60}, {63, 120}, {69, 0}, {70, 0}};
        drawAndExpect("frame1", watched);
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        ok = patch(false, second, 2) && updateUploads("frame2", second, 2, &stray, 2 * kCellBytes);
        // Nothing written since: nothing to copy.
        const front::DeviceCounters was = Counters();
        ok = ok && Ok(device->UpdateTexture(staging, shown), "UpdateTexture");
        Equal("clean_update_uploads", Counters().textureUploads - was.textureUploads, uint64_t(0));
    }
    if (ok)
    {
        const Patch watched[4] = {{0, 240}, {63, 120}, {69, 180}, {70, 0}};
        drawAndExpect("frame2", watched);
    }
    Equal("no_unsupported_calls", Counters().unsupportedCalls, before.unsupportedCalls);
    SafeRelease(shown);
    SafeRelease(staging);
    SafeRelease(managed);
}

// UpdateTexture copies only what is dirty, in every level, as D3D9 does: a
// 16x16 A8R8G8B8 texture of two levels and a 16x16 DXT1 texture, each from
// SYSTEMMEM into DEFAULT. A new texture is copied whole. Then texels written
// under D3DLOCK_NO_DIRTY_UPDATE reach the drawn texture only inside an
// AddDirtyRect, which reaches level 1 halved and a DXT1 texture grown to
// whole 4x4 blocks, and a lock without the flag marks its own region. Each
// level is drawn 1:1 (level 1 as an 8x8 quad, point mip filter) and read back.
void Check::DirtyUpdate()
{
    Reset();
    IDirect3DTexture9 *staging = nullptr, *shown = nullptr, *blockStaging = nullptr, *blockShown = nullptr;
    if (!Ok(device->CreateTexture(16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr),
            "CreateTexture(system memory)")
        || !Ok(device->CreateTexture(16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shown, nullptr),
               "CreateTexture(default)")
        || !Ok(device->CreateTexture(16, 16, 1, 0, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &blockStaging, nullptr),
               "CreateTexture(DXT1 system memory)")
        || !Ok(device->CreateTexture(16, 16, 1, 0, D3DFMT_DXT1, D3DPOOL_DEFAULT, &blockShown, nullptr),
               "CreateTexture(DXT1 default)"))
    {
        SafeRelease(blockStaging);
        SafeRelease(shown);
        SafeRelease(staging);
        return;
    }
    const uint32_t red = Argb(255, 255, 0, 0), green = Argb(255, 0, 255, 0), blue = Argb(255, 0, 0, 255),
                   white = Argb(255, 255, 255, 255), yellow = Argb(255, 255, 255, 0);
    // Writes value over [x0, x1) x [y0, y1) of a level, locked with flags and
    // an optional rect.
    const auto write = [&](UINT level, const RECT *rect, DWORD flags, UINT x0, UINT y0, UINT x1, UINT y1,
                           uint32_t value) {
        D3DLOCKED_RECT locked{};
        if (!Ok(staging->LockRect(level, &locked, rect, flags), "LockRect"))
            return;
        uint8_t *base = static_cast<uint8_t *>(locked.pBits);
        const UINT left = rect ? UINT(rect->left) : 0, top = rect ? UINT(rect->top) : 0;
        for (UINT y = y0; y < y1; ++y)
            for (UINT x = x0; x < x1; ++x)
                std::memcpy(base + (y - top) * locked.Pitch + 4 * (x - left), &value, 4);
        staging->UnlockRect(level);
    };
    // One DXT1 block of one colour (RGB565, every index 0).
    const auto block = [](uint16_t colour) {
        uint8_t bytes[8] = {uint8_t(colour), uint8_t(colour >> 8), uint8_t(colour), uint8_t(colour >> 8), 0, 0, 0, 0};
        uint64_t value = 0;
        std::memcpy(&value, bytes, 8);
        return value;
    };
    const auto writeBlocks = [&](DWORD flags, UINT bx0, UINT by0, UINT bx1, UINT by1, uint16_t colour) {
        D3DLOCKED_RECT locked{};
        if (!Ok(blockStaging->LockRect(0, &locked, nullptr, flags), "LockRect(DXT1)"))
            return;
        const uint64_t value = block(colour);
        for (UINT by = by0; by < by1; ++by)
            for (UINT bx = bx0; bx < bx1; ++bx)
                std::memcpy(static_cast<uint8_t *>(locked.pBits) + by * locked.Pitch + 8 * bx, &value, 8);
        blockStaging->UnlockRect(0);
    };
    const auto update = [&](const char *name, IDirect3DTexture9 *from, IDirect3DTexture9 *to, uint64_t uploads,
                            uint64_t bytes) {
        const front::DeviceCounters was = Counters();
        Ok(device->UpdateTexture(from, to), "UpdateTexture");
        const front::DeviceCounters now = Counters();
        char label[64];
        std::snprintf(label, sizeof(label), "%s_uploads", name);
        Equal(label, now.textureUploads - was.textureUploads, uploads);
        std::snprintf(label, sizeof(label), "%s_bytes", name);
        Equal(label, now.textureUploadBytes - was.textureUploadBytes, bytes);
    };
    // Level 0 at (0,0), level 1 at (32,0), the DXT1 texture at (64,0).
    const auto draw = [&]() {
        Reset();
        Ok(device->SetPixelShader(psTexture), "SetPixelShader");
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
        Ok(device->SetTexture(0, shown), "SetTexture");
        DrawQuad(0, 0, 16, 16, 0.5f, 0xffffffffu);
        DrawQuad(32, 0, 40, 8, 0.5f, 0xffffffffu);
        Ok(device->SetTexture(0, blockShown), "SetTexture(DXT1)");
        DrawQuad(64, 0, 80, 16, 0.5f, 0xffffffffu);
        device->SetTexture(0, nullptr);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        return Read();
    };

    // New: everything is copied.
    write(0, nullptr, 0, 0, 0, 16, 16, red);
    write(1, nullptr, 0, 0, 0, 8, 8, green);
    update("new", staging, shown, 2, 16 * 16 * 4 + 8 * 8 * 4);
    writeBlocks(0, 0, 0, 4, 4, 0xf800);
    update("new_dxt1", blockStaging, blockShown, 1, 16 * 8);

    // Marked: blue inside the rect on both levels, white outside it.
    write(0, nullptr, D3DLOCK_NO_DIRTY_UPDATE, 8, 8, 12, 12, blue);
    write(0, nullptr, D3DLOCK_NO_DIRTY_UPDATE, 0, 0, 4, 4, white);
    write(1, nullptr, D3DLOCK_NO_DIRTY_UPDATE, 4, 4, 6, 6, blue);
    write(1, nullptr, D3DLOCK_NO_DIRTY_UPDATE, 0, 0, 2, 2, white);
    const RECT dirty = {8, 8, 12, 12};
    Ok(staging->AddDirtyRect(&dirty), "AddDirtyRect");
    update("marked", staging, shown, 2, 4 * 4 * 4 + 2 * 2 * 4);
    // A rect inside one block grows to the block: green there, a stray green
    // block outside.
    writeBlocks(D3DLOCK_NO_DIRTY_UPDATE, 1, 1, 2, 2, 0x07e0);
    writeBlocks(D3DLOCK_NO_DIRTY_UPDATE, 2, 2, 3, 3, 0x07e0);
    const RECT inBlock = {5, 5, 7, 7};
    Ok(blockStaging->AddDirtyRect(&inBlock), "AddDirtyRect(DXT1)");
    update("marked_dxt1", blockStaging, blockShown, 1, 8);
    // Nothing dirty since: nothing copied.
    update("clean", staging, shown, 0, 0);

    // A lock without the flag marks its rect, on level 0 and halved on level 1.
    const RECT corner = {12, 0, 16, 4};
    write(0, &corner, 0, 12, 0, 16, 4, yellow);
    update("locked", staging, shown, 2, 4 * 4 * 4 + 2 * 2 * 4);

    if (draw())
    {
        Expect("level0_marked", 9, 9, 0, 0, 255);
        Expect("level0_unmarked", 1, 1, 255, 0, 0);
        Expect("level0_untouched", 14, 14, 255, 0, 0);
        Expect("level0_locked", 13, 1, 255, 255, 0);
        Expect("level1_marked", 32 + 4, 4, 0, 0, 255);
        Expect("level1_unmarked", 32 + 0, 0, 0, 255, 0);
        Expect("level1_untouched", 32 + 7, 7, 0, 255, 0);
        Expect("dxt1_marked_block", 64 + 4, 4, 0, 255, 0);
        Expect("dxt1_marked_block_edge", 64 + 7, 7, 0, 255, 0);
        Expect("dxt1_unmarked_block", 64 + 9, 9, 255, 0, 0);
    }
    SafeRelease(blockShown);
    SafeRelease(blockStaging);
    SafeRelease(shown);
    SafeRelease(staging);
}

// D3D9's reference counts: levels share their texture's count, objects the
// device has bound outlive the program's last reference, and the back buffer
// and the implicit swap chain have counts of their own that hold the device.
void Check::References()
{
    Reset();
    const uint32_t liveBefore = Counters().liveObjects;
    IDirect3DTexture9 *texture = FilledTexture(4, 4, [](UINT, UINT) { return Argb(255, 255, 255, 255); });
    if (!texture)
        return;
    Equal("live_after_create", Counters().liveObjects, liveBefore + 1);
    Equal("addref", texture->AddRef(), 2ul);
    Equal("release", texture->Release(), 1ul);
    IDirect3DSurface9 *level = nullptr;
    Ok(texture->GetSurfaceLevel(0, &level), "GetSurfaceLevel");
    Equal("level_counts_texture", texture->AddRef(), 3ul);
    texture->Release();
    IDirect3DTexture9 *container = nullptr;
    Ok(level->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&container)), "GetContainer");
    Equal("container", container == texture, true);
    SafeRelease(container);
    IUnknown *unknown = nullptr;
    Ok(texture->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(&unknown)), "QueryInterface(IUnknown)");
    Equal("query_unknown", unknown == static_cast<IUnknown *>(texture), true);
    SafeRelease(unknown);
    Equal("level_release", level->Release(), 1ul);
    level = nullptr;

    // Bound, then released by the program: it still draws, and goes when unbound.
    Ok(device->SetTexture(0, texture), "SetTexture");
    Equal("last_release", texture->Release(), 0ul);
    Equal("bound_alive", Counters().liveObjects, liveBefore + 1);
    device->SetPixelShader(psTexture);
    DrawQuad(0, 0, 64, 64, 0.5f, 0xffffffffu);
    device->SetTexture(0, nullptr);
    Equal("unbound_gone", Counters().liveObjects, liveBefore);
    if (Read())
        Expect("released_but_bound", 32, 32, 255, 255, 255);

    // The back buffer and the implicit swap chain have counts of their own,
    // and hold the device while the program holds them. The engine asserts
    // their last Release returns 0 before every reset and at shutdown.
    const auto deviceRefs = [this]() {
        const ULONG refs = device->AddRef() - 1;
        device->Release();
        return refs;
    };
    // The check holds the back buffer (backBuffer) from its setup on.
    const ULONG refsHeld = deviceRefs();
    IDirect3DSurface9 *surface = nullptr;
    Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface), "GetBackBuffer");
    Equal("back_buffer_same", surface == backBuffer, true);
    Equal("back_buffer_holds_device_once", deviceRefs(), refsHeld);
    Equal("back_buffer_addref", surface->AddRef(), 3ul);
    Equal("back_buffer_release", surface->Release(), 2ul);
    Equal("back_buffer_release_again", surface->Release(), 1ul);
    // The check's own last reference, then taken again for the scenes after.
    Equal("back_buffer_last_release", backBuffer->Release(), 0ul);
    backBuffer = nullptr;
    const ULONG refsReleased = deviceRefs();
    Equal("back_buffer_lets_device_go", refsReleased, refsHeld - 1);
    Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer again");
    Equal("back_buffer_holds_device", deviceRefs(), refsHeld);
    IDirect3DSwapChain9 *chain = nullptr;
    Ok(device->GetSwapChain(0, &chain), "GetSwapChain");
    Equal("swap_chain_holds_device", deviceRefs(), refsHeld + 1);
    Equal("swap_chain_last_release", chain->Release(), 0ul);
    Equal("swap_chain_lets_device_go", deviceRefs(), refsHeld);
    // Handed out again after the last release, as before a reset.
    Ok(device->GetSwapChain(0, &chain), "GetSwapChain again");
    Equal("swap_chain_again", chain->Release(), 0ul);
    IDirect3DDevice9 *owner = nullptr;
    Ok(decl->GetDevice(&owner), "GetDevice");
    Equal("get_device", owner == device, true);
    SafeRelease(owner);
}

// The renderer's threads. The engine creates its device with
// D3DCREATE_MULTITHREADED and, during a map load, its database thread creates
// and fills textures while the render thread draws. Here a worker does the
// same (create, lock, fill, unlock, release, with the last one kept) while
// this thread draws and presents; then the kept texture is drawn and read.
void Check::Threads()
{
    constexpr int kTextures = 200;
    constexpr UINT kEdge = 16;
    std::atomic<int> made{0};
    std::atomic<bool> failed{false};
    IDirect3DTexture9 *kept = nullptr;
    const uint32_t liveBefore = Counters().liveObjects;
    IDirect3DDevice9 *const shared = device;
    std::thread worker([&] {
        for (int i = 0; i < kTextures; ++i)
        {
            IDirect3DTexture9 *texture = nullptr;
            D3DLOCKED_RECT locked{};
            if (FAILED(shared->CreateTexture(kEdge, kEdge, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture,
                                             nullptr)))
            {
                failed = true;
                return;
            }
            if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
            {
                texture->Release();
                failed = true;
                return;
            }
            const uint32_t value = Argb(255, uint32_t(i) & 0xff, 0x80, 0x40);
            for (UINT y = 0; y < kEdge; ++y)
                for (UINT x = 0; x < kEdge; ++x)
                    std::memcpy(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch + 4 * x, &value, 4);
            texture->UnlockRect(0);
            if (i == kTextures - 1)
                kept = texture;
            else
                texture->Release();
            ++made;
        }
    });
    int frames = 0;
    do
    {
        Reset(Argb(255, 0, 0, uint32_t(frames) & 0xff));
        DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        ++frames;
    } while (made.load() < kTextures && !failed.load());
    worker.join();
    Equal("worker_failed", failed.load(), false);
    Equal("worker_textures", made.load(), kTextures);
    Equal("frames_meanwhile", frames > 0, true);
    Equal("live_after_worker", Counters().liveObjects, liveBefore + (kept ? 1 : 0));
    if (!kept)
        return;
    Reset();
    device->SetPixelShader(psTexture);
    Ok(device->SetTexture(0, kept), "SetTexture");
    DrawQuad(0, 0, 64, 64, 0.5f, 0xffffffffu);
    device->SetTexture(0, nullptr);
    if (Read())
        Expect("worker_texture", 32, 32, (kTextures - 1) & 0xff, 0x80, 0x40);
    Equal("kept_release", kept->Release(), 0ul);
    Equal("live_at_end", Counters().liveObjects, liveBefore);
}

// Event and occlusion queries.
void Check::Queries()
{
    Reset();
    Equal("event_supported", device->CreateQuery(D3DQUERYTYPE_EVENT, nullptr), D3D_OK);
    Equal("timestamp_refused", device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, nullptr), D3DERR_NOTAVAILABLE);
    IDirect3DQuery9 *occlusion = nullptr, *event = nullptr;
    if (!Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion), "CreateQuery(occlusion)")
        || !Ok(device->CreateQuery(D3DQUERYTYPE_EVENT, &event), "CreateQuery(event)"))
    {
        SafeRelease(occlusion);
        return;
    }
    Ok(occlusion->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    Ok(occlusion->Issue(D3DISSUE_END), "Issue(end)");
    Ok(event->Issue(D3DISSUE_END), "Issue(event)");
    BOOL done = FALSE;
    HRESULT hr = S_FALSE;
    int polls = 0;
    while ((hr = event->GetData(&done, sizeof(done), D3DGETDATA_FLUSH)) == S_FALSE && polls < 100000)
    {
        ++polls;
        SDL_Delay(polls > 100 ? 1 : 0);
    }
    Equal("event_done", hr == S_OK && done == TRUE, true);
    DWORD samples = 0;
    while ((hr = occlusion->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH)) == S_FALSE && polls < 200000)
        ++polls;
    Equal("occlusion_done", hr == S_OK && samples > 0, true);
    Equal("event_size", event->GetDataSize(), static_cast<DWORD>(sizeof(BOOL)));
    std::printf("d3d9_check.event_query_polls=%d\n", polls);

    // The engine's swap fence: an event issued right after Present marks the
    // end of that frame, so it finishes with the frame's submission, without a
    // flush and without waiting for the next frame's work (RB_BackendTimeout
    // polls it before every frame ends).
    DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    Ok(event->Issue(D3DISSUE_END), "Issue(event after Present)");
    DrawQuad(64, 64, 64, 64, 0.5f, Argb(255, 0, 0, 255)); // the next frame's work, not submitted
    int fencePolls = 0;
    done = FALSE;
    while ((hr = event->GetData(&done, sizeof(done), 0)) == S_FALSE && fencePolls < 5000)
    {
        ++fencePolls;
        SDL_Delay(1);
    }
    Equal("swap_fence_done_without_flush", hr == S_OK && done == TRUE, true);
    std::printf("d3d9_check.swap_fence_polls=%d\n", fencePolls);

    // Polled with FLUSH, as the engine polls it once the next frame is drawn:
    // it still finishes with its own frame, and the first poll that finds it
    // unfinished hands the next frame's work to the GPU, once, ahead of its
    // Present. The frame then goes on in a new submission with its state and
    // its target's contents. A heavy frame first, so that the poll finds the
    // fence unfinished; a GPU that still finishes first gets another try.
    // The frame stays under the GPU feed's 128 draws, so it reaches the GPU
    // whole at its Present and is still at work when Present returns. Present
    // must not wait for it: it once did on lavapipe, which blocks a query read
    // while its queue runs a submission, when it read the frame before's
    // timestamps from the pool (they are copied on the GPU now).
    const char *fencePath = "finished_first";
    unsigned long long fenceFlushes = 0;
    for (int attempt = 0; attempt < 8 && fencePath[0] == 'f'; ++attempt)
    {
        Reset();
        for (int i = 0; i < 112; ++i)
            DrawQuad(0, 0, kSize, kSize, 0.5f, Argb(255, 32, 32, 32));
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        Ok(event->Issue(D3DISSUE_END), "Issue(event after Present)");
        Reset(Argb(255, 0, 64, 0));
        DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0)); // the next frame's work so far
        const uint64_t before = Counters().fenceFlushes;
        done = FALSE;
        if (event->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) != S_FALSE)
            continue;
        fencePath = "ahead";
        const uint64_t afterFirst = Counters().fenceFlushes;
        int waits = 0;
        while ((hr = event->GetData(&done, sizeof(done), D3DGETDATA_FLUSH)) == S_FALSE && waits < 5000)
        {
            ++waits;
            SDL_Delay(1);
        }
        Equal("swap_fence_done_after_flush", hr == S_OK && done == TRUE, true);
        Equal("swap_fence_flushes_once", Counters().fenceFlushes, afterFirst);
        fenceFlushes = afterFirst - before;
        DrawQuad(64, 64, 96, 96, 0.5f, Argb(255, 0, 0, 255));
        if (Read())
        {
            Expect("fence_flush_drawn_before", 16, 16, 255, 0, 0);
            Expect("fence_flush_drawn_after", 80, 80, 0, 0, 255);
            Expect("fence_flush_clear", 120, 16, 0, 64, 0);
        }
    }
    std::printf("d3d9_check.fence_flush=%s flushes=%llu\n", fencePath, fenceFlushes);
    SafeRelease(event);
    SafeRelease(occlusion);
}

// Occlusion queries count the samples their draws put on the target: the
// engine's sun flare divides one query's count by another's
// (RB_DrawSunQuerySprite, RB_CalcSunSpriteSamples), so the numbers must be
// exact. A quad of w x h pixels is w * h samples (no multisampling here).
// The queries here are cut by everything that ends a Vulkan pass, a query
// there being unable to outlive its pass: other queries, target changes,
// Present, and the GPU feed, which with KISAK_VK_GPU_FEED=always submits
// after every draw. Where the device cannot count, every finished query
// reports 0xffffff and the scene checks that instead.
void Check::Occlusion()
{
    constexpr DWORD kEverySample = 0xffffff;
    const front::DeviceCounters before = Counters();
    const bool counted = before.occlusionCounted;
    IDirect3DQuery9 *a = nullptr, *b = nullptr;
    if (!Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &a), "CreateQuery(occlusion)")
        || !Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &b), "CreateQuery(occlusion)"))
    {
        SafeRelease(a);
        return;
    }
    // Polls as the engine does (RB_HW_ReadOcclusionQuery).
    auto read = [&](IDirect3DQuery9 *query, DWORD &samples) {
        HRESULT hr = S_FALSE;
        for (int polls = 0; polls < 200000; ++polls)
        {
            hr = query->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH);
            if (hr != S_FALSE)
                break;
            SDL_Delay(polls > 100 ? 1 : 0);
        }
        return hr == S_OK;
    };
    auto expect = [&](const char *name, IDirect3DQuery9 *query, DWORD want) {
        DWORD samples = 0;
        if (!read(query, samples))
            Fail(std::string(name) + ": the query did not finish");
        else
            Equal(name, samples, counted ? want : kEverySample);
    };
    Reset();

    // Never issued: done, with nothing counted.
    DWORD samples = 12345;
    Equal("occlusion_unissued_done", a->GetData(&samples, sizeof(samples), 0), S_OK);
    Equal("occlusion_unissued", samples, counted ? 0u : kEverySample);

    // One quad. Between BEGIN and END a query has no result.
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    if (counted)
        Equal("occlusion_open_has_no_result", a->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH), S_FALSE);
    DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_quad", a, 64 * 64);
    // Asked again, the same answer.
    expect("occlusion_quad_again", a, 64 * 64);

    // Two draws, the second on top of the first: both count.
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
    DrawQuad(16, 16, 32, 32, 0.5f, Argb(255, 0, 255, 0));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_two_draws", a, 32 * 32 + 16 * 16);

    // No draw, and a clear, which is not one. The statistics take it as one
    // result with no sample (what the engine gets while the sun's place on
    // screen is behind a wall), and a second read of it as nothing new.
    const front::DeviceCounters beforeEmpty = Counters();
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    Ok(device->Clear(0, nullptr, D3DCLEAR_TARGET, Argb(255, 0, 0, 64), 1.0f, 0), "Clear");
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_no_draw", a, 0);
    expect("occlusion_no_draw_again", a, 0);
    const front::DeviceCounters afterEmpty = Counters();
    Equal("occlusion_empty_result", afterEmpty.occlusionResults - beforeEmpty.occlusionResults,
          uint64_t(counted ? 1 : 0));
    Equal("occlusion_empty_result_zero", afterEmpty.occlusionResultsZero - beforeEmpty.occlusionResultsZero,
          uint64_t(counted ? 1 : 0));
    Equal("occlusion_empty_result_samples", afterEmpty.occlusionSamples - beforeEmpty.occlusionSamples, uint64_t(0));

    // The scissor rectangle cuts samples off.
    const RECT scissor = {0, 0, 16, LONG(kSize)};
    Ok(device->SetScissorRect(&scissor), "SetScissorRect");
    Ok(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "SetRenderState(scissor)");
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 0));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    Ok(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE), "SetRenderState(scissor)");
    expect("occlusion_scissor", a, 16 * 64);

    // Begun again before it was read: only the second count stands. And an
    // END that no BEGIN came before counts nothing.
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 16, 16, 0.5f, Argb(255, 255, 255, 255));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_begun_again", a, 16 * 16);
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_end_alone", a, 0);

    // Two queries that overlap: the middle draw counts for both.
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
    Ok(b->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 16, 16, 0.5f, Argb(255, 0, 255, 0));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    DrawQuad(0, 0, 8, 8, 0.5f, Argb(255, 0, 0, 255));
    Ok(b->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_overlap_first", a, 32 * 32 + 16 * 16);
    expect("occlusion_overlap_second", b, 16 * 16 + 8 * 8);

    // Depth: a far quad half behind a near one, and one wholly behind it.
    IDirect3DSurface9 *depth = nullptr;
    if (Ok(device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr),
           "CreateDepthStencilSurface"))
    {
        Ok(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
        Ok(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0), "Clear(depth)");
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        DrawQuad(0, 0, 64, kSize, 0.25f, Argb(255, 255, 0, 0)); // near: the left half
        Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(32, 0, 96, 64, 0.75f, Argb(255, 0, 255, 0)); // far: its right half shows
        Ok(a->Issue(D3DISSUE_END), "Issue(end)");
        Ok(b->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 64, 64, kSize, 0.75f, Argb(255, 0, 0, 255)); // far: hidden
        Ok(b->Issue(D3DISSUE_END), "Issue(end)");
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        Ok(device->SetDepthStencilSurface(nullptr), "SetDepthStencilSurface(null)");
        expect("occlusion_half_hidden", a, 32 * 64);
        expect("occlusion_hidden", b, 0);
        if (Read())
        {
            Expect("occlusion_near_kept", 48, 32, 255, 0, 0);
            Expect("occlusion_far_shown", 80, 32, 0, 255, 0);
            Expect("occlusion_far_hidden", 32, 96, 255, 0, 0);
        }
    }
    SafeRelease(depth);

    // A query over two targets, and over a Present.
    IDirect3DTexture9 *texture = nullptr;
    IDirect3DSurface9 *level = nullptr;
    if (Ok(device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr),
           "CreateTexture(render target)")
        && Ok(texture->GetSurfaceLevel(0, &level), "GetSurfaceLevel"))
    {
        Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
        Ok(device->SetRenderTarget(0, level), "SetRenderTarget(level)");
        DrawQuad(0, 0, 16, 16, 0.5f, Argb(255, 0, 255, 0), 64);
        Ok(device->SetRenderTarget(0, backBuffer), "SetRenderTarget(back buffer)");
        DrawQuad(0, 0, 8, 8, 0.5f, Argb(255, 0, 0, 255));
        Ok(a->Issue(D3DISSUE_END), "Issue(end)");
        expect("occlusion_two_targets", a, 32 * 32 + 16 * 16 + 8 * 8);
    }
    SafeRelease(level);
    SafeRelease(texture);
    Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
    DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    DrawQuad(0, 0, 16, 16, 0.5f, Argb(255, 0, 255, 0));
    Ok(a->Issue(D3DISSUE_END), "Issue(end)");
    expect("occlusion_two_frames", a, 32 * 32 + 16 * 16);

    // A query released between its BEGIN and its END takes nothing with it.
    IDirect3DQuery9 *dropped = nullptr;
    if (Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &dropped), "CreateQuery(occlusion)"))
    {
        Ok(dropped->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
        Equal("occlusion_dropped_release", dropped->Release(), 0ul);
        Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 0, 8, 8, 0.5f, Argb(255, 0, 0, 255));
        Ok(a->Issue(D3DISSUE_END), "Issue(end)");
        expect("occlusion_after_dropped", a, 8 * 8);
    }

    // More queries held at once than one pool of Vulkan queries has: a
    // second pool is made, and every count is still its own.
    constexpr size_t kHeld = 300;
    std::vector<IDirect3DQuery9 *> held(kHeld, nullptr);
    auto heldWidth = [](size_t i) { return DWORD(1 + i % 8); };
    for (size_t i = 0; i < kHeld; ++i)
    {
        if (!Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &held[i]), "CreateQuery(occlusion)"))
            break;
        Ok(held[i]->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 0, float(heldWidth(i)), 8, 0.5f, Argb(255, 255, 255, 255));
        Ok(held[i]->Issue(D3DISSUE_END), "Issue(end)");
    }
    int heldWrong = 0;
    for (size_t i = 0; i < kHeld; ++i)
    {
        samples = 0;
        if (!held[i] || !read(held[i], samples) || samples != (counted ? heldWidth(i) * 8 : kEverySample))
            ++heldWrong;
        SafeRelease(held[i]);
    }
    Equal("occlusion_held_wrong", heldWrong, 0);
    const front::DeviceCounters afterHeld = Counters();
    if (counted)
        Equal("occlusion_second_pool", afterHeld.occlusionPools, 2u);

    // One query used over and over, read each time, as the engine uses its
    // own: the Vulkan queries it gives up are used again once the GPU has
    // finished with them, and no pool is added.
    constexpr int kRounds = 400;
    int roundsWrong = 0;
    for (int i = 0; i < kRounds; ++i)
    {
        Ok(a->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(0, 0, 8, 8, 0.5f, Argb(255, 255, 255, 255));
        Ok(a->Issue(D3DISSUE_END), "Issue(end)");
        samples = 0;
        if (!read(a, samples) || samples != (counted ? 64u : kEverySample))
            ++roundsWrong;
    }
    Equal("occlusion_rounds_wrong", roundsWrong, 0);
    const front::DeviceCounters after = Counters();
    Equal("occlusion_pools_kept", after.occlusionPools, afterHeld.occlusionPools);
    if (!counted)
        Equal("occlusion_nothing_counted", after.occlusionCounts, 0u);
    // The statistics of those rounds: a result each, 64 samples each, none
    // empty. A device that cannot count has no results to report.
    Equal("occlusion_rounds_results", after.occlusionResults - afterHeld.occlusionResults,
          uint64_t(counted ? kRounds : 0));
    Equal("occlusion_rounds_samples", after.occlusionSamples - afterHeld.occlusionSamples,
          uint64_t(counted ? kRounds * 64 : 0));
    Equal("occlusion_rounds_none_empty", after.occlusionResultsZero - afterHeld.occlusionResultsZero, uint64_t(0));
    SafeRelease(b);
    SafeRelease(a);
    std::printf("d3d9_check.occlusion=counted=%s queries=%d counts=%llu pools=%u results=%llu empty=%llu "
                "samples=%llu\n",
                counted ? "yes" : "no", int(kHeld) + kRounds,
                static_cast<unsigned long long>(after.occlusionCounts - before.occlusionCounts), after.occlusionPools,
                static_cast<unsigned long long>(after.occlusionResults - before.occlusionResults),
                static_cast<unsigned long long>(after.occlusionResultsZero - before.occlusionResultsZero),
                static_cast<unsigned long long>(after.occlusionSamples - before.occlusionSamples));
}

// The engine's frame, as its render thread runs it (RB_RenderCommandFrame,
// RB_EndFrame, RB_SwapBuffers): draw; poll the swap fence with FLUSH until the
// frame before has finished; issue the GPU sync fence; Present; issue the swap
// fence. Meanwhile another thread polls the GPU sync fence with FLUSH, as the
// main thread does in R_SyncGpu. Every frame takes at most three submissions
// (the swap fence's flush or the sync fence's own, and the present), the swap
// fence flushes at most once a frame, and the last frame comes out whole,
// drawn on both sides of its wait.
void Check::FrameLoop()
{
    constexpr int kFrames = 48;
    IDirect3DQuery9 *swapFence = nullptr, *syncFence = nullptr;
    if (!Ok(device->CreateQuery(D3DQUERYTYPE_EVENT, &swapFence), "CreateQuery(swap fence)")
        || !Ok(device->CreateQuery(D3DQUERYTYPE_EVENT, &syncFence), "CreateQuery(sync fence)"))
    {
        SafeRelease(swapFence);
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<bool> pollFailed{false};
    IDirect3DQuery9 *const polled = syncFence;
    std::thread mainThread([&] {
        while (!stop.load())
        {
            BOOL done = FALSE;
            if (FAILED(polled->GetData(&done, sizeof(done), D3DGETDATA_FLUSH)))
                pollFailed = true;
            std::this_thread::yield();
        }
    });
    const front::DeviceCounters before = Counters();
    int stuck = 0;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        const bool last = frame == kFrames - 1;
        Reset(Argb(255, 0, 64, 0));
        for (int i = 0; i < 24; ++i)
            DrawQuad(0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
        BOOL done = FALSE;
        int waits = 0;
        while (swapFence->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE && waits < 20000)
        {
            ++waits;
            if (waits > 200)
                SDL_Delay(1);
            else
                std::this_thread::yield();
        }
        stuck += waits >= 20000;
        Ok(syncFence->Issue(D3DISSUE_END), "Issue(sync fence)");
        if (last)
        {
            DrawQuad(64, 64, 96, 96, 0.5f, Argb(255, 0, 0, 255));
            if (Read())
            {
                Expect("frame_drawn_before_wait", 16, 16, 255, 0, 0);
                Expect("frame_drawn_after_wait", 80, 80, 0, 0, 255);
                Expect("frame_clear", 120, 16, 0, 64, 0);
            }
        }
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        Ok(swapFence->Issue(D3DISSUE_END), "Issue(swap fence)");
    }
    stop = true;
    mainThread.join();
    const front::DeviceCounters after = Counters();
    const uint64_t presents = after.presents - before.presents;
    // Parts submitted to feed the GPU are apart (none in frames this small,
    // unless KISAK_VK_GPU_FEED=always submits after every draw).
    const uint64_t submissions = after.submissions - before.submissions - (after.feedFlushes - before.feedFlushes);
    const uint64_t flushes = after.fenceFlushes - before.fenceFlushes;
    Equal("frames_presented", presents + (after.presentsSkipped - before.presentsSkipped), uint64_t(kFrames));
    Equal("swap_fence_never_stuck", stuck, 0);
    Equal("sync_fence_polls_ok", pollFailed.load(), false);
    // The read-back of the last frame is one more.
    Equal("at_most_three_submissions_a_frame", submissions <= 3u * kFrames + 1, true);
    Equal("swap_fence_flushes_at_most_once_a_frame", flushes <= uint64_t(kFrames), true);
    std::printf("d3d9_check.frame_loop=frames=%d submissions=%llu fence_flushes=%llu\n", kFrames,
                static_cast<unsigned long long>(submissions), static_cast<unsigned long long>(flushes));
    SafeRelease(swapFence);
    SafeRelease(syncFence);
}

// A frame of many draws, as the engine's are. The adapter submits such a
// frame in parts while it is drawn (OptionalFeatures::gpuFeed), each part in
// a command buffer of its own with a pass of its own on the same target.
// Every draw fills a cell of its own here, so the picture shows each one,
// whichever part it fell in; the cells no draw touches show that no part
// began by clearing; and the scissor rectangle, set once before the first
// draw, still cuts the last column of cells from the draws of every part.
// How many parts there were is printed, for the test that knows the setting.
void Check::GpuFeed()
{
    constexpr int kDraws = 600;
    constexpr int kCell = 4;
    constexpr int kAcross = int(kSize) / kCell;
    static_assert(kDraws < kAcross * kAcross, "a cell for every draw, and some left clear");
    auto color = [](int draw) { return Argb(255, uint32_t(40 + draw % 200), uint32_t(draw / 3 % 256), 255); };
    Reset(Argb(255, 0, 64, 0));
    const RECT scissor = {0, 0, LONG(kSize) - kCell, LONG(kSize)};
    Ok(device->SetScissorRect(&scissor), "SetScissorRect");
    Ok(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "SetRenderState(scissor)");
    const front::DeviceCounters before = Counters();
    for (int draw = 0; draw < kDraws; ++draw)
    {
        const float x = float(draw % kAcross * kCell);
        const float y = float(draw / kAcross * kCell);
        DrawQuad(x, y, x + kCell, y + kCell, 0.5f, color(draw));
    }
    const front::DeviceCounters after = Counters();
    Ok(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE), "SetRenderState(scissor)");
    if (Read())
    {
        for (int draw : {0, 1, 126, 127, 128, 129, 254, 255, 256, 257, 300, 383, 384, 510, 511, 512, 598, 599})
        {
            const UINT x = UINT(draw % kAcross * kCell + kCell / 2);
            const UINT y = UINT(draw / kAcross * kCell + kCell / 2);
            char name[32];
            std::snprintf(name, sizeof(name), "feed_draw_%d", draw);
            if (draw % kAcross == kAcross - 1)
                Expect(name, x, y, 0, 64, 0); // cut by the scissor rectangle
            else
                Expect(name, x, y, 40 + draw % 200, (draw / 3) % 256, 255);
        }
        Expect("feed_clear_kept", 2, kSize - 2, 0, 64, 0);
        Expect("feed_clear_kept_right", kSize - 6, kSize - 2, 0, 64, 0);
    }
    const uint64_t parts = after.feedFlushes - before.feedFlushes;
    Equal("feed_at_most_a_part_a_draw", parts <= uint64_t(kDraws), true);
    std::printf("d3d9_check.gpu_feed=draws=%d parts=%llu\n", kDraws, static_cast<unsigned long long>(parts));
}

// What a program changes and asks for while its device exists: the GPU feed
// (front::Tuning, taken up at every Present; the engine's r_vkGpuFeed dvars)
// and the frame statistics (front::LogDeviceStats; the engine's r_vkStats).
// Each setting draws a frame of 600 draws and is known by its parts: none
// with the feed off, one for every draw with "always", two to twelve at 50
// draws a part (a GPU that stays behind gets a part about every 200 draws).
// Under the idle rule the first part goes after 10 draws, because the frame
// starts with the GPU read back and so with nothing left, and the rule sends
// five such parts at most (10, 20, 40, 80 and 160 draws).
// Every change is logged, the last one back to the device's own setting. The
// statistics, started again before the first setting, then report exactly
// the frames, draws and parts counted here; a second report repeats them and
// starts again, and a third has no frame in it.
void Check::Tuning()
{
    constexpr int kDraws = 600;
    constexpr int kCell = 4;
    constexpr int kAcross = int(kSize) / kCell;
    std::vector<std::string> lines;
    const auto keep = [&](std::vector<std::string> *kept) {
        const std::lock_guard<std::mutex> lock(host->mutex);
        host->logKept = kept;
    };
    // How many of the kept lines, or of the first `limit` of them, have the text.
    const auto logged = [&](const char *text, size_t limit = SIZE_MAX) {
        const std::lock_guard<std::mutex> lock(host->mutex);
        size_t count = 0;
        for (size_t i = 0; i < lines.size() && i < limit; ++i)
            count += lines[i].find(text) != std::string::npos;
        return count;
    };
    const auto set = [&](const front::Tuning &tuning) {
        const std::lock_guard<std::mutex> lock(host->mutex);
        host->tuning = tuning;
    };
    keep(&lines);
    Equal("stats_devices", front::LogDeviceStats(true), 1u);
    const front::DeviceCounters start = Counters();
    uint64_t draws = 0;

    struct Parts
    {
        uint64_t all, early;
    };
    // The Present takes the setting up; the frame's draws then count from a
    // GPU that has finished everything (Read waits for it).
    const auto frame = [&](const front::Tuning &tuning) {
        set(tuning);
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        Reset(Argb(255, 0, 64, 0));
        Read();
        const front::DeviceCounters before = Counters();
        for (int draw = 0; draw < kDraws; ++draw)
        {
            const float x = float(draw % kAcross * kCell);
            const float y = float(draw / kAcross * kCell);
            DrawQuad(x, y, x + kCell, y + kCell, 0.5f,
                     Argb(255, uint32_t(40 + draw % 200), uint32_t(draw / 3 % 256), 255));
        }
        draws += kDraws;
        const front::DeviceCounters after = Counters();
        return Parts{after.feedFlushes - before.feedFlushes, after.feedIdleFlushes - before.feedIdleFlushes};
    };
    const auto setting = [](int feed, uint32_t feedDraws, uint32_t idleDraws) {
        front::Tuning tuning;
        tuning.gpuFeed = feed;
        tuning.gpuFeedDraws = feedDraws;
        tuning.gpuFeedIdleDraws = idleDraws;
        return tuning;
    };

    Parts parts = frame(setting(0, 0, 0));
    Equal("feed_off_parts", parts.all, uint64_t(0));
    parts = frame(setting(2, 0, 0));
    Equal("feed_always_parts", parts.all, uint64_t(kDraws));
    Equal("feed_always_early_parts", parts.early, uint64_t(0));
    parts = frame(setting(1, 50, 0));
    Equal("feed_50_parts", parts.all >= 2 && parts.all <= 12, true);
    Equal("feed_50_early_parts", parts.early, uint64_t(0));
    parts = frame(setting(1, 300, 10));
    Equal("feed_idle_early_parts", parts.early >= 1 && parts.early <= 5, true);
    Equal("feed_idle_other_parts", parts.all >= parts.early && parts.all - parts.early <= 2, true);
    const Parts idle = parts;
    // The picture of a frame cut by the rule: the draws on both sides of its
    // first parts, the last draw, and the cells no draw touched.
    if (Read())
    {
        for (int draw : {0, 9, 10, 11, 29, 30, 31, 69, 70, 71, 300, 599})
        {
            char name[32];
            std::snprintf(name, sizeof(name), "idle_draw_%d", draw);
            Expect(name, UINT(draw % kAcross * kCell + kCell / 2), UINT(draw / kAcross * kCell + kCell / 2),
                   40 + draw % 200, (draw / 3) % 256, 255);
        }
        Expect("idle_clear_kept", kSize - 6, kSize - 2, 0, 64, 0);
    }
    // No rule where its part would be no smaller than the plain one.
    parts = frame(setting(1, 50, 80));
    Equal("feed_idle_too_large_parts", parts.all >= 2 && parts.all <= 12, true);
    Equal("feed_idle_too_large_early_parts", parts.early, uint64_t(0));
    // The device's own setting again, with nothing drawn under it.
    const size_t settingsLogged = logged(""); // every line so far
    set(front::Tuning{});
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    const front::DeviceCounters end = Counters();

    // Every change was logged as it was taken up: four for certain and a
    // fifth before them unless the device began with the feed off, then the
    // one back to the device's own setting, whatever that is. At 50 draws
    // twice: the too-large idle count is no rule and is not worded.
    const size_t changes = logged("GPU feed now ", settingsLogged);
    Equal("logged_changes", changes == 4 || changes == 5, true);
    Equal("logged_always", logged("GPU feed now after every draw", settingsLogged), size_t(1));
    Equal("logged_idle",
          logged("GPU feed now every 300 draws (10 when the GPU has nothing left)", settingsLogged), size_t(1));
    Equal("logged_50", logged("GPU feed now every 50 draws", settingsLogged), size_t(2));
    Equal("logged_own_setting", logged("GPU feed now ") - changes, size_t(1));

    // The statistics since the scene began.
    struct Report
    {
        bool found = false;
        unsigned long long frames = 0, flushes = 0, fenceFlushes = 0, feedFlushes = 0, timed = 0, early = 0;
        double seconds = 0, fps = 0, draws = 0, passes = 0, sets = 0, reused = 0, constants = 0, renames = 0;
        double submissions = 0, gpuAverage = 0, gpuMin = 0, gpuMax = 0;
        bool gpuTime = false;
        bool passesTimed = true; // false: the line without pass timestamps
        double inPasses = 0, between = 0, before = 0, beforeMax = 0;
    };
    const auto report = [&](bool reset) {
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            lines.clear();
        }
        Report out;
        if (front::LogDeviceStats(reset) != 1)
            return out;
        std::vector<std::string> said;
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            said = lines;
        }
        for (const std::string &line : said)
        {
            if (KISAK_SCAN_NUMBERS(line.c_str(),
                            "vulkan d3d9: frame stats: %llu frames in %lf s (%lf fps); per frame %lf draws, %lf "
                            "passes, %lf descriptor sets (%lf reused), %lf KB constants, %lf dynamic renames, %lf "
                            "submissions (%llu flushes in all, %llu of them ahead of Present at a swap fence, %llu "
                            "to feed the GPU during a frame); GPU frame %lf ms average, %lf min, %lf max over %llu "
                            "frames",
                            &out.frames, &out.seconds, &out.fps, &out.draws, &out.passes, &out.sets, &out.reused,
                            &out.constants, &out.renames, &out.submissions, &out.flushes, &out.fenceFlushes,
                            &out.feedFlushes, &out.gpuAverage, &out.gpuMin, &out.gpuMax, &out.timed)
                == 17)
                out.found = true;
            if (KISAK_SCAN_NUMBERS(line.c_str(),
                            "vulkan d3d9: GPU time: per frame %lf ms in passes, %lf ms between them, and %lf ms "
                            "without work before the frame (%lf max); %llu parts sent early to a GPU with nothing "
                            "left",
                            &out.inPasses, &out.between, &out.before, &out.beforeMax, &out.early)
                == 5)
                out.gpuTime = true;
            double frameMs = 0;
            if (KISAK_SCAN_NUMBERS(line.c_str(),
                            "vulkan d3d9: GPU time: per frame %lf ms from first command to last (passes not timed), "
                            "and %lf ms without work before the frame (%lf max); %llu parts sent early to a GPU with "
                            "nothing left",
                            &frameMs, &out.before, &out.beforeMax, &out.early)
                == 4)
            {
                out.gpuTime = true;
                out.passesTimed = false;
                out.between = frameMs;
            }
        }
        return out;
    };
    const uint64_t frames = (end.presents - start.presents) - (end.presentsSkipped - start.presentsSkipped);
    const Report first = report(false);
    Equal("stats_line", first.found, true);
    Equal("stats_frames", uint64_t(first.frames), frames);
    Equal("stats_draws_per_frame", first.draws, frames ? std::round(double(draws) / double(frames)) : 0.0);
    Equal("stats_feed_parts", uint64_t(first.feedFlushes), end.feedFlushes - start.feedFlushes);
    Equal("stats_seconds", first.seconds > 0.0 && first.fps > 0.0, true);
    // A device with timestamps has timed some of these frames by now, and
    // then says where the GPU's time went.
    Equal("stats_gpu_time_line", first.gpuTime, first.timed > 0);
    if (first.gpuTime)
    {
        // KISAK_VK_PASS_TIMESTAMPS=0 leaves passes untimed, and the line says so.
        Equal("stats_passes_timed", first.passesTimed, PassesTimed());
        Equal("stats_gpu_time_values",
              first.inPasses >= 0.0 && first.between >= 0.0 && first.before >= 0.0 && first.beforeMax >= first.before,
              true);
        Equal("stats_early_parts", uint64_t(first.early), end.feedIdleFlushes - start.feedIdleFlushes);
    }
    Equal("stats_occlusion_line", logged("vulkan d3d9: occlusion queries: "), size_t(1));
    // Asked for again with a new start: the same stretch, and then nothing.
    const Report second = report(true);
    Equal("stats_kept_frames", uint64_t(second.frames), frames);
    Equal("stats_kept_feed_parts", second.feedFlushes, first.feedFlushes);
    const Report third = report(false);
    Equal("stats_reset_line", third.found, true);
    Equal("stats_reset_frames", third.frames, 0ull);
    Equal("stats_reset_feed_parts", third.feedFlushes, 0ull);
    Equal("stats_reset_timed_frames", third.timed, 0ull);
    keep(nullptr);

    // As in the engine, whose main thread runs r_vkStats and sets the dvars
    // while its render thread draws: another thread asks for the statistics
    // and changes the setting while this one draws and presents. The last
    // frame comes out whole, and the device ends on its own setting.
    constexpr int kRounds = 8;
    std::atomic<int> rounds{0}, answered{0};
    std::thread other([&] {
        for (int round = 0; round < kRounds; ++round)
        {
            set(setting(round % 2, 32, 8));
            answered += front::LogDeviceStats(round % 2 == 0) == 1;
            ++rounds;
            SDL_Delay(1);
        }
    });
    // The top row of cells, drawn three times over: the last colours stay.
    const auto row = [&] {
        Reset(Argb(255, 0, 64, 0));
        for (int draw = 0; draw < 3 * kAcross; ++draw)
        {
            const float x = float(draw % kAcross * kCell);
            DrawQuad(x, 0, x + kCell, kCell, 0.5f, Argb(255, 255, uint32_t(draw), 0));
        }
    };
    int meanwhile = 0;
    do
    {
        row();
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        ++meanwhile;
    } while (rounds.load() < kRounds);
    other.join();
    Equal("other_thread_answered", answered.load(), kRounds);
    Equal("other_thread_frames", meanwhile > 0, true);
    set(front::Tuning{});
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    row();
    if (Read())
    {
        Expect("other_thread_first_cell", kCell / 2, kCell / 2, 255, 64, 0);
        Expect("other_thread_last_cell", kSize - kCell / 2, kCell / 2, 255, 95, 0);
        Expect("other_thread_clear_kept", kSize / 2, kSize / 2, 0, 64, 0);
    }
    std::printf("d3d9_check.tuning=frames=%llu draws=%llu parts=%llu early=%llu idle_frame_parts=%llu "
                "idle_frame_early=%llu changes=%zu gpu_time=%s\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(draws),
                static_cast<unsigned long long>(end.feedFlushes - start.feedFlushes),
                static_cast<unsigned long long>(end.feedIdleFlushes - start.feedIdleFlushes),
                static_cast<unsigned long long>(idle.all), static_cast<unsigned long long>(idle.early), changes + 1,
                first.gpuTime ? "yes" : "no");
}

// The statistics' `slow frame` lines: the frames that took longest since the
// statistics were last reset, each with what the device did in it. Eight
// frames are presented, of which the second builds a pipeline (the only
// blended draws of the whole check) and then stands still for 400 ms, and the
// third uploads a texture and stands still for 250 ms. The report must name
// those two by their numbers, with their draws, the pipeline and the upload,
// and with the GPU's times where the device timed its frames; it holds one
// line after one frame, three after eight, the longest first, and none after
// a reset. The two frames are looked up by number and not by rank: a machine
// busy enough to hold another frame up for longer still passes.
void Check::SlowFrames()
{
    constexpr int kFrames = 8;
    constexpr int kPipelineFrame = 2, kPipelineDraws = 20, kPipelineMs = 400;
    constexpr int kUploadFrame = 3, kUploadDraws = 5, kUploadMs = 250;
    std::vector<std::string> lines;
    bool timed = false;
    // The `slow frame` lines of one report.
    const auto report = [&](bool reset) {
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            lines.clear();
            host->logKept = &lines;
        }
        const uint32_t devices = front::LogDeviceStats(reset);
        std::vector<std::string> slow;
        const std::lock_guard<std::mutex> lock(host->mutex);
        host->logKept = nullptr;
        for (const std::string &line : lines)
        {
            if (devices == 1 && line.find("vulkan d3d9: slow frame ") != std::string::npos)
                slow.push_back(line);
            const size_t over = line.find(" max over ");
            if (over != std::string::npos && line.find("vulkan d3d9: frame stats: ") != std::string::npos)
                timed = std::strtoull(line.c_str() + over + 10, nullptr, 10) > 0;
        }
        return slow;
    };
    // The number a line has in front of `unit`, or behind `label`; -1 without it.
    const auto before = [](const std::string &line, const char *unit) {
        const size_t at = line.find(unit);
        if (at == std::string::npos)
            return -1.0;
        size_t start = at;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.'))
            --start;
        return start < at ? std::strtod(line.c_str() + start, nullptr) : -1.0;
    };
    const auto behind = [](const std::string &line, const char *label) {
        const size_t at = line.find(label);
        return at == std::string::npos ? -1.0 : std::strtod(line.c_str() + at + std::strlen(label), nullptr);
    };
    const auto numbered = [&](const std::vector<std::string> &slow, int number) -> const std::string * {
        for (const std::string &line : slow)
        {
            if (behind(line, " ms, frame ") == double(number))
                return &line;
        }
        return nullptr;
    };
    const auto draw = [&](int draws) {
        for (int i = 0; i < draws; ++i)
            DrawQuad(float(4 * i), 0, float(4 * i + 4), 4, 0.5f, Argb(128, 255, uint32_t(8 * i), 0));
    };

    // Frames count from a Present, and the statistics from here.
    Reset(Argb(255, 0, 64, 0));
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    report(true);
    Equal("lines_after_reset", report(false).size(), size_t(0));

    IDirect3DTexture9 *texture = nullptr;
    std::vector<std::string> first;
    for (int frame = 1; frame <= kFrames; ++frame)
    {
        Reset(Argb(255, 0, 64, 0));
        if (frame == kPipelineFrame)
        {
            // No other scene blends: the first of these draws builds a pipeline.
            Ok(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE), "SetRenderState");
            Ok(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR), "SetRenderState");
            Ok(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR), "SetRenderState");
            draw(kPipelineDraws);
            SDL_Delay(kPipelineMs);
        }
        else if (frame == kUploadFrame)
        {
            texture = FilledTexture(64, 64, [](UINT x, UINT y) { return Argb(255, 4 * x, 4 * y, 128); });
            if (!texture)
                return;
            Ok(device->SetTexture(0, texture), "SetTexture");
            Ok(device->SetPixelShader(psTexture), "SetPixelShader");
            draw(kUploadDraws);
            SDL_Delay(kUploadMs);
        }
        else
            draw(frame == 1 ? 10 : 3);
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        if (frame == 1)
            first = report(false);
    }
    const std::vector<std::string> slow = report(false);
    const bool gpuTimed = timed; // by that report's frame stats line
    device->SetTexture(0, nullptr);
    SafeRelease(texture);

    Equal("first_frame_lines", first.size(), size_t(1));
    if (first.size() == 1)
    {
        Equal("first_frame_rank", behind(first[0], "slow frame "), 1);
        Equal("first_frame_number", behind(first[0], " ms, frame "), 1);
        Equal("first_frame_draws", before(first[0], " draws"), 10);
    }
    Equal("lines", slow.size(), size_t(3));
    double longest = 0.0, second = 0.0;
    bool ordered = true;
    for (size_t i = 0; i < slow.size(); ++i)
    {
        const double ms = before(slow[i], " ms, frame ");
        ordered = ordered && behind(slow[i], "slow frame ") == double(i + 1) && ms > 0.0
                  && (i == 0 || ms <= before(slow[i - 1], " ms, frame "));
        if (i == 0)
            longest = ms;
        else if (i == 1)
            second = ms;
    }
    Equal("longest_first", ordered, true);
    const std::string *pipelineLine = numbered(slow, kPipelineFrame);
    Equal("pipeline_frame_named", pipelineLine != nullptr, true);
    if (pipelineLine)
    {
        Equal("pipeline_frame_time", before(*pipelineLine, " ms, frame ") >= double(kPipelineMs), true);
        Equal("pipeline_frame_draws", before(*pipelineLine, " draws"), kPipelineDraws);
        Equal("pipeline_frame_pipelines", before(*pipelineLine, " pipelines") >= 1.0, true);
        Equal("pipeline_frame_uploads", before(*pipelineLine, " texture uploads"), 0);
        // Its timestamps were read frames ago, where the device has them.
        Equal("pipeline_frame_gpu_time", pipelineLine->find("; GPU time not read") == std::string::npos, gpuTimed);
        if (gpuTimed && !PassesTimed())
            Equal("pipeline_frame_gpu_values",
                  pipelineLine->find(" (passes not timed), ") != std::string::npos
                      && before(*pipelineLine, " without work before it") >= 0.0,
                  true);
        else if (gpuTimed)
            Equal("pipeline_frame_gpu_values",
                  behind(*pipelineLine, "; GPU ") >= before(*pipelineLine, " in passes")
                      && before(*pipelineLine, " in passes") >= 0.0
                      && before(*pipelineLine, " without work before it") >= 0.0,
                  true);
    }
    const std::string *uploadLine = numbered(slow, kUploadFrame);
    Equal("upload_frame_named", uploadLine != nullptr, true);
    if (uploadLine)
    {
        Equal("upload_frame_time", before(*uploadLine, " ms, frame ") >= double(kUploadMs), true);
        Equal("upload_frame_draws", before(*uploadLine, " draws"), kUploadDraws);
        Equal("upload_frame_uploads", before(*uploadLine, " texture uploads") >= 1.0, true);
        Equal("upload_frame_megabytes", behind(*uploadLine, " texture uploads (") > 0.0, true);
    }
    // Asked for again with a new start: the same three, and then none.
    Equal("kept_lines", report(true).size(), size_t(3));
    Equal("lines_after_second_reset", report(false).size(), size_t(0));
    std::printf("d3d9_check.slow_frames=frames=%d longest_ms=%.2f second_ms=%.2f gpu_time=%s\n", kFrames, longest,
                second, gpuTimed ? "yes" : "no");
}

// The switch for what a draw takes up from the draw before it
// (front::Tuning::keepDrawState, the engine's r_vkKeepDrawState). The same
// frame of 600 draws, in runs of eight that share a texture and with a vertex
// shader constant set to another value for every draw, is drawn with the
// state kept, not kept, and kept again. Each change is taken up by the
// Present after it and logged once. The three pictures are the same to the
// pixel, and the statistics say what the switch did: every draw of the frame
// in the middle wrote all its constants again, close to twice the bytes of
// the frames around it or more.
void Check::KeptState()
{
    constexpr int kDraws = 600;
    constexpr int kRun = 8;
    constexpr int kCell = 4;
    constexpr int kAcross = int(kSize) / kCell;
    std::vector<std::string> lines;
    const auto set = [&](bool keep) {
        const std::lock_guard<std::mutex> lock(host->mutex);
        host->tuning = front::Tuning{};
        host->tuning.keepDrawState = keep;
    };
    // The number the line has behind `label`, or in front of `unit`; -1 without it.
    const auto behind = [](const std::string &line, const char *label) {
        const size_t at = line.find(label);
        return at == std::string::npos ? -1.0 : std::strtod(line.c_str() + at + std::strlen(label), nullptr);
    };
    const auto before = [](const std::string &line, const char *unit) {
        const size_t at = line.find(unit);
        if (at == std::string::npos)
            return -1.0;
        size_t start = at;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.'))
            --start;
        return start < at ? std::strtod(line.c_str() + start, nullptr) : -1.0;
    };
    struct Frame
    {
        double frames = -1, draws = -1, constantKb = -1, sets = -1;
        size_t saidOn = 0, saidOff = 0;
        std::vector<uint8_t> picture;
    };
    IDirect3DTexture9 *textures[2] = {
        FilledTexture(2, 2, [](UINT, UINT) { return Argb(255, 255, 255, 255); }),
        FilledTexture(2, 2, [](UINT, UINT) { return Argb(255, 255, 128, 0); }),
    };
    if (!textures[0] || !textures[1])
    {
        SafeRelease(textures[0]);
        SafeRelease(textures[1]);
        return;
    }
    // The Present takes the setting up; the statistics then count the one
    // frame drawn under it.
    const auto frame = [&](bool keep) {
        Frame out;
        set(keep);
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            lines.clear();
            host->logKept = &lines;
        }
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        front::LogDeviceStats(true);
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            for (const std::string &line : lines)
            {
                out.saidOn += line.find("draw state kept from one draw to the next: now on") != std::string::npos;
                out.saidOff += line.find("draw state kept from one draw to the next: now off") != std::string::npos;
            }
            lines.clear();
        }
        Reset(Argb(255, 0, 64, 0));
        Ok(device->SetPixelShader(psTexture), "SetPixelShader");
        for (int draw = 0; draw < kDraws; ++draw)
        {
            if (draw % kRun == 0)
                Ok(device->SetTexture(0, textures[draw / kRun % 2]), "SetTexture");
            // The quad is made one to three cells to the left of its place
            // and the constant, which the vertex shader adds, brings it
            // back: a draw that took the constant of the one before would
            // land in another cell and leave its own as the Clear made it.
            const int shift = 1 + draw % 3;
            const float back[4] = {float(shift) * 2.0f * kCell / float(kSize), 0, 0, 0};
            Ok(device->SetVertexShaderConstantF(0, back, 1), "SetVertexShaderConstantF");
            const float x = float((draw % kAcross - shift) * kCell);
            const float y = float(draw / kAcross * kCell);
            DrawQuad(x, y, x + kCell, y + kCell, 0.5f, 0xffffffffu);
        }
        if (Read())
            out.picture = pixels;
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        if (front::LogDeviceStats(true) == 1)
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            for (const std::string &line : lines)
            {
                if (line.find("vulkan d3d9: frame stats: ") == std::string::npos)
                    continue;
                out.frames = behind(line, "frame stats: ");
                out.draws = before(line, " draws, ");
                out.constantKb = before(line, " KB constants");
                out.sets = before(line, " descriptor sets (");
            }
        }
        const std::lock_guard<std::mutex> lock(host->mutex);
        host->logKept = nullptr;
        return out;
    };

    const Frame kept = frame(true);
    const Frame unkept = frame(false);
    const Frame again = frame(true);
    set(true);
    device->SetTexture(0, nullptr);
    SafeRelease(textures[0]);
    SafeRelease(textures[1]);

    Equal("kept_frames", kept.frames, 1);
    Equal("kept_draws", kept.draws, kDraws);
    Equal("unkept_frames", unkept.frames, 1);
    Equal("unkept_draws", unkept.draws, kDraws);
    Equal("again_draws", again.draws, kDraws);
    // The device begins with the state kept: the first frame logs no change.
    Equal("kept_logs_nothing", kept.saidOn + kept.saidOff, size_t(0));
    Equal("off_logged_once", unkept.saidOff, size_t(1));
    Equal("off_logged_alone", unkept.saidOn, size_t(0));
    Equal("on_logged_once", again.saidOn, size_t(1));
    Equal("on_logged_alone", again.saidOff, size_t(0));
    // Kept, a draw writes the vertex shader's constants; not kept, the pixel
    // shader's sampler states and alpha test with them.
    Equal("kept_writes_constants", kept.constantKb > 0.0, true);
    Equal("unkept_writes_more", unkept.constantKb >= 1.9 * kept.constantKb, true);
    Equal("again_writes_as_before", again.constantKb, kept.constantKb);
    // Not kept, a draw takes its sets from the frame's pools; kept, the sets
    // the first frame wrote last across frames, so the third writes none.
    // Under plain uniform buffers (KISAK_VK_UNIFORMS=static) a set 0 holds
    // the draw's offsets and is written for every draw: one a draw, and no
    // texture set.
    Equal("unkept_writes_sets", unkept.sets > 0.0, true);
    Equal("again_writes_no_kept_set", again.sets == 0.0 || again.sets == double(kDraws), true);
    Equal("kept_picture_read", kept.picture.size(), size_t(kSize) * kSize * 4);
    Equal("unkept_picture_same", unkept.picture == kept.picture, true);
    Equal("again_picture_same", again.picture == kept.picture, true);
    if (kept.picture.size() == size_t(kSize) * kSize * 4)
    {
        // Every cell has its run's texture: white, then orange.
        int wrong = 0;
        for (int draw = 0; draw < kDraws; ++draw)
        {
            const uint8_t *p = &kept.picture[((draw / kAcross * kCell + kCell / 2) * kSize
                                              + draw % kAcross * kCell + kCell / 2) * 4];
            const bool orange = draw / kRun % 2 == 1;
            wrong += !(p[2] == 255 && p[1] == (orange ? 128 : 255) && p[0] == (orange ? 0 : 255));
        }
        Equal("cells_wrong", wrong, 0);
        pixels = kept.picture;
        Expect("first_cell", kCell / 2, kCell / 2, 255, 255, 255);
        Expect("second_run", 8 * kCell + kCell / 2, kCell / 2, 255, 128, 0);
        Expect("clear_kept", kSize - 6, kSize - 2, 0, 64, 0);
    }
    std::printf("d3d9_check.kept_state=draws=%d kept_kb=%.1f unkept_kb=%.1f\n", kDraws, kept.constantKb,
                unkept.constantKb);
}

// Not part of a full run: --scene bench. What a frame of the engine's size
// costs the thread that draws it, with the engine's other habits around it:
// 768 draws of four D3D9 calls each (a constant, a texture, a render state,
// the draw), while a second thread polls the GPU sync fence with FLUSH every
// 20 microseconds, as the main thread does in R_SyncGpu for as long as the
// GPU is behind (here all the time, whatever the GPU: the worst case). Each
// frame ends as FrameLoop's do. Prints the time the drawing thread spent on
// its draw calls per frame (wall clock, so the cost of sharing the front's
// lock with the poller is in it) and the polls made meanwhile. The numbers
// depend on the machine and on what else runs on it; the scene passes when
// its frames were presented.
//
// --scene bench_runs is the same frame in the order the engine draws one:
// sorted by material. Eight draws in a row share a texture, an alpha
// reference and a pixel shader constant (three calls for the run), and each
// draw is one vertex shader constant and two indexed triangles from static
// buffers (two calls). It measures what a draw costs when most of its state
// is the last draw's, which is the usual case in the game and the one the
// adapter can do least for in `bench`, where every draw changes everything.
// Its last frame is read back: every cell must hold its run's texture.
void Check::Bench() { BenchFrames(false); }
void Check::BenchRuns() { BenchFrames(true); }

void Check::BenchFrames(bool runs)
{
    constexpr int kFrames = 120;
    constexpr int kWarmUp = 20;
    constexpr int kDraws = 768;
    constexpr int kCell = 4;
    constexpr int kAcross = int(kSize) / kCell;
    constexpr int kRun = 8;
    constexpr int kTextures = 4;
    using Clock = std::chrono::steady_clock;
    static const uint32_t kColours[kTextures] = {Argb(255, 255, 255, 255), Argb(255, 128, 128, 128),
                                                 Argb(255, 255, 0, 0), Argb(255, 0, 0, 255)};
    IDirect3DQuery9 *swapFence = nullptr, *syncFence = nullptr;
    IDirect3DTexture9 *textures[kTextures] = {};
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    bool made = true;
    for (int i = 0; i < kTextures; ++i)
    {
        const uint32_t colour = kColours[i];
        textures[i] = FilledTexture(2, 2, [colour](UINT, UINT) { return colour; });
        made = made && textures[i];
    }
    made = made && Ok(device->CreateQuery(D3DQUERYTYPE_EVENT, &swapFence), "CreateQuery(swap fence)")
        && Ok(device->CreateQuery(D3DQUERYTYPE_EVENT, &syncFence), "CreateQuery(sync fence)");
    if (made && runs)
    {
        // One cell at the top left, moved by the vertex shader constant; the
        // index buffer holds it once for each draw of a run.
        made = Ok(device->CreateVertexBuffer(4 * sizeof(Vertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr),
                  "CreateVertexBuffer")
            && Ok(device->CreateIndexBuffer(kRun * 6 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                            D3DPOOL_DEFAULT, &ib, nullptr),
                  "CreateIndexBuffer");
        void *data = nullptr;
        if (made && Ok(vb->Lock(0, 0, &data, 0), "Lock(vertex buffer)"))
        {
            QuadCorners(static_cast<Vertex *>(data), 0, 0, kCell, kCell, 0.5f, Argb(255, 255, 255, 255));
            vb->Unlock();
        }
        if (made && Ok(ib->Lock(0, 0, &data, 0), "Lock(index buffer)"))
        {
            const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < kRun; ++i)
                std::memcpy(static_cast<uint8_t *>(data) + i * sizeof(quad), quad, sizeof(quad));
            ib->Unlock();
        }
    }
    if (!made)
    {
        SafeRelease(swapFence);
        SafeRelease(syncFence);
        SafeRelease(vb);
        SafeRelease(ib);
        for (IDirect3DTexture9 *&texture : textures)
            SafeRelease(texture);
        return;
    }
    // --keep-draw-state 0: a Present takes the setting up before the first
    // frame is drawn, and the scene leaves the device as it found it.
    const bool keepDrawState = host->benchKeepsDrawState;
    const auto keep = [&](bool on) {
        {
            const std::lock_guard<std::mutex> lock(host->mutex);
            host->tuning.keepDrawState = on;
        }
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    };
    if (!keepDrawState)
        keep(false);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> polls{0};
    IDirect3DQuery9 *const polled = syncFence;
    std::thread mainThread([&] {
        Clock::time_point last = Clock::now();
        while (!stop.load(std::memory_order_relaxed))
        {
            const Clock::time_point now = Clock::now();
            if (now - last < std::chrono::microseconds(20))
                continue;
            last = now;
            BOOL done = FALSE;
            polled->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
            polls.fetch_add(1, std::memory_order_relaxed);
        }
    });
    const front::DeviceCounters before = Counters();
    double totalMs = 0.0, minMs = 1e9;
    uint64_t pollsCounted = 0;
    bool lastFrameRead = false;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        Reset(Argb(255, 0, 64, 0));
        Ok(device->SetPixelShader(psTexture), "SetPixelShader");
        const uint64_t pollsBefore = polls.load(std::memory_order_relaxed);
        const Clock::time_point start = Clock::now();
        if (runs)
        {
            device->SetStreamSource(0, vb, 0, sizeof(Vertex));
            device->SetIndices(ib);
        }
        for (int draw = 0; draw < kDraws; ++draw)
        {
            const float x = float(draw % kAcross * kCell);
            const float y = float(draw / kAcross * kCell);
            if (runs)
            {
                const int run = draw / kRun;
                if (draw % kRun == 0)
                {
                    const float tint[4] = {float(run), 0.0f, 0.0f, 1.0f};
                    device->SetTexture(0, textures[run % kTextures]);
                    device->SetRenderState(D3DRS_ALPHAREF, DWORD(run & 0xff));
                    device->SetPixelShaderConstantF(0, tint, 1);
                }
                // Clip space is two units across: the cell moves x pixels right and y down.
                const float offset[4] = {2.0f * x / kSize, -2.0f * y / kSize, 0.0f, 0.0f};
                device->SetVertexShaderConstantF(0, offset, 1);
                device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, UINT(6 * (draw % kRun)), 2);
                continue;
            }
            const float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            device->SetVertexShaderConstantF(0, offset, 1);
            device->SetTexture(0, textures[draw & 1]);
            device->SetRenderState(D3DRS_ALPHAREF, DWORD(draw & 0xff));
            Vertex quad[6];
            QuadList(quad, x, y, x + kCell, y + kCell, 0.5f, Argb(255, 255, 255, 255), kSize);
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex));
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (frame >= kWarmUp)
        {
            totalMs += ms;
            minMs = ms < minMs ? ms : minMs;
            pollsCounted += polls.load(std::memory_order_relaxed) - pollsBefore;
        }
        BOOL done = FALSE;
        int waits = 0;
        while (swapFence->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE && waits < 20000)
        {
            ++waits;
            if (waits > 200)
                SDL_Delay(1);
            else
                std::this_thread::yield();
        }
        if (runs && frame == kFrames - 1)
            lastFrameRead = Read();
        Ok(syncFence->Issue(D3DISSUE_END), "Issue(sync fence)");
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
        Ok(swapFence->Issue(D3DISSUE_END), "Issue(swap fence)");
    }
    stop = true;
    mainThread.join();
    const front::DeviceCounters after = Counters();
    if (!keepDrawState)
        keep(true);
    Equal(runs ? "bench_runs_frames_presented" : "bench_frames_presented",
          (after.presents - before.presents) + (after.presentsSkipped - before.presentsSkipped), uint64_t(kFrames));
    const int counted = kFrames - kWarmUp;
    const int calls = runs ? 2 * kDraws + 3 * (kDraws / kRun) : 4 * kDraws;
    std::printf("d3d9_check.%s=frames=%d draws=%d calls=%d draw_ms_average=%.3f draw_ms_min=%.3f "
                "polls_per_frame=%.0f submissions_per_frame=%.2f\n",
                runs ? "bench_runs" : "bench", counted, kDraws, calls, totalMs / counted, minMs,
                double(pollsCounted) / counted, double(after.submissions - before.submissions) / kFrames);
    if (runs && lastFrameRead)
    {
        // The first and last cell of some runs, and the cells either side of
        // a run's end: each holds its own run's texture.
        const int cells[] = {0, kRun - 1, kRun, 2 * kRun - 1, 2 * kRun, 3 * kRun, 37 * kRun + 3, kDraws - kRun - 1,
                             kDraws - kRun, kDraws - 1};
        for (const int cell : cells)
        {
            const uint32_t colour = kColours[cell / kRun % kTextures];
            const std::string name = "bench_runs_cell_" + std::to_string(cell);
            Expect(name.c_str(), cell % kAcross * kCell + kCell / 2, cell / kAcross * kCell + kCell / 2,
                   (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
        }
    }
    device->SetTexture(0, nullptr);
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    SafeRelease(swapFence);
    SafeRelease(syncFence);
    SafeRelease(vb);
    SafeRelease(ib);
    for (IDirect3DTexture9 *&texture : textures)
        SafeRelease(texture);
}

// Reset to another back buffer size and back: the new size, default state,
// and drawing at that size.
// Multisampled targets, as the renderer uses them with r_aaSamples above 1
// (R_SetupAntiAliasing, r_rendertarget.cpp, R_Resolve): a back buffer and a
// depth surface of 4 samples, drawn into, resolved into a texture with
// StretchRect, read with GetRenderTargetData (D3D9 refuses that one; the
// front gives the resolved picture, for screenshots), and presented. A
// triangle's slanted edge is where samples show: with one sample a pixel is
// in or out, with four a pixel the edge crosses takes a share of the colour.
void Check::Multisample()
{
    constexpr UINT kTarget = 64;
    // A triangle over the upper left half of a kTarget square: its long edge
    // runs from the top right corner to the bottom left one.
    auto drawTriangle = [&](uint32_t color) {
        Vertex tri[3] = {
            Vertex{ClipX(0, kTarget), ClipY(0, kTarget), 0.5f, 0.0f, 0.0f, color},
            Vertex{ClipX(kTarget, kTarget), ClipY(0, kTarget), 0.5f, 0.0f, 0.0f, color},
            Vertex{ClipX(0, kTarget), ClipY(kTarget, kTarget), 0.5f, 0.0f, 0.0f, color},
        };
        Ok(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex)), "DrawPrimitiveUP(triangle)");
    };
    // Pixels of the read picture whose red lies strictly between the two
    // colours: along the slanted edge, and with several samples also the
    // top row and left column, whose upper and left samples lie outside the
    // clip volume once the viewport is moved by half a pixel (as DXVK moves
    // it; a GPU that clips with a guard band may cover them).
    auto partlyCovered = [&](UINT size) {
        int count = 0;
        for (UINT y = 0; y < size; ++y)
        {
            for (UINT x = 0; x < size; ++x)
            {
                const int red = pixels[(y * readSize + x) * 4 + 2];
                count += red > 24 && red < 232;
            }
        }
        return count;
    };

    // One sample first, as the yardstick: no pixel is partly covered.
    Reset();
    drawTriangle(Argb(255, 255, 0, 0));
    int oneSample = -1;
    if (Read())
        oneSample = partlyCovered(kTarget);
    Equal("one_sample_edge_hard", oneSample, 0);

    IDirect3DSurface9 *target = nullptr, *depth = nullptr, *oneDepth = nullptr, *resolvedLevel = nullptr;
    IDirect3DTexture9 *resolved = nullptr;
    if (!Ok(device->CreateRenderTarget(kTarget, kTarget, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &target,
                                       nullptr),
            "CreateRenderTarget(4 samples)")
        // The back buffer's size: it serves both (D3D9 lets a depth surface be
        // larger than the target).
        || !Ok(device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, TRUE, &depth,
                                                 nullptr),
               "CreateDepthStencilSurface(4 samples)")
        || !Ok(device->CreateDepthStencilSurface(kTarget, kTarget, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                 &oneDepth, nullptr),
               "CreateDepthStencilSurface(1 sample)")
        || !Ok(device->CreateTexture(kTarget, kTarget, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                     &resolved, nullptr),
               "CreateTexture(resolve target)")
        || !Ok(resolved->GetSurfaceLevel(0, &resolvedLevel), "GetSurfaceLevel(resolve target)"))
    {
        SafeRelease(resolvedLevel);
        SafeRelease(resolved);
        SafeRelease(oneDepth);
        SafeRelease(depth);
        SafeRelease(target);
        return;
    }
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    Equal("target_samples", desc.MultiSampleType, D3DMULTISAMPLE_4_SAMPLES);
    depth->GetDesc(&desc);
    Equal("depth_samples", desc.MultiSampleType, D3DMULTISAMPLE_4_SAMPLES);
    resolvedLevel->GetDesc(&desc);
    Equal("texture_samples", desc.MultiSampleType, D3DMULTISAMPLE_NONE);
    // 8 samples where the device has them, else 4 (lavapipe has 1 and 4).
    IDirect3DSurface9 *eight = nullptr;
    if (Ok(device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, 0, FALSE, &eight, nullptr),
           "CreateRenderTarget(8 samples)"))
    {
        eight->GetDesc(&desc);
        Equal("eight_or_four", desc.MultiSampleType == D3DMULTISAMPLE_8_SAMPLES
                                   || desc.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES,
              true);
        std::printf("d3d9_check.multisample=eight_samples_made_with=%u\n", unsigned(desc.MultiSampleType));
        SafeRelease(eight);
    }
    Equal("three_refused", device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_3_SAMPLES, 0, FALSE,
                                                      &eight, nullptr),
          D3DERR_NOTAVAILABLE);

    // Drawn with depth: a far quad over all, the triangle in front of it.
    Reset();
    Ok(device->SetRenderTarget(0, target), "SetRenderTarget(4 samples)");
    Ok(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface(4 samples)");
    Ok(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, Argb(255, 0, 0, 255), 1.0f, 0), "Clear(4 samples)");
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    drawTriangle(Argb(255, 255, 0, 0));
    DrawQuad(0, 0, kTarget, kTarget, 0.75f, Argb(255, 0, 255, 0), kTarget); // behind: shows below the edge only
    // Its samples counted: a quad of 8 x 8 pixels in front of everything is
    // 256 samples (or every sample where the device does not count them).
    IDirect3DQuery9 *occlusion = nullptr;
    if (Ok(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion), "CreateQuery(occlusion)"))
    {
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        Ok(occlusion->Issue(D3DISSUE_BEGIN), "Issue(begin)");
        DrawQuad(48, 48, 56, 56, 0.25f, Argb(255, 255, 255, 255), kTarget);
        Ok(occlusion->Issue(D3DISSUE_END), "Issue(end)");
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
        DWORD samples = 0;
        HRESULT hr = S_FALSE;
        int polls = 0;
        while ((hr = occlusion->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH)) == S_FALSE && polls < 100000)
        {
            ++polls;
            SDL_Delay(polls > 100 ? 1 : 0);
        }
        Equal("occlusion_done", hr, S_OK);
        Equal("occlusion_samples", samples, Counters().occlusionCounted ? DWORD(8 * 8 * 4) : DWORD(0xffffff));
        SafeRelease(occlusion);
    }

    // A depth surface of another count beside it: the draw is skipped.
    Ok(device->SetDepthStencilSurface(oneDepth), "SetDepthStencilSurface(1 sample beside 4)");
    DrawQuad(0, 0, 8, 8, 0.1f, Argb(255, 255, 255, 255), kTarget);
    Ok(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface(4 samples)");

    // Resolved into a texture of its size (the renderer's R_Resolve).
    Ok(device->StretchRect(target, nullptr, resolvedLevel, nullptr, D3DTEXF_NONE), "StretchRect(resolve)");
    int edge = -1;
    if (Read(resolvedLevel, kTarget))
    {
        Expect("resolved_inside", 8, 8, 255, 0, 0);
        Expect("resolved_behind", 56, 56, 0, 255, 0);
        Expect("mismatched_draw_skipped", 2, 2, 255, 0, 0);
        edge = partlyCovered(kTarget);
    }
    // Every pixel the edge crosses: about one in each row.
    Equal("resolved_edge_soft", edge >= int(kTarget) / 2, true);
    std::printf("d3d9_check.multisample=edge_pixels=%d\n", edge);

    // Read directly: the same resolved picture.
    std::vector<uint8_t> resolvedPixels = pixels;
    if (Read(target, kTarget))
        Equal("read_is_resolved", pixels == resolvedPixels, true);

    // Into the corner of the back buffer at half size: resolved, then scaled.
    Reset(Argb(255, 0, 0, 0));
    RECT corner{0, 0, kTarget / 2, kTarget / 2};
    Ok(device->StretchRect(target, nullptr, backBuffer, &corner, D3DTEXF_POINT), "StretchRect(resolve and scale)");
    if (Read())
    {
        Expect("scaled_inside", 4, 4, 255, 0, 0);
        Expect("scaled_behind", 28, 28, 0, 255, 0);
        Expect("scaled_outside", 48, 48, 0, 0, 0);
    }

    // The back buffer itself with 4 samples, through Reset, as the renderer
    // makes it: drawn, read, presented, and read from the front buffer.
    SafeRelease(backBuffer);
    D3DPRESENT_PARAMETERS multisampled = params;
    multisampled.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
    if (Ok(device->Reset(&multisampled), "Reset(4 samples)")
        && Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer(4 samples)"))
    {
        backBuffer->GetDesc(&desc);
        Equal("back_buffer_samples", desc.MultiSampleType, D3DMULTISAMPLE_4_SAMPLES);
        Equal("back_buffer_width", desc.Width, kSize);
        Reset(Argb(255, 0, 0, 255));
        Ok(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface(4 samples, back buffer)");
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        Ok(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0), "Clear(depth)");
        // The viewport is the whole back buffer here, so the triangle covers
        // the upper left half of it and its slanted edge crosses all of it.
        drawTriangle(Argb(255, 255, 0, 0));
        int backEdge = -1;
        if (Read())
        {
            Expect("back_buffer_inside", 8, 8, 255, 0, 0);
            Expect("back_buffer_outside", 100, 100, 0, 0, 255);
            backEdge = partlyCovered(kSize);
        }
        Equal("back_buffer_edge_soft", backEdge >= int(kSize) / 2, true);
        std::printf("d3d9_check.multisample=back_buffer_edge_pixels=%d\n", backEdge);
        std::vector<uint8_t> drawn = pixels;
        const uint64_t before = Counters().presents + Counters().presentsSkipped;
        Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present(4 samples)");
        Equal("presented", Counters().presents + Counters().presentsSkipped, before + 1);
        if (Ok(device->GetFrontBufferData(0, readSurface), "GetFrontBufferData(4 samples)") && ReadLocked(kSize))
            Equal("front_buffer_is_resolved", pixels == drawn, true);
        presented += Counters().presents > 0;
    }
    SafeRelease(backBuffer);
    if (Ok(device->Reset(&params), "Reset(1 sample)")
        && Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer"))
    {
        backBuffer->GetDesc(&desc);
        Equal("back_buffer_one_sample_again", desc.MultiSampleType, D3DMULTISAMPLE_NONE);
    }
    Reset();
    SafeRelease(resolvedLevel);
    SafeRelease(resolved);
    SafeRelease(oneDepth);
    SafeRelease(depth);
    SafeRelease(target);
}

// The Modern path's temporal anti-aliasing (front::ModernSceneDone,
// docs/vulkan-modern.md): its camera reprojection on the CPU, then frames
// drawn into the back buffer with a depth surface and handed to it. A scene
// that stands still comes back as it was drawn; with the sub-pixel jitter the
// engine applies, a pixel the jitter covers in some frames and not in others
// ends between the two colours, where without the pass it would be one or the
// other; a scene with no depth surface is refused.
void Check::ModernTaa()
{
    // ---- Reprojection, in double and as the float matrix the shader gets ----
    // A camera far from the origin (the engine's maps reach tens of
    // thousands of units) that moves and turns between two frames.
    using Row4 = double[4];
    const auto viewMatrix = [](const double origin[3], double yaw, float out[4][4]) {
        const double forward[3] = {std::cos(yaw), std::sin(yaw), 0.0};
        const double left[3] = {-std::sin(yaw), std::cos(yaw), 0.0};
        const double up[3] = {0.0, 0.0, 1.0};
        for (int i = 0; i < 3; ++i)
        {
            out[i][0] = static_cast<float>(-left[i]);
            out[i][1] = static_cast<float>(up[i]);
            out[i][2] = static_cast<float>(forward[i]);
            out[i][3] = 0.0f;
        }
        for (int c = 0; c < 3; ++c)
        {
            const double axis[3] = {c == 0 ? -left[0] : c == 1 ? up[0] : forward[0],
                                    c == 0 ? -left[1] : c == 1 ? up[1] : forward[1],
                                    c == 0 ? -left[2] : c == 1 ? up[2] : forward[2]};
            out[3][c] = static_cast<float>(-(origin[0] * axis[0] + origin[1] * axis[1] + origin[2] * axis[2]));
        }
        out[3][3] = 1.0f;
    };
    // The engine's InfinitePerspectiveMatrix (src/universal/com_math.cpp).
    const auto projectionMatrix = [](float jitterX, float jitterY, float out[4][4]) {
        std::memset(out, 0, sizeof(float) * 16);
        const float k = 0.99951172f;
        out[0][0] = k / 0.75f;
        out[1][1] = k / 0.5625f;
        out[2][2] = k;
        out[2][3] = 1.0f;
        out[3][2] = -4.0f * k;
        out[2][0] = jitterX;
        out[2][1] = jitterY;
    };
    // p * view * projection with the jitter left out, in double.
    const auto toClip = [](const Row4 p, const gfx_vk::modern::SceneView &scene, Row4 out) {
        double viewed[4];
        for (int c = 0; c < 4; ++c)
            viewed[c] = p[0] * scene.view[0][c] + p[1] * scene.view[1][c] + p[2] * scene.view[2][c]
                + p[3] * scene.view[3][c];
        for (int c = 0; c < 4; ++c)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += viewed[k] * ((k == 2 && c < 2) ? 0.0 : scene.projection[k][c]);
            out[c] = sum;
        }
    };
    gfx_vk::modern::SceneView before{}, now{};
    const double originBefore[3] = {52000.0, -31000.0, 900.0};
    const double originNow[3] = {52012.0, -30995.0, 901.5};
    viewMatrix(originBefore, 0.70, before.view);
    viewMatrix(originNow, 0.72, now.view);
    projectionMatrix(0.004f, -0.003f, before.projection);
    projectionMatrix(-0.002f, 0.005f, now.projection);
    double reprojection[4][4];
    Equal("reprojection_made", gfx_vk::modern::Reprojection(now, before, reprojection), true);
    // World points 30 to 6000 units in front, and a direction (the sky).
    const Row4 points[] = {
        {52030.0 + 5.0, -30995.0 + 25.0, 902.0, 1.0},
        {52012.0 + 400.0, -30995.0 + 300.0, 950.0, 1.0},
        {52012.0 + 4500.0, -30995.0 + 3800.0, 1200.0, 1.0},
        {0.75, 0.65, 0.05, 0.0},
    };
    double worstDouble = 0.0, worstFloat = 0.0;
    for (const Row4 &point : points)
    {
        double clipNow[4], clipBefore[4];
        toClip(point, now, clipNow);
        toClip(point, before, clipBefore);
        // What the shader starts from: NDC x, y and the depth, w = 1.
        const double start[4] = {clipNow[0] / clipNow[3], clipNow[1] / clipNow[3], clipNow[2] / clipNow[3], 1.0};
        const double wantX = clipBefore[0] / clipBefore[3], wantY = clipBefore[1] / clipBefore[3];
        double viaDouble[4] = {}, viaFloat[4] = {};
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
            {
                viaDouble[c] += start[k] * reprojection[k][c];
                viaFloat[c] += static_cast<float>(start[k]) * static_cast<float>(reprojection[k][c]);
            }
        worstDouble = std::max(worstDouble, std::max(std::fabs(viaDouble[0] / viaDouble[3] - wantX),
                                                     std::fabs(viaDouble[1] / viaDouble[3] - wantY)));
        worstFloat = std::max(worstFloat, std::max(std::fabs(viaFloat[0] / viaFloat[3] - wantX),
                                                   std::fabs(viaFloat[1] / viaFloat[3] - wantY)));
    }
    // In NDC; a pixel of a 1920-wide image is 0.00104.
    Equal("reprojection_exact", worstDouble < 1e-9, true);
    Equal("reprojection_float_under_quarter_pixel", worstFloat < 0.00026, true);
    if (worstFloat >= 0.00026)
        result->detail += " reprojection_float_error=" + std::to_string(worstFloat);
    float jx = 0.0f, jy = 0.0f;
    gfx_vk::modern::JitterPixels(0, jx, jy);
    Equal("jitter_first_x_milli", std::lround(jx * 1000.0f), 0);
    Equal("jitter_first_y_milli", std::lround(jy * 1000.0f), -167);
    gfx_vk::modern::JitterPixels(9, jx, jy);
    Equal("jitter_wraps_x_milli", std::lround(jx * 1000.0f), -250);

    // ---- Frames through the front ----
    IDirect3DSurface9 *depth = nullptr;
    if (!Ok(device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth,
                                              nullptr),
            "CreateDepthStencilSurface"))
        return;
    front::ModernSceneView view{};
    for (int i = 0; i < 4; ++i)
        view.view[i][i] = view.projection[i][i] = 1.0f;
    view.width = view.height = kSize;
    // One frame: black, with a white quad over the left half whose right
    // edge sits 0.4 pixels past pixel 63's, so that pixel 64 is covered when
    // the jitter moves the scene right by more than 0.1 pixels.
    const auto frame = [&](float pixelsX, float pixelsY, bool first) {
        Reset(Argb(255, 0, 0, 0));
        device->SetDepthStencilSurface(depth);
        Ok(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0), "Clear(depth)");
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        const float jitter[4] = {2.0f * pixelsX / kSize, -2.0f * pixelsY / kSize, 0.0f, 0.0f};
        device->SetVertexShaderConstantF(0, jitter, 1);
        DrawQuad(0, 0, 64.4f, 128, 0.5f, Argb(255, 255, 255, 255));
        view.projection[2][0] = jitter[0];
        view.projection[2][1] = jitter[1];
        view.cut = first;
        return front::ModernSceneDone(device, view) == front::ModernResult::Done;
    };
    const auto present = [&]() { Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present"); };

    // Still: what was drawn comes back.
    bool done = true;
    for (int i = 0; i < 4; ++i)
    {
        done = frame(0.0f, 0.0f, i == 0) && done;
        if (i < 3)
            present();
    }
    Equal("still_resolved", done, true);
    if (Read())
    {
        Expect("still_inside", 32, 64, 255, 255, 255);
        Expect("still_edge_in", 63, 64, 255, 255, 255);
        Expect("still_edge_out", 64, 64, 0, 0, 0);
        Expect("still_outside", 96, 64, 0, 0, 0);
    }
    present();

    // Jittered: pixel 64 is covered in 3 of the 8 frames of the sequence.
    done = true;
    for (uint32_t i = 0; i < 16; ++i)
    {
        gfx_vk::modern::JitterPixels(i, jx, jy);
        done = frame(jx, jy, i == 0) && done;
        if (i < 15)
            present();
    }
    Equal("jittered_resolved", done, true);
    if (Read())
    {
        Expect("jittered_inside", 32, 64, 255, 255, 255);
        Expect("jittered_outside", 96, 64, 0, 0, 0);
        const int edge = pixels[(64 * readSize + 64) * 4 + 1];
        Equal("jittered_edge_between", edge > 20 && edge < 235, true);
        if (!(edge > 20 && edge < 235))
            result->detail += " jittered_edge=" + std::to_string(edge);
    }
    present();

    // No depth surface: refused, and the scene is left as drawn.
    Reset(Argb(255, 0, 0, 0));
    DrawQuad(0, 0, 64, 128, 0.5f, Argb(255, 255, 255, 255));
    Equal("no_depth_refused", front::ModernSceneDone(device, view) == front::ModernResult::Refused, true);
    if (Read())
        Expect("no_depth_untouched", 32, 64, 255, 255, 255);
    device->SetDepthStencilSurface(nullptr);
    const float zero[4] = {0, 0, 0, 0};
    device->SetVertexShaderConstantF(0, zero, 1);
    SafeRelease(depth);
}

// D3D9 switches the display for a fullscreen device and back for a windowed
// one; the front asks the host (Host::fullscreen). This host only records the
// asks, so nothing on the machine changes.
void Check::Fullscreen()
{
    host->fullscreenCalls.clear();
    SafeRelease(backBuffer);
    D3DPRESENT_PARAMETERS fullscreen = params;
    fullscreen.Windowed = FALSE;
    fullscreen.BackBufferWidth = fullscreen.BackBufferHeight = 96;
    fullscreen.FullScreen_RefreshRateInHz = 60;
    if (Ok(device->Reset(&fullscreen), "Reset(fullscreen)"))
    {
        Equal("fullscreen_asks", host->fullscreenCalls.size(), size_t(1));
        if (!host->fullscreenCalls.empty())
        {
            const SdlHost::FullscreenCall &call = host->fullscreenCalls.back();
            Equal("fullscreen_mode", call.fullscreen, true);
            Equal("fullscreen_width", call.mode.width, 96u);
            Equal("fullscreen_height", call.mode.height, 96u);
            Equal("fullscreen_refresh", call.mode.refreshRate, 60u);
        }
        if (Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer(fullscreen)"))
        {
            Reset(Argb(255, 0, 128, 0));
            Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present(fullscreen)");
        }
        SafeRelease(backBuffer);
    }
    // Windowed again: the desktop's mode back, once.
    host->fullscreenCalls.clear();
    if (Ok(device->Reset(&params), "Reset(windowed)"))
    {
        Equal("windowed_asks", host->fullscreenCalls.size(), size_t(1));
        if (!host->fullscreenCalls.empty())
            Equal("windowed_mode", host->fullscreenCalls.back().fullscreen, false);
    }
    host->fullscreenCalls.clear();
    if (Ok(device->Reset(&params), "Reset(windowed again)"))
        Equal("windowed_again_asks", host->fullscreenCalls.size(), size_t(0));
    Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer");
}

void Check::ResetDevice()
{
    SafeRelease(backBuffer);
    D3DPRESENT_PARAMETERS resized = params;
    resized.BackBufferWidth = resized.BackBufferHeight = 96;
    if (!Ok(device->Reset(&resized), "Reset(96x96)"))
        return;
    Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer");
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    Equal("reset_width", desc.Width, 96u);
    DWORD value = 1;
    device->GetRenderState(D3DRS_ALPHAREF, &value);
    Equal("reset_state", value, 0ul);
    IDirect3DPixelShader9 *shader = psColor;
    device->GetPixelShader(&shader);
    Equal("reset_unbinds", shader == nullptr, true);
    Reset(Argb(255, 0, 128, 0));
    DrawQuad(0, 0, 48, 48, 0.5f, Argb(255, 255, 0, 0), 96);
    if (Read(backBuffer, 96))
    {
        Expect("reset_quad", 24, 24, 255, 0, 0);
        Expect("reset_clear", 72, 72, 0, 128, 0);
        Expect("reset_edge", 95, 95, 0, 128, 0);
    }
    SafeRelease(backBuffer);
    if (Ok(device->Reset(&params), "Reset(128x128)"))
        Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer");
}

// Present through the device and the swap chain, and read the front buffer.
// The back buffer's contents are undefined after a Present (DISCARD), so
// each frame clears first.
void Check::Present()
{
    Reset(Argb(255, 10, 20, 30));
    const uint64_t before = Counters().presents + Counters().presentsSkipped;
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    Reset(Argb(255, 200, 100, 50));
    IDirect3DSwapChain9 *chain = nullptr;
    if (Ok(device->GetSwapChain(0, &chain), "GetSwapChain"))
    {
        Ok(chain->Present(nullptr, nullptr, nullptr, nullptr, 0), "IDirect3DSwapChain9::Present");
        SafeRelease(chain);
    }
    const front::DeviceCounters counters = Counters();
    Equal("presents_counted", counters.presents + counters.presentsSkipped, before + 2);
    presented += counters.presents > 0;

    // Halfway through the next frame, as when the engine's screenshot command
    // runs on the main thread while the render thread draws: the front buffer
    // is still the frame last shown, not the back buffer being drawn.
    Reset(Argb(255, 0, 0, 255));
    DrawQuad(0, 0, 8, 8, 0.5f, Argb(255, 0, 255, 0));
    if (readSize != kSize)
        Read();
    if (Ok(device->GetFrontBufferData(0, readSurface), "GetFrontBufferData") && ReadLocked(kSize))
    {
        Expect("front_buffer", 5, 5, 200, 100, 50);
        Expect("front_buffer_far", kSize - 1, kSize - 1, 200, 100, 50);
    }
    if (Read())
        Expect("back_buffer_mid_frame", 5, 5, 0, 255, 0);

    // Windowed screenshots: the renderer asks for the front buffer in a surface
    // the size of the display, and finds the window's image at the window's
    // place on it (Host::windowOrigin).
    IDirect3DSurface9 *display = nullptr;
    constexpr UINT kDisplay = kSize * 2;
    if (Ok(device->CreateOffscreenPlainSurface(kDisplay, kDisplay, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &display, nullptr),
           "CreateOffscreenPlainSurface(display)"))
    {
        D3DLOCKED_RECT locked{};
        if (Ok(display->LockRect(&locked, nullptr, 0), "LockRect(display)"))
        {
            for (UINT y = 0; y < kDisplay; ++y)
                std::memset(static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, 0x11, kDisplay * 4);
            display->UnlockRect();
        }
        if (Ok(device->GetFrontBufferData(0, display), "GetFrontBufferData(display)")
            && Ok(display->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect(display)"))
        {
            const auto at = [&](UINT x, UINT y) {
                const uint8_t *p = static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch + x * 4;
                return Argb(p[3], p[2], p[1], p[0]);
            };
            Equal("windowed_front_buffer", at(kWindowOriginX + 5, kWindowOriginY + 5), Argb(255, 200, 100, 50));
            Equal("windowed_outside_left", at(kWindowOriginX - 1, kWindowOriginY + 5), 0x11111111u);
            Equal("windowed_outside_below", at(kWindowOriginX + 5, kWindowOriginY + kSize), 0x11111111u);
            display->UnlockRect();
        }
        SafeRelease(display);
    }
}

bool Check::CreateDevice(std::string &error)
{
    params = D3DPRESENT_PARAMETERS{};
    params.BackBufferWidth = kSize;
    params.BackBufferHeight = kSize;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = WindowHandle(*host);
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, WindowHandle(*host), D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                         &params, &device);
    if (FAILED(hr))
    {
        char line[64];
        std::snprintf(line, sizeof(line), "CreateDevice failed (0x%08x)", static_cast<unsigned>(hr));
        error = line;
        return false;
    }
    return true;
}

bool Check::CreateObjects(std::string &error)
{
    HRESULT hr = device->CreateVertexShader(reinterpret_cast<const DWORD *>(shaders::kVsBasic), &vsBasic);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD *>(shaders::kPsColor), &psColor);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD *>(shaders::kPsTexture), &psTexture);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD *>(shaders::kPsShadow), &psShadow);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD *>(shaders::kPsVolume), &psVolume);
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    if (SUCCEEDED(hr))
        hr = device->CreateVertexDeclaration(elements, &decl);
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readSurface, nullptr);
    if (SUCCEEDED(hr))
        hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr))
    {
        char line[64];
        std::snprintf(line, sizeof(line), "setup failed (0x%08x)", static_cast<unsigned>(hr));
        error = line;
        return false;
    }
    // The shader's bytecode comes back as it went in.
    UINT bytes = 0;
    vsBasic->GetFunction(nullptr, &bytes);
    if (bytes != sizeof(shaders::kVsBasic))
    {
        error = "GetFunction returned " + std::to_string(bytes) + " bytes, expected "
            + std::to_string(sizeof(shaders::kVsBasic));
        return false;
    }
    return true;
}

void Check::ReleaseObjects()
{
    if (device)
    {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetVertexDeclaration(nullptr);
    }
    SafeRelease(vsBasic);
    SafeRelease(psColor);
    SafeRelease(psTexture);
    SafeRelease(psShadow);
    SafeRelease(psVolume);
    SafeRelease(decl);
    SafeRelease(readSurface);
    SafeRelease(backBuffer);
}

int Run(const Options &options)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return Failed("sdl", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(nullptr))
        return Failed("loader", SDL_GetError());
    SdlHost sdl;
    sdl.benchKeepsDrawState = options.benchKeepsDrawState;
    sdl.window = SDL_CreateWindow("KisakCOD Vulkan D3D9 check", int(kSize) * 4, int(kSize) * 4, SDL_WINDOW_VULKAN);
    if (!sdl.window)
        return Failed("window", SDL_GetError());
    sdl.extensions = SDL_Vulkan_GetInstanceExtensions(&sdl.extensionCount);
    if (!sdl.extensions)
        return Failed("loader", SDL_GetError());

    front::Host host;
    host.user = &sdl;
    host.windowSystem = HostWindowSystem;
    host.modeCount = HostModeCount;
    host.mode = HostMode;
    host.currentMode = HostCurrentMode;
    host.log = LogLine;
    host.validation = options.validation;
    host.deviceIndex = options.device;
    host.coreDestroyed = HostCoreDestroyed;
    host.windowOrigin = HostWindowOrigin;
    host.fullscreen = HostFullscreen;
    host.tuning = HostTuning;

    Check check;
    check.host = &sdl;
    check.d3d = front::CreateDirect3D9(host);

    struct Scene
    {
        const char *name;
        void (Check::*run)();
        bool needsDevice;
        bool byNameOnly = false; // not part of a full run
    };
    const Scene scenes[] = {
        {"adapter", &Check::Adapter, false},
        {"states", &Check::States, true},
        {"draw", &Check::Draw, true},
        {"render_target", &Check::RenderTarget, true},
        {"shadow_map", &Check::ShadowMap, true},
        {"model_lighting", &Check::ModelLighting, true},
        {"dirty_update", &Check::DirtyUpdate, true},
        {"references", &Check::References, true},
        {"threads", &Check::Threads, true},
        {"queries", &Check::Queries, true},
        {"occlusion", &Check::Occlusion, true},
        {"frame_loop", &Check::FrameLoop, true},
        {"gpu_feed", &Check::GpuFeed, true},
        {"tuning", &Check::Tuning, true},
        {"slow_frames", &Check::SlowFrames, true},
        {"kept_state", &Check::KeptState, true},
        {"multisample", &Check::Multisample, true},
        {"modern_taa", &Check::ModernTaa, true},
        {"bench", &Check::Bench, true, true},
        {"bench_runs", &Check::BenchRuns, true, true},
        {"reset", &Check::ResetDevice, true},
        {"fullscreen", &Check::Fullscreen, true},
        {"present", &Check::Present, true},
    };
    const char *failedStage = nullptr;
    std::string failedReason;
    int ran = 0;
    std::string error;
    for (const Scene &scene : scenes)
    {
        if (options.only.empty() ? scene.byNameOnly : options.only != scene.name)
            continue;
        ++ran;
        if (scene.needsDevice && !check.device)
        {
            if (!check.CreateDevice(error) || !check.CreateObjects(error))
            {
                failedStage = "device";
                failedReason = error;
                break;
            }
            std::printf("d3d9_check.validation=%s\n", check.Counters().validation ? "on" : "off");
        }
        PumpEvents();
        SceneResult result;
        check.result = &result;
        (check.*scene.run)();
        std::printf("d3d9_check.scene=%s points=%d mismatched=%d%s%s\n", scene.name, result.points, result.mismatched,
                    result.detail.c_str(), result.error.empty() ? "" : (" error=" + result.error).c_str());
        std::fflush(stdout);
        if (!failedStage && (!result.error.empty() || result.mismatched || !result.points))
        {
            failedStage = "scene";
            failedReason = std::string(scene.name) + (result.error.empty() ? ": values differ" : ": " + result.error);
        }
        if (check.device)
        {
            const HRESULT hr = check.device->Present(nullptr, nullptr, nullptr, nullptr);
            if (FAILED(hr) && !failedStage)
            {
                failedStage = "present";
                failedReason = "Present failed";
            }
        }
    }
    if (!ran && !failedStage)
    {
        failedStage = "usage";
        failedReason = "no scene is called " + options.only;
    }

    // Teardown: every object, then the device, then IDirect3D9, each to zero.
    if (check.device)
    {
        const front::DeviceCounters counters = check.Counters();
        std::printf("d3d9_check.presents=%llu skipped=%llu\n", static_cast<unsigned long long>(counters.presents),
                    static_cast<unsigned long long>(counters.presentsSkipped));
        std::printf("d3d9_check.unsupported_calls=%llu expected=%llu\n",
                    static_cast<unsigned long long>(counters.unsupportedCalls),
                    static_cast<unsigned long long>(options.only.empty() || options.only == "states"
                                                        ? kExpectedUnsupported
                                                        : 0));
        if (!failedStage && (options.only.empty() || options.only == "states")
            && counters.unsupportedCalls != kExpectedUnsupported)
        {
            failedStage = "unsupported";
            failedReason = "a check reached a method the front does not support (see the log lines)";
        }
        if (!failedStage && !counters.presents)
        {
            failedStage = "present";
            failedReason = "no frame was presented";
        }
        check.ReleaseObjects();
        const uint32_t live = check.Counters().liveObjects;
        std::printf("d3d9_check.live_objects_at_teardown=%u\n", live);
        if (!failedStage && live)
        {
            failedStage = "teardown";
            failedReason = std::to_string(live) + " objects outlived their last release";
        }
        // The engine's order at quit: the window goes first (WM_DESTROY, which
        // detaches the device's surface), then the device is released.
        const uint32_t detached = front::WindowDestroying(WindowHandle(sdl));
        const uint64_t skippedBefore = check.Counters().presentsSkipped;
        const HRESULT detachedPresent = check.device->Present(nullptr, nullptr, nullptr, nullptr);
        const bool presentSkipped = check.Counters().presentsSkipped == skippedBefore + 1;
        SDL_DestroyWindow(sdl.window);
        sdl.window = nullptr;
        std::printf("d3d9_check.window_detached=%u present_after=%s\n", detached,
                    SUCCEEDED(detachedPresent) ? (presentSkipped ? "skipped" : "shown") : "failed");
        if (!failedStage && (detached != 1 || FAILED(detachedPresent) || !presentSkipped))
        {
            failedStage = "teardown";
            failedReason = "the device did not let go of its window when the window went";
        }
        IDirect3DDevice9 *device = check.device;
        check.device = nullptr;
        const ULONG refs = device->Release();
        front::DeviceCounters after{};
        const bool stillThere = front::Counters(device, after);
        std::printf("d3d9_check.device_release=%lu destroyed=%s\n", static_cast<unsigned long>(refs),
                    stillThere ? "no" : "yes");
        if (!failedStage && (refs || stillThere || !sdl.coreDestroyed))
        {
            failedStage = "teardown";
            failedReason = "the device outlived its last release";
        }
    }
    const ULONG d3dRefs = check.d3d->Release();
    if (!failedStage && d3dRefs)
    {
        failedStage = "teardown";
        failedReason = "IDirect3D9 outlived its last release";
    }
    if (sdl.window)
        SDL_DestroyWindow(sdl.window);
    std::printf("d3d9_check.validation_errors=%u\n", sdl.finalStats.validationErrors);
    std::printf("d3d9_check.validation_warnings=%u\n", sdl.finalStats.validationWarnings);
    std::printf("d3d9_check.validation_ignored=%u\n", sdl.finalStats.validationIgnored);
    if (!failedStage && sdl.finalStats.validationErrors)
    {
        failedStage = "validation";
        failedReason = "the validation layer reported errors (see the log lines)";
    }
    if (failedStage)
        return Failed(failedStage, failedReason);
    std::printf("d3d9_check=passed\n");
    std::fflush(stdout);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        Usage(argv[0]);
        return 2;
    }
    const int status = Run(options);
    SDL_Quit();
    return status;
}
