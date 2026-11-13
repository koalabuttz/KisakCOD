// The D3D9 front: IDirect3D9 (adapters, modes, caps, formats, device creation).
// See vk_d3d9.h.

#include "vk_d3d9_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace gfx_vk::d3d9
{
bool OffersSamples(D3DMULTISAMPLE_TYPE type)
{
#if defined(KISAK_GFX_GXM)
    return type == D3DMULTISAMPLE_NONE;
#else
    return type == D3DMULTISAMPLE_NONE || type == D3DMULTISAMPLE_2_SAMPLES || type == D3DMULTISAMPLE_4_SAMPLES
        || type == D3DMULTISAMPLE_8_SAMPLES;
#endif
}

namespace
{
std::vector<Device *> &LiveDevices()
{
    static std::vector<Device *> devices;
    return devices;
}

bool IsDepthFormat(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D32:
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D16:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
        return true;
    default:
        return false;
    }
}
} // namespace

void HostLog(const Host &host, const char *format, ...)
{
    if (!host.log)
        return;
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    host.log(host.user, line);
}

#if !defined(__vita__)
namespace
{
// Tells the processor the loop is a wait (and lets a sibling hyperthread run).
inline void CpuRelax()
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

bool SpinDisabled()
{
#if defined(_MSC_VER)
    char *text = nullptr;
    size_t length = 0;
    const bool off = _dupenv_s(&text, &length, "KISAK_VK_FRONT_SPIN") == 0 && text && std::strcmp(text, "0") == 0;
    std::free(text);
    return off;
#else
    const char *text = std::getenv("KISAK_VK_FRONT_SPIN");
    return text && std::strcmp(text, "0") == 0;
#endif
}

// FrontLockSlept's counts.
std::atomic<uint64_t> g_frontSleeps{0};
std::atomic<uint64_t> g_frontSleepMicros{0};

// What a waiter sleeps on. Made by the first waiter and never destroyed.
struct FrontSleep
{
    std::mutex mutex;
    std::condition_variable wake;
};

FrontSleep &Sleep()
{
    static FrontSleep *const sleep = new FrontSleep;
    return *sleep;
}

uint32_t Spins()
{
    static const uint32_t spins = SpinDisabled() ? 0 : 2000;
    return spins;
}
} // namespace

void FrontMutexType::LockSlow()
{
    const uint32_t spins = Spins();
    for (uint32_t spin = 0; spin < spins; ++spin)
    {
        CpuRelax();
        // Looks before it tries: a failed try still takes the lock's cache
        // line from the thread that will unlock it.
        uint32_t free = kFree;
        if (state_.load(std::memory_order_relaxed) == kFree &&
            state_.compare_exchange_weak(free, kHeld, std::memory_order_acquire, std::memory_order_relaxed))
            return;
    }
    // Marks the lock as having sleepers, so its holder wakes one when it
    // leaves; a waiter that takes it this way leaves the mark, which costs at
    // most one wake nobody needed. The holder takes Sleep().mutex before it
    // wakes anyone, so the mark and the wait cannot miss each other.
    FrontSleep &sleep = Sleep();
    const auto start = std::chrono::steady_clock::now();
    uint64_t sleeps = 0;
    {
        std::unique_lock<std::mutex> guard(sleep.mutex);
        while (state_.exchange(kSleepers, std::memory_order_acquire) != kFree)
        {
            ++sleeps;
            sleep.wake.wait(guard);
        }
    }
    if (sleeps)
    {
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        g_frontSleeps.fetch_add(sleeps, std::memory_order_relaxed);
        g_frontSleepMicros.fetch_add(static_cast<uint64_t>(micros), std::memory_order_relaxed);
    }
}

FrontLockSleeps FrontLockSlept()
{
    FrontLockSleeps slept;
    slept.sleeps = g_frontSleeps.load(std::memory_order_relaxed);
    slept.micros = g_frontSleepMicros.load(std::memory_order_relaxed);
    return slept;
}

