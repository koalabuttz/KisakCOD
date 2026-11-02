// KisakCOD-device-probe: creates the Direct3D 9 device through the engine's own
// src/gfx_d3d/r_init.cpp on native Linux (DXVK Native, SDL3) and checks it.
//
// It runs the part of R_InitGraphicsApi that ends with a device, unmodified and
// in the engine's order:
//
//   R_PreCreateWindow   Direct3DCreate9, R_ChooseAdapter (monitor selection),
//                       R_StoreDirect3DCaps (R_PickRenderer picks the shader
//                       path), R_EnumDisplayModes (registers r_mode)
//   R_SetWndParms       window parameters from the dvars
//   R_CreateWindow      CreateWindowExA (src/linux/client/window/win32_window_sdl3.cpp)
//   R_CreateDevice      present parameters, anti-aliasing, IDirect3D9::CreateDevice
//
// and then checks what it got: the shader model 3 path was picked, the device
// is not lost, a clear of the back buffer reads back with the cleared colour,
// and frames present. The device, the window and the Direct3D object are then
// released in R_ShutdownDirect3D's order. R_InitHardware's later steps (zones,
// render targets, Bink, initial state, scene) are not run: they need assets or
// code that is not linked yet, and the probe says so instead of pretending.
//
// No game file is read, nothing is written, and no image leaves the process:
// the read-back is compared in memory. Output is key=value lines on stdout; the
// engine's own messages are printed as they come. Exit status 0 means every
// check passed, 1 a failed stage (the last line names it), 2 a usage error.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <script/scr_stringlist.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_init.h>
#include <linux/d3d9_types.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/window/win32_window_sdl3.h>

#include "device_probe.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

// r_init.cpp defines this without declaring it in r_init.h.
char __cdecl R_CreateWindow(GfxWindowParms *wndParms);

namespace
{
using kisak::device_probe::EngineError;

constexpr int kMinimumSide = 64;
constexpr int kMaximumSide = 8192;
constexpr int kMaximumFrames = 600;

struct Options
{
    int width = 640;
    int height = 480;
    int frames = 3;
};

class StageFailure : public std::runtime_error
{
  public:
    StageFailure(const char *stage, const std::string &reason) : std::runtime_error(reason), stage_(stage)
    {
    }
    const char *Stage() const
    {
        return stage_;
    }

  private:
    const char *stage_;
};

[[noreturn]] void Fail(const char *stage, const std::string &reason)
{
    throw StageFailure(stage, reason);
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08" PRIX32, value);
    return text;
}

bool ParseInt(const char *text, int minimum, int maximum, int &value)
{
    if (!text || !*text)
        return false;
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (*end || parsed < minimum || parsed > maximum)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

int Usage(const char *problem)
{
    if (problem)
        std::fprintf(stderr, "device_probe: %s\n", problem);
    std::fprintf(stderr,
                 "usage: KisakCOD-device-probe [--width %d..%d] [--height %d..%d] [--frames 1..%d]\n"
                 "Creates the D3D9 device through the engine's r_init.cpp (DXVK Native, SDL3 WSI).\n"
                 "Needs libdxvk_d3d9.so and libSDL3.so.0 on the library path and a Vulkan driver.\n",
                 kMinimumSide, kMaximumSide, kMinimumSide, kMaximumSide, kMaximumFrames);
    return 2;
}

// DXVK reads DXVK_WSI_DRIVER when the first device is made. The windows here are
// SDL3 windows, so any other WSI would misread every HWND.
void SelectWsi()
{
    const char *const current = std::getenv("DXVK_WSI_DRIVER");
    if (!current)
    {
        if (setenv("DXVK_WSI_DRIVER", "SDL3", 1) != 0)
            Fail("environment", "cannot set DXVK_WSI_DRIVER=SDL3");
        std::printf("dxvk_wsi_driver=SDL3 (set by the probe)\n");
        return;
    }
    if (std::strcmp(current, "SDL3") != 0)
        Fail("environment", std::string("DXVK_WSI_DRIVER is '") + current +
                                "'; the engine's windows are SDL3 windows, so it must be SDL3 or unset");
    std::printf("dxvk_wsi_driver=SDL3\n");
}

void StartEngineRuntime(const Options &options)
{
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    SL_Init(); // string dvars keep their values in the script string list
    Dvar_Init();
    // Com_InitDvars registers this before the renderer exists (common.cpp:1635);
    // R_RegisterDvars reads it when it registers the worker thread dvars.
    sys_smp_allowed = Dvar_RegisterBool("sys_smp_allowed", Sys_GetCpuCount() > 1u, DVAR_INIT, "Allow multi-threading");
    R_RegisterDvars();
    // Windowed at the requested client size: r_customMode is the engine's own
    // way to ask for a size that is not in the display mode list. Both dvars
    // latch, so the latched values are made current, as R_MakeDedicated does.
    Dvar_SetBool(r_fullscreen, false);
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_fullscreen));
    const std::string mode = std::to_string(options.width) + "x" + std::to_string(options.height);
    Dvar_SetString(r_customMode, mode.c_str());
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_customMode));
    std::printf("engine_runtime=ready dvars=registered r_fullscreen=%d r_customMode=%s r_vsync=%d r_aaSamples=%d\n",
                r_fullscreen->current.enabled ? 1 : 0, r_customMode->current.string,
                r_vsync->current.enabled ? 1 : 0, r_aaSamples->current.integer);
}

