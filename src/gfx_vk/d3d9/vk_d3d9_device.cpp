// The D3D9 front: IDirect3DDevice9 on the Legacy adapter. See vk_d3d9.h.

#include "vk_d3d9_internal.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace gfx_vk::d3d9
{
namespace
{
constexpr uint32_t kVsFloatConstants = 256;
constexpr uint32_t kPsFloatConstants = 224;
constexpr uint32_t kIntConstants = 16;
constexpr uint32_t kBoolConstants = 16;
// The usage bits the adapter acts on; the rest (WRITEONLY aside) are hints.
constexpr DWORD kAdapterUsage = D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC;

lg::Rect ToRect(const RECT &rect)
{
    return lg::Rect{static_cast<int32_t>(rect.left), static_cast<int32_t>(rect.top), static_cast<int32_t>(rect.right),
                    static_cast<int32_t>(rect.bottom)};
}

// The byte length of D3D9 shader bytecode, up to and including the end token;
// 0 when there is none within a sane length.
size_t ShaderBytes(const DWORD *code)
{
    constexpr size_t kMaxTokens = 1u << 20;
    const uint32_t major = (code[0] >> 8) & 0xff;
    size_t i = 1;
    while (i < kMaxTokens)
    {
        const uint32_t token = code[i];
        if (token == 0x0000ffffu)
            return (i + 1) * sizeof(DWORD);
        if ((token & 0xffffu) == 0xfffeu) // comment: length in bits 16-30
            i += 1 + ((token >> 16) & 0x7fffu);
        else if (major >= 2) // instruction length in bits 24-27
            i += 1 + ((token >> 24) & 0x0fu);
        else // shader model 1 has no lengths; parameter tokens never look like the end token
            ++i;
    }
    return 0;
}
} // namespace

int Device::SamplerSlot(DWORD sampler)
{
    if (sampler < 16)
        return static_cast<int>(sampler);
    if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
        return static_cast<int>(16 + sampler - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

template <class T>
void Device::Rebind(T *&slot, T *object)
{
    if (slot == object)
        return;
    if (object)
        object->Bind();
    T *old = slot;
    slot = object;
    if (old)
        old->Unbind();
}

// ---- Life ------------------------------------------------------------------------------------------

Device::Device(Direct3D *parent, const D3DPRESENT_PARAMETERS &params, D3D9Window window, DWORD behavior)
    : params_(params), parent_(parent), window_(window), behavior_(behavior)
{
    parent_->AddRef();
    vsConstantsF_.assign(kVsFloatConstants * 4, 0.0f);
    psConstantsF_.assign(kPsFloatConstants * 4, 0.0f);
    vsConstantsI_.assign(kIntConstants * 4, 0);
    psConstantsI_.assign(kIntConstants * 4, 0);
    vsConstantsB_.assign(kBoolConstants, FALSE);
    psConstantsB_.assign(kBoolConstants, FALSE);
    for (UINT i = 0; i < 256; ++i)
    {
        const WORD value = static_cast<WORD>(i * 257);
        gamma_.red[i] = gamma_.green[i] = gamma_.blue[i] = value;
    }
}

Device::~Device()
{
    Destroy();
}

bool Device::Init()
{
    const Host &h = host();
    if (!h.windowSystem || !h.windowSystem(h.user, window_, windowSystem_))
    {
        HostLog(h, "vulkan d3d9: CreateDevice: the host has no Vulkan window system for this window");
        return false;
    }

    CoreDesc desc;
    desc.validation = h.validation;
    desc.deviceIndex = h.deviceIndex;
    desc.vsync = params_.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    desc.pipelineCachePath = h.pipelineCachePath;
    desc.log = h.log;
    desc.logUser = h.user;
    char error[512] = {};
    core_ = CreateCore(desc, windowSystem_, error, sizeof(error));
    if (!core_)
    {
        HostLog(h, "vulkan d3d9: CreateDevice: %s", error);
        return false;
    }

    lg::DeviceDesc legacyDesc;
    legacyDesc.width = params_.BackBufferWidth;
    legacyDesc.height = params_.BackBufferHeight;
    legacyDesc.log = h.log;
    legacyDesc.logUser = h.user;
    std::string legacyError;
    legacy_ = lg::CreateDevice(core_, legacyDesc, legacyError);
    if (!legacy_)
    {
        HostLog(h, "vulkan d3d9: CreateDevice: %s", legacyError.c_str());
        return false;
    }

    backBuffer_.reset(new Surface(this, lg::BackBuffer(legacy_)));
    swapChain_.reset(new SwapChain(this));

    uint32_t width = 0, height = 0;
    lg::d3d::Format format = lg::d3d::FMT_UNKNOWN;
    lg::SurfaceSize(backBuffer_->handle(), width, height, format);
    params_.BackBufferWidth = width;
    params_.BackBufferHeight = height;
    if (params_.BackBufferFormat == D3DFMT_UNKNOWN)
        params_.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!params_.BackBufferCount)
        params_.BackBufferCount = 1;
    if (!params_.hDeviceWindow)
        params_.hDeviceWindow = window_;
    if (params_.EnableAutoDepthStencil)
        HostLog(h, "vulkan d3d9: CreateDevice: no automatic depth-stencil surface (the renderer creates its own)");

    const DeviceInfo &info = gfx_vk::Device(core_);
    HostLog(h, "vulkan d3d9: device %s, back buffer %ux%u, vsync %s", info.name, width, height,
            desc.vsync ? "on" : "off");
    DefaultState();
    return true;
}

void Device::ObjectDestroyed()
{
    --objects_;
    if (!objects_ && !refs_ && !releasing_)
        delete this;
}

ULONG STDMETHODCALLTYPE Device::Release()
{
    if (!refs_)
        return 0;
    const ULONG refs = --refs_;
    if (refs)
        return refs;
    // The device's own holds go with its last reference, as in D3D9; objects
    // the program still holds keep the device until they go.
    releasing_ = true;
    UnbindAll();
    releasing_ = false;
    if (!objects_ && !refs_)
        delete this;
    return 0;
}

HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9))
    {
        *object = static_cast<IDirect3DDevice9 *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

void Device::UnbindAll()
{
    for (Surface *&target : renderTargets_)
        Rebind(target, static_cast<Surface *>(nullptr));
    Rebind(depthStencil_, static_cast<Surface *>(nullptr));
    for (uint32_t slot = 0; slot < kSamplerSlots; ++slot)
    {
        textures_[slot] = nullptr;
        Rebind(textureObjects_[slot], static_cast<Counted *>(nullptr));
    }
    for (Stream &stream : streams_)
    {
        Rebind(stream.buffer, static_cast<VertexBuffer *>(nullptr));
        stream = Stream{};
    }
    Rebind(indices_, static_cast<IndexBuffer *>(nullptr));
    Rebind(vertexShader_, static_cast<VertexShader *>(nullptr));
    Rebind(pixelShader_, static_cast<PixelShader *>(nullptr));
    Rebind(declaration_, static_cast<VertexDeclaration *>(nullptr));
}

void Device::DefaultState()
{
    UnbindAll();
    if (legacy_)
    {
        lg::ResetState(legacy_);
        // D3D9 turns the depth test on by default only with an automatic
        // depth-stencil surface, which the front never makes.
        lg::SetRenderState(legacy_, lg::d3d::RS_ZENABLE, params_.EnableAutoDepthStencil ? 1 : 0);
    }
    Rebind(renderTargets_[0], backBuffer_.get());
    for (uint32_t slot = 0; slot < kSamplerSlots; ++slot)
    {
        DWORD *ss = samplerStates_[slot];
        std::fill(ss, ss + lg::d3d::SAMP_COUNT, 0u);
        ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[D3DSAMP_MAXANISOTROPY] = 1;
    }
    std::fill(vsConstantsF_.begin(), vsConstantsF_.end(), 0.0f);
    std::fill(psConstantsF_.begin(), psConstantsF_.end(), 0.0f);
    std::fill(vsConstantsI_.begin(), vsConstantsI_.end(), 0);
    std::fill(psConstantsI_.begin(), psConstantsI_.end(), 0);
    std::fill(vsConstantsB_.begin(), vsConstantsB_.end(), FALSE);
    std::fill(psConstantsB_.begin(), psConstantsB_.end(), FALSE);
    viewport_ = D3DVIEWPORT9{0, 0, params_.BackBufferWidth, params_.BackBufferHeight, 0.0f, 1.0f};
    scissor_ = RECT{0, 0, static_cast<LONG>(params_.BackBufferWidth), static_cast<LONG>(params_.BackBufferHeight)};
}

void Device::Destroy()
{
    RegisterDevice(this, false);
    swapChain_.reset();
    backBuffer_.reset();
    if (legacy_)
    {
        lg::DestroyDevice(legacy_);
        legacy_ = nullptr;
    }
    if (core_)
    {
        CoreStats final{};
        DestroyCore(core_, &final);
        core_ = nullptr;
        if (parent_ && host().coreDestroyed)
            host().coreDestroyed(host().user, final);
    }
    if (parent_)
    {
        parent_->Release();
        parent_ = nullptr;
    }
}

DeviceCounters Device::counters() const
{
    DeviceCounters out = counters_;
    out.liveObjects = objects_;
    out.validationErrors = core_ ? Stats(core_).validationErrors : 0;
    out.validation = core_ && ValidationEnabled(core_);
    return out;
}

HRESULT Device::Unsupported(const char *method)
{
    ++counters_.unsupportedCalls;
    if (std::find(unsupportedLogged_.begin(), unsupportedLogged_.end(), method) == unsupportedLogged_.end())
    {
        unsupportedLogged_.push_back(method);
        HostLog(host(), "vulkan d3d9: %s is not supported (answered D3DERR_INVALIDCALL; logged once)", method);
    }
    return D3DERR_INVALIDCALL;
}

// ---- Device and display ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::TestCooperativeLevel()
{
    return D3D_OK;
}

UINT STDMETHODCALLTYPE Device::GetAvailableTextureMem()
{
    // Device-local memory, as DXVK reports it, kept below 4 GB.
    VkPhysicalDeviceMemoryProperties memory{};
    Functions(core_).vkGetPhysicalDeviceMemoryProperties(PhysicalDeviceHandle(core_), &memory);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            bytes += memory.memoryHeaps[i].size;
    return static_cast<UINT>(std::min<uint64_t>(bytes, 0xfff00000u));
}

HRESULT STDMETHODCALLTYPE Device::GetDirect3D(IDirect3D9 **d3d)
{
    if (!d3d)
        return D3DERR_INVALIDCALL;
    parent_->AddRef();
    *d3d = parent_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetDeviceCaps(D3DCAPS9 *caps)
{
    if (!caps)
        return D3DERR_INVALIDCALL;
    FillCaps(*caps);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetDisplayMode(UINT swapChain, D3DDISPLAYMODE *mode)
{
    if (swapChain != 0)
        return D3DERR_INVALIDCALL;
    return parent_->GetAdapterDisplayMode(0, mode);
}

HRESULT STDMETHODCALLTYPE Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    params->AdapterOrdinal = 0;
    params->DeviceType = D3DDEVTYPE_HAL;
    params->hFocusWindow = window_;
    params->BehaviorFlags = behavior_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetCursorProperties(UINT, UINT, IDirect3DSurface9 *)
{
    return Unsupported("IDirect3DDevice9::SetCursorProperties");
}

HRESULT STDMETHODCALLTYPE Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **swapChain)
{
    if (swapChain)
        *swapChain = nullptr;
    return Unsupported("IDirect3DDevice9::CreateAdditionalSwapChain");
}

HRESULT STDMETHODCALLTYPE Device::GetSwapChain(UINT index, IDirect3DSwapChain9 **swapChain)
{
    if (!swapChain)
        return D3DERR_INVALIDCALL;
    *swapChain = nullptr;
    if (index != 0)
        return D3DERR_INVALIDCALL;
    swapChain_->AddRef();
    *swapChain = swapChain_.get();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::Reset(D3DPRESENT_PARAMETERS *params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    if (params->MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        HostLog(host(), "vulkan d3d9: Reset: multisampled back buffers are not supported");
        return D3DERR_NOTAVAILABLE;
    }
    if ((params->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)
        != (params_.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE))
        HostLog(host(), "vulkan d3d9: Reset: the presentation interval changes only with a new device");

    uint32_t width = params->BackBufferWidth, height = params->BackBufferHeight;
    if ((!width || !height) && windowSystem_.getPixelSize)
    {
        uint32_t windowWidth = 0, windowHeight = 0;
        windowSystem_.getPixelSize(windowSystem_.user, &windowWidth, &windowHeight);
        width = width ? width : windowWidth;
        height = height ? height : windowHeight;
    }
    if (!width || !height)
        return D3DERR_INVALIDCALL;

    UnbindAll();
    if (width != params_.BackBufferWidth || height != params_.BackBufferHeight)
    {
        if (!lg::ResizeBackBuffer(legacy_, width, height))
        {
            HostLog(host(), "vulkan d3d9: Reset: cannot make a %ux%u back buffer", width, height);
            return D3DERR_INVALIDCALL;
        }
    }
    const D3DPRESENT_PARAMETERS old = params_;
    params_ = *params;
    params_.BackBufferWidth = params->BackBufferWidth = width;
    params_.BackBufferHeight = params->BackBufferHeight = height;
    if (params_.BackBufferFormat == D3DFMT_UNKNOWN)
        params_.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!params_.BackBufferCount)
        params_.BackBufferCount = 1;
    if (!params_.hDeviceWindow)
        params_.hDeviceWindow = old.hDeviceWindow;
    params_.PresentationInterval = old.PresentationInterval;
    DefaultState();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::Present(const RECT *src, const RECT *dst, D3D9Window window, const RGNDATA *dirty)
{
    if (src || dst || dirty || (window && window != window_))
        Unsupported("IDirect3DDevice9::Present with rects, a dirty region or another window (presented whole)");
    switch (lg::Present(legacy_))
    {
    case lg::PresentResult::Ok:
        ++counters_.presents;
        return D3D_OK;
    case lg::PresentResult::Skipped:
        ++counters_.presentsSkipped;
        return D3D_OK;
    case lg::PresentResult::Failed:
    default:
        return D3DERR_DRIVERINTERNALERROR;
    }
}

HRESULT STDMETHODCALLTYPE Device::GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type,
                                                IDirect3DSurface9 **surface)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    if (swapChain != 0 || index != 0 || type != D3DBACKBUFFER_TYPE_MONO)
        return D3DERR_INVALIDCALL;
    backBuffer_->AddRef();
    *surface = backBuffer_.get();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRasterStatus(UINT swapChain, D3DRASTER_STATUS *status)
{
    if (swapChain != 0 || !status)
        return D3DERR_INVALIDCALL;
    status->InVBlank = FALSE;
    status->ScanLine = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetDialogBoxMode(BOOL)
{
    return D3D_OK;
}

void STDMETHODCALLTYPE Device::SetGammaRamp(UINT swapChain, DWORD, const D3DGAMMARAMP *ramp)
{
    // Kept for GetGammaRamp. DXVK applies the ramp only to fullscreen
    // swapchains; the Vulkan front draws windowed and leaves it out.
    if (swapChain == 0 && ramp)
        gamma_ = *ramp;
}

void STDMETHODCALLTYPE Device::GetGammaRamp(UINT swapChain, D3DGAMMARAMP *ramp)
{
    if (swapChain == 0 && ramp)
        *ramp = gamma_;
}

// ---- Resource creation ------------------------------------------------------------------------------

namespace
{
HRESULT MakeTexture(Device &device, TextureState &state, lg::TextureKind kind, UINT width, UINT height, UINT depth,
                    UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
{
    if (!width || !height || !depth)
        return D3DERR_INVALIDCALL;
    if (usage & D3DUSAGE_AUTOGENMIPMAP)
        return device.Unsupported("D3DUSAGE_AUTOGENMIPMAP");
    lg::TextureDesc desc;
    desc.kind = kind;
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.levels = levels;
    desc.usage = usage & kAdapterUsage;
    desc.format = static_cast<lg::d3d::Format>(format);
    desc.pool = static_cast<lg::d3d::Pool>(pool);
    state.texture = lg::CreateTexture(device.legacy(), desc);
    if (!state.texture)
        return D3DERR_INVALIDCALL;
    const lg::TextureDesc &made = lg::Describe(state.texture);
    state.usage = usage;
    state.pool = pool;
    state.format = format;
    state.width = made.width;
    state.height = made.height;
    state.depth = made.depth;
    state.levels = made.levels;
    state.faces = kind == lg::TextureKind::Cube ? 6 : 1;
    state.surfaces.resize(static_cast<size_t>(state.faces) * state.levels);
    return D3D_OK;
}
} // namespace

HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                                D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared)
{
    if (!texture)
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    Texture *object = new Texture(this);
    const HRESULT hr = MakeTexture(*this, object->state_, lg::TextureKind::Tex2D, width, height, 1, levels, usage,
                                   format, pool);
    if (FAILED(hr))
    {
        object->Release();
        return hr;
    }
    *texture = object;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                                      D3DFORMAT format, D3DPOOL pool,
                                                      IDirect3DVolumeTexture9 **texture, HANDLE *shared)
{
    if (!texture)
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    VolumeTexture *object = new VolumeTexture(this);
    const HRESULT hr = MakeTexture(*this, object->state_, lg::TextureKind::Volume, width, height, depth, levels,
                                   usage, format, pool);
    if (FAILED(hr))
    {
        object->Release();
        return hr;
    }
    *texture = object;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
                                                    D3DPOOL pool, IDirect3DCubeTexture9 **texture, HANDLE *shared)
{
    if (!texture)
        return D3DERR_INVALIDCALL;
    *texture = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    CubeTexture *object = new CubeTexture(this);
    const HRESULT hr = MakeTexture(*this, object->state_, lg::TextureKind::Cube, edge, edge, 1, levels, usage, format,
                                   pool);
    if (FAILED(hr))
    {
        object->Release();
        return hr;
    }
    *texture = object;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                     IDirect3DVertexBuffer9 **buffer, HANDLE *shared)
{
    if (!buffer)
        return D3DERR_INVALIDCALL;
    *buffer = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    if (!length)
        return D3DERR_INVALIDCALL;
    lg::VertexBuffer *handle = lg::CreateVertexBuffer(legacy_, length, usage & kAdapterUsage,
                                                      static_cast<lg::d3d::Pool>(pool), nullptr);
    if (!handle)
        return D3DERR_OUTOFVIDEOMEMORY;
    *buffer = new VertexBuffer(this, handle, length, usage, fvf, pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DIndexBuffer9 **buffer, HANDLE *shared)
{
    if (!buffer)
        return D3DERR_INVALIDCALL;
    *buffer = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    if (!length || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32))
        return D3DERR_INVALIDCALL;
    lg::IndexBuffer *handle = lg::CreateIndexBuffer(legacy_, length, usage & kAdapterUsage,
                                                    static_cast<lg::d3d::Format>(format),
                                                    static_cast<lg::d3d::Pool>(pool), nullptr);
    if (!handle)
        return D3DERR_OUTOFVIDEOMEMORY;
    *buffer = new IndexBuffer(this, handle, length, usage, format, pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format,
                                                     D3DMULTISAMPLE_TYPE samples, DWORD, BOOL,
                                                     IDirect3DSurface9 **surface, HANDLE *shared)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    if (samples != D3DMULTISAMPLE_NONE)
        return D3DERR_NOTAVAILABLE;
    lg::Surface *handle = lg::CreateRenderTarget(legacy_, width, height, static_cast<lg::d3d::Format>(format), nullptr);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *surface = new Surface(this, handle, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format,
                                                            D3DMULTISAMPLE_TYPE samples, DWORD, BOOL,
                                                            IDirect3DSurface9 **surface, HANDLE *shared)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    if (samples != D3DMULTISAMPLE_NONE)
        return D3DERR_NOTAVAILABLE;
    lg::Surface *handle = lg::CreateDepthStencil(legacy_, width, height, static_cast<lg::d3d::Format>(format), nullptr);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *surface = new Surface(this, handle, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format,
                                                              D3DPOOL pool, IDirect3DSurface9 **surface,
                                                              HANDLE *shared)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    if (shared)
        return Unsupported("shared resource handles");
    lg::Surface *handle = lg::CreatePlainSurface(legacy_, width, height, static_cast<lg::d3d::Format>(format),
                                                 static_cast<lg::d3d::Pool>(pool), nullptr);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *surface = new Surface(this, handle, 0, pool);
    return D3D_OK;
}

// ---- Copies ----------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *,
                                                const D3D9Point *)
{
    return Unsupported("IDirect3DDevice9::UpdateSurface");
}

HRESULT STDMETHODCALLTYPE Device::UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    lg::Texture *from = TextureHandle(src);
    lg::Texture *to = TextureHandle(dst);
    if (!from || !to || src->GetType() != dst->GetType())
        return D3DERR_INVALIDCALL;
    lg::UpdateTexture(legacy_, from, to);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRenderTargetData(IDirect3DSurface9 *renderTarget, IDirect3DSurface9 *dst)
{
    if (!renderTarget || !dst)
        return D3DERR_INVALIDCALL;
    const bool ok = lg::GetRenderTargetData(legacy_, static_cast<Surface *>(renderTarget)->handle(),
                                            static_cast<Surface *>(dst)->handle());
    return ok ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Device::GetFrontBufferData(UINT swapChain, IDirect3DSurface9 *dst)
{
    // The last presented image is still in the back buffer (Present copies it
    // to the swapchain), so the copy comes from there, at back buffer size.
    if (swapChain != 0 || !dst)
        return D3DERR_INVALIDCALL;
    const bool ok = lg::GetRenderTargetData(legacy_, backBuffer_->handle(), static_cast<Surface *>(dst)->handle());
    return ok ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Device::StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst,
                                              const RECT *dstRect, D3DTEXTUREFILTERTYPE filter)
{
    if (!src || !dst)
        return D3DERR_INVALIDCALL;
    lg::Rect from{}, to{};
    if (srcRect)
        from = ToRect(*srcRect);
    if (dstRect)
        to = ToRect(*dstRect);
    lg::StretchRect(legacy_, static_cast<Surface *>(src)->handle(), srcRect ? &from : nullptr,
                    static_cast<Surface *>(dst)->handle(), dstRect ? &to : nullptr, filter == D3DTEXF_LINEAR);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::ColorFill(IDirect3DSurface9 *, const RECT *, D3DCOLOR)
{
    return Unsupported("IDirect3DDevice9::ColorFill");
}

// ---- Targets, viewport, clears ---------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(DWORD index, IDirect3DSurface9 *surface)
{
    if (index >= lg::kMaxRenderTargets || (index == 0 && !surface))
        return D3DERR_INVALIDCALL;
    Surface *target = static_cast<Surface *>(surface);
    if (target)
    {
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        if (!(desc.Usage & D3DUSAGE_RENDERTARGET))
            return D3DERR_INVALIDCALL;
    }
    lg::SetRenderTarget(legacy_, index, target ? target->handle() : nullptr);
    Rebind(renderTargets_[index], target);
    if (index == 0)
    {
        uint32_t width = 0, height = 0;
        lg::d3d::Format format = lg::d3d::FMT_UNKNOWN;
        lg::SurfaceSize(target->handle(), width, height, format);
        viewport_ = D3DVIEWPORT9{0, 0, width, height, 0.0f, 1.0f};
        scissor_ = RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRenderTarget(DWORD index, IDirect3DSurface9 **surface)
{
    if (!surface || index >= lg::kMaxRenderTargets)
        return D3DERR_INVALIDCALL;
    *surface = renderTargets_[index];
    if (!*surface)
        return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetDepthStencilSurface(IDirect3DSurface9 *surface)
{
    Surface *depth = static_cast<Surface *>(surface);
    if (depth)
    {
        D3DSURFACE_DESC desc{};
        depth->GetDesc(&desc);
        if (!(desc.Usage & D3DUSAGE_DEPTHSTENCIL))
            return D3DERR_INVALIDCALL;
    }
    lg::SetDepthStencilSurface(legacy_, depth ? depth->handle() : nullptr);
    Rebind(depthStencil_, depth);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetDepthStencilSurface(IDirect3DSurface9 **surface)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = depthStencil_;
    if (!*surface)
        return D3DERR_NOTFOUND;
    (*surface)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                                        DWORD stencil)
{
    if (count && !rects)
        return D3DERR_INVALIDCALL;
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !depthStencil_)
        return D3DERR_INVALIDCALL;
    std::vector<lg::Rect> converted(count);
    for (DWORD i = 0; i < count; ++i)
        converted[i] = lg::Rect{static_cast<int32_t>(rects[i].x1), static_cast<int32_t>(rects[i].y1),
                                static_cast<int32_t>(rects[i].x2), static_cast<int32_t>(rects[i].y2)};
    lg::Clear(legacy_, count, count ? converted.data() : nullptr, flags, color, z, stencil);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT9 *viewport)
{
    if (!viewport)
        return D3DERR_INVALIDCALL;
    viewport_ = *viewport;
    lg::SetViewport(legacy_, lg::Viewport{viewport->X, viewport->Y, viewport->Width, viewport->Height,
                                          viewport->MinZ, viewport->MaxZ});
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT9 *viewport)
{
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = viewport_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetScissorRect(const RECT *rect)
{
    if (!rect)
        return D3DERR_INVALIDCALL;
    scissor_ = *rect;
    lg::SetScissorRect(legacy_, ToRect(*rect));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetScissorRect(RECT *rect)
{
    if (!rect)
        return D3DERR_INVALIDCALL;
    *rect = scissor_;
    return D3D_OK;
}

// ---- Fixed function (not supported: the renderer draws everything with shaders) -------------------

HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *)
{
    return Unsupported("IDirect3DDevice9::SetTransform");
}

HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX *)
{
    return Unsupported("IDirect3DDevice9::GetTransform");
}

HRESULT STDMETHODCALLTYPE Device::MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *)
{
    return Unsupported("IDirect3DDevice9::MultiplyTransform");
}

HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL9 *)
{
    return Unsupported("IDirect3DDevice9::SetMaterial");
}

HRESULT STDMETHODCALLTYPE Device::GetMaterial(D3DMATERIAL9 *)
{
    return Unsupported("IDirect3DDevice9::GetMaterial");
}

HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD, const D3DLIGHT9 *)
{
    return Unsupported("IDirect3DDevice9::SetLight");
}

HRESULT STDMETHODCALLTYPE Device::GetLight(DWORD, D3DLIGHT9 *)
{
    return Unsupported("IDirect3DDevice9::GetLight");
}

HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD, BOOL)
{
    return Unsupported("IDirect3DDevice9::LightEnable");
}

HRESULT STDMETHODCALLTYPE Device::GetLightEnable(DWORD, BOOL *)
{
    return Unsupported("IDirect3DDevice9::GetLightEnable");
}

HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD, const float *)
{
    return Unsupported("IDirect3DDevice9::SetClipPlane");
}

HRESULT STDMETHODCALLTYPE Device::GetClipPlane(DWORD, float *)
{
    return Unsupported("IDirect3DDevice9::GetClipPlane");
}

HRESULT STDMETHODCALLTYPE Device::SetClipStatus(const D3DCLIPSTATUS9 *)
{
    return Unsupported("IDirect3DDevice9::SetClipStatus");
}

HRESULT STDMETHODCALLTYPE Device::GetClipStatus(D3DCLIPSTATUS9 *)
{
    return Unsupported("IDirect3DDevice9::GetClipStatus");
}

HRESULT STDMETHODCALLTYPE Device::GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD *)
{
    return Unsupported("IDirect3DDevice9::GetTextureStageState");
}

HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)
{
    return Unsupported("IDirect3DDevice9::SetTextureStageState");
}

HRESULT STDMETHODCALLTYPE Device::SetPaletteEntries(UINT, const PALETTEENTRY *)
{
    return Unsupported("IDirect3DDevice9::SetPaletteEntries");
}

HRESULT STDMETHODCALLTYPE Device::GetPaletteEntries(UINT, PALETTEENTRY *)
{
    return Unsupported("IDirect3DDevice9::GetPaletteEntries");
}

HRESULT STDMETHODCALLTYPE Device::SetCurrentTexturePalette(UINT)
{
    return Unsupported("IDirect3DDevice9::SetCurrentTexturePalette");
}

HRESULT STDMETHODCALLTYPE Device::GetCurrentTexturePalette(UINT *)
{
    return Unsupported("IDirect3DDevice9::GetCurrentTexturePalette");
}

HRESULT STDMETHODCALLTYPE Device::SetFVF(DWORD fvf)
{
    return fvf ? Unsupported("IDirect3DDevice9::SetFVF") : D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetFVF(DWORD *fvf)
{
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9 *,
                                                  IDirect3DVertexDeclaration9 *, DWORD)
{
    return Unsupported("IDirect3DDevice9::ProcessVertices");
}

HRESULT STDMETHODCALLTYPE Device::SetSoftwareVertexProcessing(BOOL software)
{
    return software ? Unsupported("IDirect3DDevice9::SetSoftwareVertexProcessing") : D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetNPatchMode(float segments)
{
    return segments > 0.0f ? Unsupported("IDirect3DDevice9::SetNPatchMode") : D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawRectPatch(UINT, const float *, const D3DRECTPATCH_INFO *)
{
    return Unsupported("IDirect3DDevice9::DrawRectPatch");
}

HRESULT STDMETHODCALLTYPE Device::DrawTriPatch(UINT, const float *, const D3DTRIPATCH_INFO *)
{
    return Unsupported("IDirect3DDevice9::DrawTriPatch");
}

HRESULT STDMETHODCALLTYPE Device::DeletePatch(UINT)
{
    return Unsupported("IDirect3DDevice9::DeletePatch");
}

HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **block)
{
    if (block)
        *block = nullptr;
    return Unsupported("IDirect3DDevice9::CreateStateBlock");
}

HRESULT STDMETHODCALLTYPE Device::BeginStateBlock()
{
    return Unsupported("IDirect3DDevice9::BeginStateBlock");
}

HRESULT STDMETHODCALLTYPE Device::EndStateBlock(IDirect3DStateBlock9 **block)
{
    if (block)
        *block = nullptr;
    return Unsupported("IDirect3DDevice9::EndStateBlock");
}

HRESULT STDMETHODCALLTYPE Device::ValidateDevice(DWORD *passes)
{
    if (passes)
        *passes = 1;
    return D3D_OK;
}

// ---- States ----------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    if (static_cast<uint32_t>(state) >= lg::d3d::RS_COUNT)
        return D3DERR_INVALIDCALL;
    lg::SetRenderState(legacy_, static_cast<lg::d3d::RenderState>(state), value);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD *value)
{
    if (!value || static_cast<uint32_t>(state) >= lg::d3d::RS_COUNT)
        return D3DERR_INVALIDCALL;
    *value = lg::GetRenderState(legacy_, static_cast<lg::d3d::RenderState>(state));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value)
{
    const int slot = SamplerSlot(sampler);
    if (!value || slot < 0 || static_cast<uint32_t>(type) >= lg::d3d::SAMP_COUNT)
        return D3DERR_INVALIDCALL;
    *value = samplerStates_[slot][type];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    const int slot = SamplerSlot(sampler);
    if (slot < 0 || static_cast<uint32_t>(type) >= lg::d3d::SAMP_COUNT)
        return D3DERR_INVALIDCALL;
    samplerStates_[slot][type] = value;
    lg::SetSamplerState(legacy_, sampler, static_cast<lg::d3d::SamplerState>(type), value);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture)
{
    const int slot = SamplerSlot(stage);
    if (!texture || slot < 0)
        return D3DERR_INVALIDCALL;
    *texture = textures_[slot];
    if (*texture)
        (*texture)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture)
{
    const int slot = SamplerSlot(stage);
    if (slot < 0)
        return D3DERR_INVALIDCALL;
    lg::SetTexture(legacy_, stage, TextureHandle(texture));
    textures_[slot] = texture;
    Rebind(textureObjects_[slot], TextureObject(texture));
    return D3D_OK;
}

// ---- Shaders, declarations, streams ----------------------------------------------------------------

template <class Interface>
HRESULT Device::CreateShaderObject(const DWORD *code, Interface **out, bool pixel)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (!code || (code[0] >> 16) != (pixel ? 0xffffu : 0xfffeu))
        return D3DERR_INVALIDCALL;
    const size_t bytes = ShaderBytes(code);
    if (!bytes)
        return D3DERR_INVALIDCALL;
    std::string error;
    lg::Shader *shader = lg::CreateShader(legacy_, code, bytes, error);
    if (!shader)
    {
        HostLog(host(), "vulkan d3d9: Create%sShader: %s", pixel ? "Pixel" : "Vertex", error.c_str());
        return D3DERR_INVALIDCALL;
    }
    *out = new Shader<Interface>(this, shader, code, bytes);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD *code, IDirect3DVertexShader9 **shader)
{
    return CreateShaderObject(code, shader, false);
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD *code, IDirect3DPixelShader9 **shader)
{
    return CreateShaderObject(code, shader, true);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShader(IDirect3DVertexShader9 *shader)
{
    VertexShader *object = static_cast<VertexShader *>(shader);
    lg::SetVertexShader(legacy_, object ? object->handle() : nullptr);
    Rebind(vertexShader_, object);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShader(IDirect3DVertexShader9 **shader)
{
    if (!shader)
        return D3DERR_INVALIDCALL;
    *shader = vertexShader_;
    if (*shader)
        (*shader)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShader(IDirect3DPixelShader9 *shader)
{
    PixelShader *object = static_cast<PixelShader *>(shader);
    lg::SetPixelShader(legacy_, object ? object->handle() : nullptr);
    Rebind(pixelShader_, object);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShader(IDirect3DPixelShader9 **shader)
{
    if (!shader)
        return D3DERR_INVALIDCALL;
    *shader = pixelShader_;
    if (*shader)
        (*shader)->AddRef();
    return D3D_OK;
}

namespace
{
template <class T>
HRESULT SetConstants(std::vector<T> &store, UINT width, UINT start, const T *data, UINT count)
{
    if (!data && count)
        return D3DERR_INVALIDCALL;
    if (static_cast<size_t>(start + count) * width > store.size())
        return D3DERR_INVALIDCALL;
    std::copy(data, data + static_cast<size_t>(count) * width, store.begin() + static_cast<size_t>(start) * width);
    return D3D_OK;
}

template <class T>
HRESULT GetConstants(const std::vector<T> &store, UINT width, UINT start, T *data, UINT count)
{
    if (!data && count)
        return D3DERR_INVALIDCALL;
    if (static_cast<size_t>(start + count) * width > store.size())
        return D3DERR_INVALIDCALL;
    std::copy(store.begin() + static_cast<size_t>(start) * width,
              store.begin() + static_cast<size_t>(start + count) * width, data);
    return D3D_OK;
}
} // namespace

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantF(UINT start, const float *data, UINT count)
{
    const HRESULT hr = SetConstants(vsConstantsF_, 4, start, data, count);
    if (SUCCEEDED(hr))
        lg::SetVertexShaderConstantF(legacy_, start, data, count);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantF(UINT start, float *data, UINT count)
{
    return GetConstants(vsConstantsF_, 4, start, data, count);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantF(UINT start, const float *data, UINT count)
{
    const HRESULT hr = SetConstants(psConstantsF_, 4, start, data, count);
    if (SUCCEEDED(hr))
        lg::SetPixelShaderConstantF(legacy_, start, data, count);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantF(UINT start, float *data, UINT count)
{
    return GetConstants(psConstantsF_, 4, start, data, count);
}

// Integer and boolean constants are kept for the getters; the Legacy shaders
// read them as zero (see docs/vulkan-legacy-adapter.md), so setting a non-zero
// value counts as unsupported.
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantI(UINT start, const int *data, UINT count)
{
    const HRESULT hr = SetConstants(vsConstantsI_, 4, start, data, count);
    if (SUCCEEDED(hr) && std::any_of(data, data + static_cast<size_t>(count) * 4, [](int v) { return v != 0; }))
        Unsupported("IDirect3DDevice9::SetVertexShaderConstantI (shaders read zero)");
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantI(UINT start, int *data, UINT count)
{
    return GetConstants(vsConstantsI_, 4, start, data, count);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantB(UINT start, const BOOL *data, UINT count)
{
    const HRESULT hr = SetConstants(vsConstantsB_, 1, start, data, count);
    if (SUCCEEDED(hr) && std::any_of(data, data + count, [](BOOL v) { return v != FALSE; }))
        Unsupported("IDirect3DDevice9::SetVertexShaderConstantB (shaders read zero)");
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantB(UINT start, BOOL *data, UINT count)
{
    return GetConstants(vsConstantsB_, 1, start, data, count);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantI(UINT start, const int *data, UINT count)
{
    const HRESULT hr = SetConstants(psConstantsI_, 4, start, data, count);
    if (SUCCEEDED(hr) && std::any_of(data, data + static_cast<size_t>(count) * 4, [](int v) { return v != 0; }))
        Unsupported("IDirect3DDevice9::SetPixelShaderConstantI (shaders read zero)");
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantI(UINT start, int *data, UINT count)
{
    return GetConstants(psConstantsI_, 4, start, data, count);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantB(UINT start, const BOOL *data, UINT count)
{
    const HRESULT hr = SetConstants(psConstantsB_, 1, start, data, count);
    if (SUCCEEDED(hr) && std::any_of(data, data + count, [](BOOL v) { return v != FALSE; }))
        Unsupported("IDirect3DDevice9::SetPixelShaderConstantB (shaders read zero)");
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantB(UINT start, BOOL *data, UINT count)
{
    return GetConstants(psConstantsB_, 1, start, data, count);
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                                          IDirect3DVertexDeclaration9 **decl)
{
    if (!decl)
        return D3DERR_INVALIDCALL;
    *decl = nullptr;
    if (!elements)
        return D3DERR_INVALIDCALL;
    constexpr UINT kMaxElements = 64;
    UINT count = 0;
    while (count < kMaxElements && elements[count].Stream != 0xff)
        ++count;
    if (count == kMaxElements)
        return D3DERR_INVALIDCALL;
    std::vector<lg::VertexElement> converted(count);
    for (UINT i = 0; i < count; ++i)
    {
        converted[i].stream = elements[i].Stream;
        converted[i].offset = elements[i].Offset;
        converted[i].type = elements[i].Type;
        converted[i].method = elements[i].Method;
        converted[i].usage = elements[i].Usage;
        converted[i].usageIndex = elements[i].UsageIndex;
    }
    lg::VertexDecl *handle = lg::CreateVertexDecl(legacy_, converted.data(), count);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *decl = new VertexDeclaration(this, handle, elements, count);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl)
{
    VertexDeclaration *object = static_cast<VertexDeclaration *>(decl);
    lg::SetVertexDecl(legacy_, object ? object->handle() : nullptr);
    Rebind(declaration_, object);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **decl)
{
    if (!decl)
        return D3DERR_INVALIDCALL;
    *decl = declaration_;
    if (*decl)
        (*decl)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset,
                                                  UINT stride)
{
    if (stream >= lg::kMaxStreams)
        return D3DERR_INVALIDCALL;
    VertexBuffer *object = static_cast<VertexBuffer *>(buffer);
    lg::SetStreamSource(legacy_, stream, object ? object->handle() : nullptr, offset, stride);
    Rebind(streams_[stream].buffer, object);
    streams_[stream].offset = offset;
    streams_[stream].stride = stride;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset,
                                                  UINT *stride)
{
    if (stream >= lg::kMaxStreams || !buffer || !offset || !stride)
        return D3DERR_INVALIDCALL;
    *buffer = streams_[stream].buffer;
    if (*buffer)
        (*buffer)->AddRef();
    *offset = streams_[stream].offset;
    *stride = streams_[stream].stride;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetStreamSourceFreq(UINT stream, UINT divider)
{
    if (stream >= lg::kMaxStreams)
        return D3DERR_INVALIDCALL;
    if (divider != 1)
        return Unsupported("IDirect3DDevice9::SetStreamSourceFreq (instancing)");
    streams_[stream].divider = divider;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSourceFreq(UINT stream, UINT *divider)
{
    if (stream >= lg::kMaxStreams || !divider)
        return D3DERR_INVALIDCALL;
    *divider = streams_[stream].divider;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer9 *buffer)
{
    IndexBuffer *object = static_cast<IndexBuffer *>(buffer);
    lg::SetIndices(legacy_, object ? object->handle() : nullptr);
    Rebind(indices_, object);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetIndices(IDirect3DIndexBuffer9 **buffer)
{
    if (!buffer)
        return D3DERR_INVALIDCALL;
    *buffer = indices_;
    if (*buffer)
        (*buffer)->AddRef();
    return D3D_OK;
}

// ---- Draws -----------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT count)
{
    lg::DrawPrimitive(legacy_, static_cast<lg::d3d::PrimitiveType>(type), startVertex, count);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                                                       UINT vertexCount, UINT startIndex, UINT count)
{
    lg::DrawIndexedPrimitive(legacy_, static_cast<lg::d3d::PrimitiveType>(type), baseVertex, minIndex, vertexCount,
                             startIndex, count);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride)
{
    if (!data || !stride)
        return D3DERR_INVALIDCALL;
    lg::DrawPrimitiveUP(legacy_, static_cast<lg::d3d::PrimitiveType>(type), count, data, stride);
    // D3D9 leaves stream 0 unbound after a draw from memory.
    Rebind(streams_[0].buffer, static_cast<VertexBuffer *>(nullptr));
    streams_[0].offset = 0;
    streams_[0].stride = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT,
                                                         const void *, UINT)
{
    return Unsupported("IDirect3DDevice9::DrawIndexedPrimitiveUP");
}

// ---- Queries ---------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query)
{
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION)
    {
        if (query)
            *query = nullptr;
        return D3DERR_NOTAVAILABLE;
    }
    if (!query) // a support check
        return D3D_OK;
    *query = nullptr;
    lg::Query *handle =
        lg::CreateQuery(legacy_, type == D3DQUERYTYPE_EVENT ? lg::QueryKind::Event : lg::QueryKind::Occlusion);
    if (!handle)
        return D3DERR_NOTAVAILABLE;
    *query = new Query(this, handle, type);
    return D3D_OK;
}
} // namespace gfx_vk::d3d9