void FrontMutexType::WakeOne()
{
    FrontSleep &sleep = Sleep();
    {
        std::lock_guard<std::mutex> guard(sleep.mutex);
    }
    sleep.wake.notify_one();
}
#else
FrontLockSleeps FrontLockSlept()
{
    return {}; // a plain recursive mutex here: its sleeps are not seen
}

FrontMutexType &FrontMutex()
{
    // Never destroyed: a device released from a static destructor still locks it.
    static FrontMutexType *const mutex = new FrontMutexType;
    return *mutex;
}
#endif

IDirect3D9 *CreateDirect3D9(const Host &host)
{
    return new Direct3D(host);
}

void RegisterDevice(Device *device, bool live)
{
    const FrontLock lock;
    std::vector<Device *> &devices = LiveDevices();
    if (live)
        devices.push_back(device);
    else
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i] == device)
            {
                devices.erase(devices.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
}

uint32_t WindowDestroying(D3D9Window window)
{
    const FrontLock lock;
    uint32_t count = 0;
    for (Device *live : LiveDevices())
        if (live->window() == window)
        {
            live->DetachWindow();
            ++count;
        }
    return count;
}

uint32_t LogDeviceStats(bool reset)
{
    const FrontLock lock;
    uint32_t count = 0;
    for (Device *live : LiveDevices())
    {
        live->LogStats(reset);
        ++count;
    }
    return count;
}

bool Counters(IDirect3DDevice9 *device, DeviceCounters &out)
{
    const FrontLock lock;
    for (Device *live : LiveDevices())
        if (static_cast<IDirect3DDevice9 *>(live) == device)
        {
            out = live->counters();
            return true;
        }
    return false;
}

ModernResult ModernSceneBegin(IDirect3DDevice9 *device)
{
    const FrontLock lock;
    for (Device *live : LiveDevices())
        if (static_cast<IDirect3DDevice9 *>(live) == device)
            return live->ModernSceneBegin();
    return ModernResult::Refused;
}

ModernResult ModernSceneLit(IDirect3DDevice9 *device, const ModernSceneView &view, bool *glowDrawn)
{
    const FrontLock lock;
    for (Device *live : LiveDevices())
        if (static_cast<IDirect3DDevice9 *>(live) == device)
            return live->ModernSceneLit(view, glowDrawn);
    return ModernResult::Refused;
}

ModernResult ModernSceneDone(IDirect3DDevice9 *device, const ModernSceneView &view, bool *glowDrawn)
{
    const FrontLock lock;
    for (Device *live : LiveDevices())
        if (static_cast<IDirect3DDevice9 *>(live) == device)
            return live->ModernSceneDone(view, glowDrawn);
    return ModernResult::Refused;
}

// ---- IUnknown -------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Direct3D::QueryInterface(REFIID, void **object)
{
    const FrontLock lock;
    if (object)
        *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Direct3D::Release()
{
    const FrontLock lock;
    const ULONG refs = --refs_;
    if (!refs)
        delete this;
    return refs;
}

// ---- Adapters and modes -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Direct3D::RegisterSoftwareDevice(void *)
{
    const FrontLock lock;
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3D::GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9 *id)
{
    const FrontLock lock;
    if (adapter != 0 || !id)
        return D3DERR_INVALIDCALL;
    std::memset(id, 0, sizeof(*id));
    // The GPU is only chosen at CreateDevice, when there is a window to present
    // to; the renderer only compares the description with NVPerfHUD's.
    std::snprintf(id->Driver, sizeof(id->Driver), "%s", "kisak_vulkan_legacy");
    std::snprintf(id->Description, sizeof(id->Description), "%s", "KisakCOD Vulkan Legacy adapter");
    std::snprintf(id->DeviceName, sizeof(id->DeviceName), "%s", "\\\\.\\DISPLAY1");
    id->WHQLLevel = 1;
    return D3D_OK;
}

bool Direct3D::Mode(UINT index, D3DDISPLAYMODE &mode) const
{
    uint32_t width = 0, height = 0, refresh = 0;
    bool ok = false;
    if (host_.modeCount && host_.mode)
        ok = host_.mode(host_.user, index, width, height, refresh);
    else if (index == 0 && host_.currentMode)
        ok = host_.currentMode(host_.user, width, height, refresh);
    if (!ok)
        return false;
    mode.Width = width;
    mode.Height = height;
    mode.RefreshRate = refresh;
    mode.Format = D3DFMT_X8R8G8B8;
    return true;
}

UINT STDMETHODCALLTYPE Direct3D::GetAdapterModeCount(UINT adapter, D3DFORMAT format)
{
    const FrontLock lock;
    if (adapter != 0 || format != D3DFMT_X8R8G8B8)
        return 0;
    if (host_.modeCount && host_.mode)
        return host_.modeCount(host_.user);
    return host_.currentMode ? 1 : 0;
}

HRESULT STDMETHODCALLTYPE Direct3D::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE *mode)
{
    const FrontLock lock;
    if (adapter != 0 || format != D3DFMT_X8R8G8B8 || !mode || !Mode(index, *mode))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3D::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode)
{
    const FrontLock lock;
    if (adapter != 0 || !mode)
        return D3DERR_INVALIDCALL;
    uint32_t width = 0, height = 0, refresh = 0;
    if (!host_.currentMode || !host_.currentMode(host_.user, width, height, refresh))
        return D3DERR_NOTAVAILABLE;
    mode->Width = width;
    mode->Height = height;
    mode->RefreshRate = refresh;
    mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE Direct3D::GetAdapterMonitor(UINT adapter)
{
    const FrontLock lock;
    if (adapter != 0 || !host_.monitor)
        return nullptr;
    return host_.monitor(host_.user);
}

// ---- Formats --------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Direct3D::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat,
                                                    D3DFORMAT backBufferFormat, BOOL)
{
    const FrontLock lock;
    if (adapter != 0 || type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    if (displayFormat != D3DFMT_X8R8G8B8 && displayFormat != D3DFMT_UNKNOWN)
        return D3DERR_NOTAVAILABLE;
    if (backBufferFormat != D3DFMT_X8R8G8B8 && backBufferFormat != D3DFMT_A8R8G8B8
        && backBufferFormat != D3DFMT_UNKNOWN)
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

// Formats are checked before any device exists, so the answer is the
// adapter's format table (every Vulkan 1.3 desktop GPU has these formats),
// not the GPU's own format list.
HRESULT STDMETHODCALLTYPE Direct3D::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT, DWORD usage,
                                                      D3DRESOURCETYPE resourceType, D3DFORMAT format)
{
    const FrontLock lock;
    if (adapter != 0 || type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    if (resourceType == D3DRTYPE_VERTEXBUFFER || resourceType == D3DRTYPE_INDEXBUFFER)
        return D3D_OK;
    if (usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_QUERY_FILTER
                 | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING | D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_WRAPANDMIP))
    {
        // The adapter samples none of its formats as sRGB.
        if (usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE))
            return D3DERR_NOTAVAILABLE;
    }
    const uint32_t legacyUsage = usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL);
    if (resourceType == D3DRTYPE_VOLUMETEXTURE && (legacyUsage || IsDepthFormat(format)))
        return D3DERR_NOTAVAILABLE;
    if (resourceType == D3DRTYPE_CUBETEXTURE && IsDepthFormat(format))
        return D3DERR_NOTAVAILABLE;
    return lg::FormatSupported(nullptr, static_cast<lg::d3d::Format>(format), legacyUsage) ? D3D_OK
                                                                                           : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE Direct3D::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT, BOOL,
                                                               D3DMULTISAMPLE_TYPE samples, DWORD *qualityLevels)
{
    const FrontLock lock;
    if (adapter != 0 || type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    // Asked before any device exists, so the answer is the counts the front
    // offers on every GPU, for every format (OffersSamples); a GPU without
    // the count gets 4 samples, which every Vulkan GPU has. One quality level.
    if (!OffersSamples(samples))
        return D3DERR_NOTAVAILABLE;
    if (qualityLevels)
        *qualityLevels = 1;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3D::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT,
                                                           D3DFORMAT renderTargetFormat, D3DFORMAT depthFormat)
{
    const FrontLock lock;
    if (adapter != 0 || type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    if (!lg::FormatSupported(nullptr, static_cast<lg::d3d::Format>(renderTargetFormat), lg::d3d::USAGE_RENDERTARGET)
        || !lg::FormatSupported(nullptr, static_cast<lg::d3d::Format>(depthFormat), lg::d3d::USAGE_DEPTHSTENCIL))
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Direct3D::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src,
                                                                D3DFORMAT dst)
{
    const FrontLock lock;
    if (adapter != 0 || type != D3DDEVTYPE_HAL)
        return D3DERR_INVALIDCALL;
    return src == dst ? D3D_OK : D3DERR_NOTAVAILABLE;
}

// ---- Caps -----------------------------------------------------------------------------------------

// The values DXVK 2.7.1 reports (src/d3d9/d3d9_adapter.cpp, zlib licence) for
// shader model 3, so the renderer takes the same paths on both back ends.
void FillCaps(D3DCAPS9 &caps)
{
    std::memset(&caps, 0, sizeof(caps));
    caps.DeviceType = D3DDEVTYPE_HAL;
    caps.AdapterOrdinal = 0;
    caps.Caps = D3DCAPS_READ_SCANLINE;
    caps.Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
    caps.Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION
        | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    caps.PresentationIntervals = D3DPRESENT_INTERVAL_DEFAULT | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO
        | D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR | D3DPRESENT_INTERVAL_IMMEDIATE;
    caps.CursorCaps = D3DCURSORCAPS_COLOR;
    caps.DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY
        | D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX
        | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2
        | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANBLTSYSTONONLOCAL
        | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    caps.PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW
        | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_CLIPTLVERTS
        | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS
        | D3DPMISCCAPS_PERSTAGECONSTANT | D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_SEPARATEALPHABLEND
        | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING
        | D3DPMISCCAPS_FOGVERTEXCLAMPED | D3DPMISCCAPS_POSTBLENDSRGBCONVERT;
    caps.RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE
        | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG
        | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST
        | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    caps.ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL
        | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    caps.SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR
        | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA
        | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT
        | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
    caps.DestBlendCaps = caps.SrcBlendCaps;
    caps.AlphaCmpCaps = caps.ZCmpCaps;
    caps.ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB
        | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
    caps.TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE
        | D3DPTEXTURECAPS_ALPHAPALETTE | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP
        | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP
        | D3DPTEXTURECAPS_MIPCUBEMAP;
    caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC
        | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
        | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    caps.CubeTextureFilterCaps = caps.TextureFilterCaps;
    caps.VolumeTextureFilterCaps = caps.TextureFilterCaps;
    caps.TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP
        | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    caps.VolumeTextureAddressCaps = caps.TextureAddressCaps;
    caps.LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP
        | D3DLINECAPS_FOG | D3DLINECAPS_ANTIALIAS;
    caps.MaxTextureWidth = 16384;
    caps.MaxTextureHeight = 16384;
    caps.MaxVolumeExtent = 8192;
    caps.MaxTextureRepeat = 8192;
    caps.MaxTextureAspectRatio = 8192;
    caps.MaxAnisotropy = 16;
    caps.MaxVertexW = 1e10f;
    caps.GuardBandLeft = -32768.0f;
    caps.GuardBandTop = -32768.0f;
    caps.GuardBandRight = 32768.0f;
    caps.GuardBandBottom = 32768.0f;
    caps.ExtentsAdjust = 0.0f;
    caps.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT
        | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR
        | D3DSTENCILCAPS_TWOSIDED;
    const DWORD maxSimultaneousTextures = 8;
    caps.FVFCaps = (maxSimultaneousTextures & D3DFVFCAPS_TEXCOORDCOUNTMASK) | D3DFVFCAPS_PSIZE;
    caps.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2
        | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD
        | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH
        | D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA
        | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_PREMODULATE
        | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA
        | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA
        | D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE | D3DTEXOPCAPS_DOTPRODUCT3
        | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP;
    caps.MaxTextureBlendStages = maxSimultaneousTextures;
    caps.MaxSimultaneousTextures = maxSimultaneousTextures;
    caps.VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS
        | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TWEENING
        | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
    caps.MaxActiveLights = 8;
    caps.MaxUserClipPlanes = 6;
    caps.MaxVertexBlendMatrices = 4;
    caps.MaxVertexBlendMatrixIndex = 0;
    caps.MaxPointSize = 256.0f;
    caps.MaxPrimitiveCount = 0x00555555;
    caps.MaxVertexIndex = 0x00ffffff;
    caps.MaxStreams = 16;
    caps.MaxStreamStride = 508;
    caps.VertexShaderVersion = D3DVS_VERSION(3, 0);
    caps.PixelShaderVersion = D3DPS_VERSION(3, 0);
    caps.MaxVertexShaderConst = 256;
    caps.PixelShader1xMaxValue = std::numeric_limits<float>::max();
    caps.DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES
        | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
    caps.MaxNpatchTessellationLevel = 0.0f;
    caps.MasterAdapterOrdinal = 0;
    caps.AdapterOrdinalInGroup = 0;
    caps.NumberOfAdaptersInGroup = 1;
    caps.DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N
        | D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    caps.NumSimultaneousRTs = lg::kMaxRenderTargets;
    caps.StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
        | D3DPTFILTERCAPS_MAGFLINEAR;
    caps.VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    caps.VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    caps.VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
    caps.VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
    caps.PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION
        | D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    caps.PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    caps.PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
    caps.PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    caps.PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
    caps.VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
        | D3DPTFILTERCAPS_MAGFLINEAR;
    caps.MaxVShaderInstructionsExecuted = 4294967295u;
    caps.MaxPShaderInstructionsExecuted = 4294967295u;
    caps.MaxVertexShader30InstructionSlots = 32768;
    caps.MaxPixelShader30InstructionSlots = 32768;
}

HRESULT STDMETHODCALLTYPE Direct3D::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps)
{
    const FrontLock lock;
    if (adapter != 0 || !caps)
        return D3DERR_INVALIDCALL;
    if (type != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    FillCaps(*caps);
    return D3D_OK;
}

// ---- Device creation ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Direct3D::CreateDevice(UINT adapter, D3DDEVTYPE type, D3D9Window focusWindow, DWORD behavior,
                                                 D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **out)
{
    const FrontLock lock;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (adapter != 0 || type != D3DDEVTYPE_HAL || !params)
        return D3DERR_INVALIDCALL;
    if (!OffersSamples(params->MultiSampleType))
    {
        HostLog(host_, "vulkan d3d9: CreateDevice: no back buffer of multisample type %u",
                unsigned(params->MultiSampleType));
        return D3DERR_NOTAVAILABLE;
    }
    D3D9Window window = params->hDeviceWindow ? static_cast<D3D9Window>(params->hDeviceWindow) : focusWindow;
    if (!window)
        return D3DERR_INVALIDCALL;
    Device *device = new Device(this, *params, window, behavior);
    if (!device->Init())
    {
        device->Release();
        return D3DERR_NOTAVAILABLE;
    }
    RegisterDevice(device, true);
    *out = device;
    return D3D_OK;
}
} // namespace gfx_vk::d3d9