void ReportAdapter()
{
    D3DADAPTER_IDENTIFIER9 id{};
    const HRESULT hr = dx.d3d9->GetAdapterIdentifier(dx.adapterIndex, 0, &id);
    if (FAILED(hr))
        Fail("adapter", "GetAdapterIdentifier failed: " + Hex(uint32_t(hr)));
    char description[sizeof(id.Description) + 1]{};
    std::memcpy(description, id.Description, sizeof(id.Description));
    std::printf("adapter_count=%u adapter_index=%u vendor_id=0x%04" PRIX32 " device_id=0x%04" PRIX32
                " description=\"%s\"\n",
                dx.d3d9->GetAdapterCount(), dx.adapterIndex, uint32_t(id.VendorId), uint32_t(id.DeviceId),
                description);
}

void CheckRenderer()
{
    D3DCAPS9 caps{};
    const HRESULT hr = dx.d3d9->GetDeviceCaps(dx.adapterIndex, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr))
        Fail("renderer", "GetDeviceCaps failed: " + Hex(uint32_t(hr)));
    const int renderer = r_rendererInUse->current.integer;
    std::printf("renderer_in_use=%d (%s) pixel_shader=%u.%u vertex_shader=%u.%u max_texture=%u texture_units=%u "
                "display_modes=%d\n",
                renderer, renderer == GFX_RENDERER_SHADER_3 ? "shader model 3" : "not shader model 3",
                unsigned((caps.PixelShaderVersion >> 8) & 0xFF), unsigned(caps.PixelShaderVersion & 0xFF),
                unsigned((caps.VertexShaderVersion >> 8) & 0xFF), unsigned(caps.VertexShaderVersion & 0xFF),
                vidConfig.maxTextureSize, vidConfig.maxTextureMaps, dx.displayModeCount);
    std::printf("metrics anisotropy=%d clip_planes=%d hardware_shadowmap=%d shadowmap_format=%s slope_bias=%d "
                "transparency_msaa=%d\n",
                gfxMetrics.maxAnisotropy, gfxMetrics.maxClipPlanes, gfxMetrics.hasHardwareShadowmap ? 1 : 0,
                Hex(uint32_t(gfxMetrics.shadowmapFormatPrimary)).c_str(), gfxMetrics.slopeScaleDepthBias ? 1 : 0,
                gfxMetrics.hasTransparencyMsaa ? 1 : 0);
    // The game's zones carry shader model 3 shaders only (checked on mp_crash
    // by the world front-end probe), so the device must be on that path.
    if (renderer != GFX_RENDERER_SHADER_3)
        Fail("renderer", "R_PickRenderer did not choose the shader model 3 path");
}

