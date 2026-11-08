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

#include "vk_legacy_check_shaders.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace front = gfx_vk::d3d9;
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
};

void Usage(const char *argv0)
{
    std::fprintf(stderr, "usage: %s [--validation off|auto|require] [--device N] [--scene NAME]\n", argv0);
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
};

void LogLine(void *, const char *line)
{
    std::printf("d3d9_check.log=%s\n", line);
    std::fflush(stdout);
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
    IDirect3DPixelShader9 *psColor = nullptr, *psTexture = nullptr;
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
    void References();
    void Threads();
    void Queries();
    void ResetDevice();
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
    Equal("msaa_4x", d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES,
                                                     nullptr),
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

// D3D9's reference counts: levels share their texture's count, objects the
// device has bound outlive the program's last reference, the back buffer
// counts as the device.
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

    // The back buffer and the swap chain count as the device.
    const ULONG deviceRefs = device->AddRef() - 1;
    device->Release();
    IDirect3DSurface9 *surface = nullptr;
    Ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface), "GetBackBuffer");
    Equal("back_buffer_counts_device", surface->Release(), deviceRefs);
    IDirect3DSwapChain9 *chain = nullptr;
    Ok(device->GetSwapChain(0, &chain), "GetSwapChain");
    Equal("swap_chain_counts_device", chain->Release(), deviceRefs);
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
    SafeRelease(event);
    SafeRelease(occlusion);
}

// Reset to another back buffer size and back: the new size, default state,
// and drawing at that size.
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
void Check::Present()
{
    Reset(Argb(255, 200, 100, 50));
    const uint64_t before = Counters().presents + Counters().presentsSkipped;
    Ok(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    IDirect3DSwapChain9 *chain = nullptr;
    if (Ok(device->GetSwapChain(0, &chain), "GetSwapChain"))
    {
        Ok(chain->Present(nullptr, nullptr, nullptr, nullptr, 0), "IDirect3DSwapChain9::Present");
        SafeRelease(chain);
    }
    const front::DeviceCounters counters = Counters();
    Equal("presents_counted", counters.presents + counters.presentsSkipped, before + 2);
    presented += counters.presents > 0;
    if (readSize != kSize)
        Read();
    if (Ok(device->GetFrontBufferData(0, readSurface), "GetFrontBufferData") && ReadLocked(kSize))
        Expect("front_buffer", 5, 5, 200, 100, 50);
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

    Check check;
    check.host = &sdl;
    check.d3d = front::CreateDirect3D9(host);

    struct Scene
    {
        const char *name;
        void (Check::*run)();
        bool needsDevice;
    };
    const Scene scenes[] = {
        {"adapter", &Check::Adapter, false},
        {"states", &Check::States, true},
        {"draw", &Check::Draw, true},
        {"render_target", &Check::RenderTarget, true},
        {"references", &Check::References, true},
        {"threads", &Check::Threads, true},
        {"queries", &Check::Queries, true},
        {"reset", &Check::ResetDevice, true},
        {"present", &Check::Present, true},
    };
    const char *failedStage = nullptr;
    std::string failedReason;
    int ran = 0;
    std::string error;
    for (const Scene &scene : scenes)
    {
        if (!options.only.empty() && options.only != scene.name)
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