// Clears the back buffer to `colour`, copies it to system memory and checks
// every pixel. Nothing is kept or written anywhere.
void CheckClearReadBack(D3DCOLOR colour, int width, int height)
{
    IDirect3DDevice9 *const device = dx.device;
    HRESULT hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, colour, 1.0f, 0);
    if (FAILED(hr))
        Fail("readback", "Clear failed: " + Hex(uint32_t(hr)));
    IDirect3DSurface9 *backBuffer = nullptr;
    hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer)
        Fail("readback", "GetBackBuffer failed: " + Hex(uint32_t(hr)));
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    IDirect3DSurface9 *copy = nullptr;
    hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
    std::string problem;
    uint32_t mismatched = 0;
    uint32_t firstSeen = 0;
    if (FAILED(hr) || !copy)
        problem = "CreateOffscreenPlainSurface failed: " + Hex(uint32_t(hr));
    else if (FAILED(hr = device->GetRenderTargetData(backBuffer, copy)))
        problem = "GetRenderTargetData failed: " + Hex(uint32_t(hr));
    else
    {
        D3DLOCKED_RECT locked{};
        hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
            problem = "LockRect failed: " + Hex(uint32_t(hr));
        else
        {
            // A8R8G8B8 as R_SetD3DPresentParameters asks; the alpha byte of an
            // X8R8G8B8 surface is undefined, so only colour is compared.
            for (uint32_t y = 0; y < desc.Height; ++y)
            {
                const uint32_t *row =
                    reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch);
                for (uint32_t x = 0; x < desc.Width; ++x)
                {
                    if ((row[x] & 0x00FFFFFFu) != (colour & 0x00FFFFFFu))
                    {
                        if (!mismatched)
                            firstSeen = row[x];
                        ++mismatched;
                    }
                }
            }
            copy->UnlockRect();
        }
    }
    if (copy)
        copy->Release();
    backBuffer->Release();
    std::printf("readback width=%u height=%u format=%s colour=%s mismatched_pixels=%" PRIu32 "\n", desc.Width,
                desc.Height, Hex(uint32_t(desc.Format)).c_str(), Hex(colour).c_str(), mismatched);
    if (!problem.empty())
        Fail("readback", problem);
    if (int(desc.Width) != width || int(desc.Height) != height)
        Fail("readback", "the back buffer is not the requested window size");
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
        Fail("readback", "the back buffer is not 32-bit RGB");
    if (mismatched)
        Fail("readback", std::to_string(mismatched) + " pixel(s) differ from the clear colour, first " +
                             Hex(firstSeen));
}

void PresentFrames(int frames)
{
    static const D3DCOLOR colours[] = {0xFF203040u, 0xFF406020u, 0xFF602040u};
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame)
    {
        SDL_PumpEvents();
        const D3DCOLOR colour = colours[frame % 3];
        HRESULT hr = dx.device->Clear(0, nullptr, D3DCLEAR_TARGET, colour, 1.0f, 0);
        if (SUCCEEDED(hr))
            hr = dx.device->BeginScene();
        if (SUCCEEDED(hr))
            hr = dx.device->EndScene();
        if (SUCCEEDED(hr))
            hr = dx.device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr))
            Fail("present", "frame " + std::to_string(frame) + " failed: " + Hex(uint32_t(hr)));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("present frames=%d seconds=%.3f\n", frames, seconds);
}

// R_ShutdownDirect3D's order for what was created: device, windows, Direct3D.
// It is not called itself because it also releases the render targets and Bink
// state that R_InitHardware would have made.
void ReleaseDevice(GfxWindowParms &window)
{
    ULONG deviceRefs = 0;
    ULONG d3dRefs = 0;
    if (dx.device)
    {
        deviceRefs = dx.device->Release();
        dx.device = nullptr;
    }
    const BOOL destroyed = window.hwnd ? DestroyWindow(window.hwnd) : FALSE;
    window.hwnd = nullptr;
    if (dx.d3d9)
    {
        d3dRefs = dx.d3d9->Release();
        dx.d3d9 = nullptr;
    }
    std::printf("shutdown device_refs_left=%lu window_destroyed=%d d3d9_refs_left=%lu open_windows=%zu\n",
                static_cast<unsigned long>(deviceRefs), destroyed ? 1 : 0, static_cast<unsigned long>(d3dRefs),
                kisak::win32_window::OpenWindowCount());
    if (deviceRefs || d3dRefs || kisak::win32_window::OpenWindowCount())
        Fail("shutdown", "something outlived the probe's release");
}

void Run(const Options &options)
{
    SelectWsi();
    StartEngineRuntime(options);

    if (!R_PreCreateWindow())
        Fail("direct3d", "R_PreCreateWindow failed: Direct3DCreate9 returned no interface "
                         "(no DXVK library, no Vulkan driver, or no adapter)");
    ReportAdapter();
    CheckRenderer();

    GfxWindowParms window{};
    R_SetWndParms(&window);
    std::printf("window_parms width=%d height=%d fullscreen=%d x=%d y=%d hz=%d aa_samples=%d\n", window.displayWidth,
                window.displayHeight, window.fullscreen ? 1 : 0, window.x, window.y, window.hz, window.aaSamples);
    if (window.fullscreen || window.displayWidth != options.width || window.displayHeight != options.height)
        Fail("window", "R_SetWndParms did not take the requested windowed size (is it larger than the monitor?)");
    if (!R_CreateWindow(&window))
        Fail("window", "R_CreateWindow failed (no Vulkan-capable SDL video driver?)");
    std::printf("window=created\n");

    try
    {
        if (!R_CreateDevice(&window))
            Fail("device", "R_CreateDevice failed");
        const HRESULT level = dx.device->TestCooperativeLevel();
        std::printf("device=created adapter=%u multisample=%d depth_stencil_format=%s native_size=%dx%d "
                    "cooperative_level=%s\n",
                    dx.adapterIndex, int(dx.multiSampleType), Hex(uint32_t(dx.depthStencilFormat)).c_str(),
                    dx.adapterNativeWidth, dx.adapterNativeHeight, Hex(uint32_t(level)).c_str());
        if (level != D3D_OK)
            Fail("device", "the new device is not ready: " + Hex(uint32_t(level)));
        CheckClearReadBack(0xFF336699u, options.width, options.height);
        PresentFrames(options.frames);
    }
    catch (...)
    {
        ReleaseDevice(window);
        throw;
    }
    ReleaseDevice(window);
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const char *const arg = argv[index];
        const char *const value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (!std::strcmp(arg, "--help"))
            return Usage(nullptr);
        if (!std::strcmp(arg, "--width") && ParseInt(value, kMinimumSide, kMaximumSide, options.width))
            ++index;
        else if (!std::strcmp(arg, "--height") && ParseInt(value, kMinimumSide, kMaximumSide, options.height))
            ++index;
        else if (!std::strcmp(arg, "--frames") && ParseInt(value, 1, kMaximumFrames, options.frames))
            ++index;
        else
            return Usage((std::string("bad argument ") + arg).c_str());
    }

    std::printf("device_probe=start width=%d height=%d frames=%d\n", options.width, options.height, options.frames);
    std::fflush(stdout);
    int status = 0;
    try
    {
        Run(options);
        std::printf("device_probe=passed renderer=shader_model_3 frames=%d\n", options.frames);
    }
    catch (const StageFailure &failure)
    {
        std::printf("device_probe=failed stage=%s reason=%s\n", failure.Stage(), failure.what());
        status = 1;
    }
    catch (const EngineError &error)
    {
        std::printf("device_probe=failed stage=engine reason=Com_Error: %s\n", error.what());
        status = 1;
    }
    catch (const std::exception &error)
    {
        std::printf("device_probe=failed stage=internal reason=%s\n", error.what());
        status = 1;
    }
    std::fflush(nullptr);
    return status;
}
