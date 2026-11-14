#pragma once

// The D3D9 front's classes (vk_d3d9.h). One object per D3D9 interface, each
// holding the adapter object it stands for.
//
// Reference counts follow D3D9 (see Counted): an object lives while the
// program holds a reference or the device has it bound. Surfaces of a texture
// level and volumes of a volume texture level share their texture's counts,
// and the back buffer and the implicit swap chain share the device's. Every
// object keeps the device alive until it goes: the device is destroyed once
// its own count and its live objects are both zero.
//
// Threads: the renderer creates its device with D3DCREATE_MULTITHREADED and
// calls it from more than one thread (the render thread draws while the
// database thread creates and fills textures), so every method holds
// FrontLock, as DXVK holds its device lock.

#include "vk_d3d9.h"
#include "vk_d3d9_backend.h"

#include <gfx_vk/legacy/vk_legacy.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gfx_vk::d3d9
{
namespace lg = gfx_vk::legacy;

class Direct3D;
class Device;

void HostLog(const Host &host, const char *format, ...);

// Multisampling: the sample counts the front offers (2, 4 and 8; the adapter
// makes a count the device lacks with 4, see legacy::UsableSamples), and none
// on the PS Vita. Samples of a D3D9 multisample type: 1 for none.
bool OffersSamples(D3DMULTISAMPLE_TYPE type);
inline uint32_t SamplesOf(D3DMULTISAMPLE_TYPE type) { return type >= D3DMULTISAMPLE_2_SAMPLES ? uint32_t(type) : 1u; }

// The lock every method of every front object holds for its whole call. One
// for the process and recursive: a method calls others (Release from a
// setter, a device method from a resource's), and it must outlive the
// objects, because a Release can destroy the device whose method is running.
// The renderer has one device, so a lock per device would gain nothing.
//
// A thread that finds the lock taken first tries again for a short while
// without sleeping, as DXVK's device lock does: nearly every call holds it
// for a microsecond or less, and a sleeping waiter costs the holder a system
// call to wake it at the end of its own call. The engine's main thread polls
// a fence thousands of times a second while the render thread draws
// (R_SyncGpu), so with a plain mutex the render thread paid that on a good
// part of its calls. A waiter that is still out of luck after 2000 tries
// (tens of microseconds: the holder is waiting for the GPU, building a
// pipeline or not running) sleeps after all.
// KISAK_VK_FRONT_SPIN=0 makes every waiter sleep at once, to compare.
//
// The lock itself is one word (free, held, or held with sleepers), so taking
// and dropping it without a waiter is one atomic operation each, inline in
// the call; the sleep goes through a mutex and condition variable only a
// waiter touches. The object needs no constructor that runs, so a call from a
// static constructor or destructor still finds it.
//
// The PS Vita build keeps the plain recursive mutex: thread_local is emulated
// there (a library call on every use), and its calls come from one thread.
#if defined(__vita__)
using FrontMutexType = std::recursive_mutex;
FrontMutexType &FrontMutex();
#else
class FrontMutexType
{
public:
    constexpr FrontMutexType() = default;
    void lock()
    {
        const void *const self = Self();
        if (owner_.load(std::memory_order_relaxed) == self)
        {
            ++depth_;
            return;
        }
        uint32_t free = kFree;
        if (!state_.compare_exchange_strong(free, kHeld, std::memory_order_acquire, std::memory_order_relaxed))
            LockSlow();
        owner_.store(self, std::memory_order_relaxed);
        depth_ = 1;
    }
    void unlock()
    {
        if (--depth_ != 0)
            return;
        owner_.store(nullptr, std::memory_order_relaxed);
        if (state_.exchange(kFree, std::memory_order_release) == kSleepers)
            WakeOne();
    }

private:
    static constexpr uint32_t kFree = 0, kHeld = 1, kSleepers = 2;
    // Any address that is this thread's alone.
    static const void *Self()
    {
        static thread_local const char t_token = 0;
        return &t_token;
    }
    void LockSlow();
    void WakeOne();
    std::atomic<uint32_t> state_{kFree};
    // The thread inside (its Self()), null when free. Only that thread stores
    // its token, so a thread that reads its own is in.
    std::atomic<const void *> owner_{nullptr};
    uint32_t depth_ = 0; // the owner's
};
inline constinit FrontMutexType g_frontMutex;
inline FrontMutexType &FrontMutex() { return g_frontMutex; }
#endif
// Waits for the front lock that ended in a sleep (spinning did not get it),
// over all threads since start: the slow frame lines print a frame's share.
struct FrontLockSleeps
{
    uint64_t sleeps = 0; // waits on the condition variable
    uint64_t micros = 0; // from the end of spinning to holding the lock
};
FrontLockSleeps FrontLockSlept();
class FrontLock
{
public:
    FrontLock() { FrontMutex().lock(); }
    ~FrontLock() { FrontMutex().unlock(); }
    FrontLock(const FrontLock &) = delete;
    FrontLock &operator=(const FrontLock &) = delete;
};
// The devices Counters can answer for.
void RegisterDevice(Device *device, bool live);

// SetPrivateData's store, for resources and volumes.
class PrivateData
{
public:
    HRESULT Set(REFGUID guid, const void *data, DWORD size, DWORD flags);
    HRESULT Get(REFGUID guid, void *data, DWORD *size) const;
    HRESULT Free(REFGUID guid);

private:
    struct Entry
    {
        GUID guid;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries_;
};

// The reference counts behind every front object (D3D9's two kinds).
//
// The public count is the program's (AddRef and Release). The bind count is the
// device's own hold on an object bound to it (a texture, buffer, shader,
// declaration or surface set on the device), so a released object the device
// still draws with stays alive until it is unbound, as in D3D9. An object goes
// once both are zero.
//
// Container: the object whose counts this one uses instead of its own (the
// texture of a level surface or volume). Device children (the back buffer and
// the implicit swap chain) have a public count of their own that starts at 0;
// while it is above 0 they hold one reference on the device, and they are
// destroyed with the device, as in D3D9.
class Counted
{
public:
    Counted(Device *device, Counted *container, bool deviceChild);
    virtual ~Counted();

    ULONG AddRefPublic();
    ULONG ReleasePublic();
    void Bind();
    void Unbind();

    // The object as IUnknown (for GetContainer).
    virtual IUnknown *Unknown() = 0;
    Device *device() const { return device_; }

protected:
    Device *device_;
    Counted *container_;
    bool deviceChild_;
    ULONG refs_ = 1;
    ULONG binds_ = 0;

private:
    void DestroyIfUnused();
};

template <class Interface>
class Object : public Interface, public Counted
{
public:
    explicit Object(Device *device, Counted *container = nullptr, bool deviceChild = false)
        : Counted(device, container, deviceChild)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefPublic(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleasePublic(); }

    IUnknown *Unknown() override { return static_cast<Interface *>(this); }
};

// IDirect3DResource9's methods for surfaces, buffers and textures.
template <class Interface>
class Resource : public Object<Interface>
{
public:
    Resource(Device *device, D3DRESOURCETYPE type, Counted *container = nullptr, bool deviceChild = false)
        : Object<Interface>(device, container, deviceChild), type_(type)
    {
    }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
    DWORD STDMETHODCALLTYPE GetPriority() override;
    void STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return type_; }

protected:
    D3DRESOURCETYPE type_;
    DWORD priority_ = 0;
    PrivateData private_;
};

class Surface final : public Resource<IDirect3DSurface9>
{
public:
    // A standalone surface (render target, depth-stencil, plain), which the
    // front destroys with the object.
    Surface(Device *device, lg::Surface *surface, DWORD usage, D3DPOOL pool);
    // A level (and cube face) of a texture, which owns the adapter surface.
    Surface(Device *device, lg::Surface *surface, Counted *texture, DWORD usage, D3DPOOL pool);
    // The back buffer, a child of the device.
    Surface(Device *device, lg::Surface *surface);
    ~Surface() override;

    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *desc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC *dc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;

    lg::Surface *handle() const { return surface_; }

private:
    lg::Surface *surface_;
    bool owned_;
    DWORD usage_;
    D3DPOOL pool_;
};

// The adapter texture behind the three texture interfaces, with the level
// surfaces (or volumes) handed out so far.
struct TextureState
{
    lg::Texture *texture = nullptr;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_MANAGED;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    uint32_t width = 0, height = 0, depth = 0, levels = 0, faces = 1;
    DWORD lod = 0;
    std::vector<std::unique_ptr<Surface>> surfaces; // faces * levels, created on demand
};

template <class Interface>
class BaseTexture : public Resource<Interface>
{
public:
    BaseTexture(Device *device, D3DRESOURCETYPE type) : Resource<Interface>(device, type) {}
    ~BaseTexture() override;

    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override;
    DWORD STDMETHODCALLTYPE GetLOD() override { return state_.lod; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return state_.levels; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override {}

    TextureState state_;

protected:
    HRESULT LevelDesc(UINT level, D3DSURFACE_DESC *desc) const;
    HRESULT LevelSurface(UINT face, UINT level, IDirect3DSurface9 **surface);
    HRESULT Lock(UINT face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags);
    HRESULT Unlock(UINT face, UINT level);
    // AddDirtyRect: rect null marks the whole face.
    HRESULT Dirty(UINT face, const RECT *rect);
};

class Texture final : public BaseTexture<IDirect3DTexture9>
{
public:
    explicit Texture(Device *device) : BaseTexture(device, D3DRTYPE_TEXTURE) {}

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *rect) override;
};

class CubeTexture final : public BaseTexture<IDirect3DCubeTexture9>
{
public:
    explicit CubeTexture(Device *device) : BaseTexture(device, D3DRTYPE_CUBETEXTURE) {}

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect,
                                       DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT *rect) override;
};

class VolumeTexture;

class Volume final : public Object<IDirect3DVolume9>
{
public:
    Volume(Device *device, VolumeTexture *texture, UINT level);

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *desc) override;
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox() override;

private:
    VolumeTexture *texture_;
    UINT level_;
    PrivateData private_;
};

class VolumeTexture final : public BaseTexture<IDirect3DVolumeTexture9>
{
public:
    explicit VolumeTexture(Device *device) : BaseTexture(device, D3DRTYPE_VOLUMETEXTURE) {}

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume9 **volume) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *box) override;

    std::vector<std::unique_ptr<Volume>> volumes_;
};

// The adapter texture, and the front object, behind any of the three
// interfaces (null for null).
lg::Texture *TextureHandle(IDirect3DBaseTexture9 *texture);
Counted *TextureObject(IDirect3DBaseTexture9 *texture);

class VertexBuffer final : public Resource<IDirect3DVertexBuffer9>
{
public:
    VertexBuffer(Device *device, lg::VertexBuffer *buffer, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool);
    ~VertexBuffer() override;

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *desc) override;

    lg::VertexBuffer *handle() const { return buffer_; }

private:
    lg::VertexBuffer *buffer_;
    UINT size_;
    DWORD usage_, fvf_;
    D3DPOOL pool_;
};

class IndexBuffer final : public Resource<IDirect3DIndexBuffer9>
{
public:
    IndexBuffer(Device *device, lg::IndexBuffer *buffer, UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~IndexBuffer() override;

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *desc) override;

    lg::IndexBuffer *handle() const { return buffer_; }

private:
    lg::IndexBuffer *buffer_;
    UINT size_;
    DWORD usage_;
    D3DFORMAT format_;
    D3DPOOL pool_;
};

template <class Interface>
class Shader final : public Object<Interface>
{
public:
    Shader(Device *device, lg::Shader *shader, const DWORD *code, size_t bytes);
    ~Shader() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

    lg::Shader *handle() const { return shader_; }

private:
    lg::Shader *shader_;
    std::vector<uint8_t> code_;
};
using VertexShader = Shader<IDirect3DVertexShader9>;
using PixelShader = Shader<IDirect3DPixelShader9>;

class VertexDeclaration final : public Object<IDirect3DVertexDeclaration9>
{
public:
    VertexDeclaration(Device *device, lg::VertexDecl *decl, const D3DVERTEXELEMENT9 *elements, UINT count);
    ~VertexDeclaration() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) override;

    lg::VertexDecl *handle() const { return decl_; }

private:
    lg::VertexDecl *decl_;
    std::vector<D3DVERTEXELEMENT9> elements_; // with the end marker
};

class Query final : public Object<IDirect3DQuery9>
{
public:
    Query(Device *device, lg::Query *query, D3DQUERYTYPE type);
    ~Query() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return type_; }
    DWORD STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override;

private:
    lg::Query *query_;
    D3DQUERYTYPE type_;
};

// The implicit swap chain (index 0), a device child (see Counted).
class SwapChain final : public Object<IDirect3DSwapChain9>
{
public:
    explicit SwapChain(Device *device);

    HRESULT STDMETHODCALLTYPE Present(const RECT *src, const RECT *dst, D3D9Window window, const RGNDATA *dirty,
                                      DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *dst) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *status) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *mode) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *params) override;
};

class Direct3D final : public IDirect3D9
{
public:
    explicit Direct3D(const Host &host) : host_(host) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        const FrontLock lock;
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *init) override;
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE *mode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat,
                                              D3DFORMAT backBufferFormat, BOOL windowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                D3DRESOURCETYPE resourceType, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format,
                                                         BOOL windowed, D3DMULTISAMPLE_TYPE samples,
                                                         DWORD *qualityLevels) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                     D3DFORMAT renderTargetFormat, D3DFORMAT depthFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src,
                                                          D3DFORMAT dst) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, D3D9Window focusWindow, DWORD behavior,
                                           D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device) override;

    const Host &host() const { return host_; }

private:
    bool Mode(UINT index, D3DDISPLAYMODE &mode) const;

    Host host_;
    ULONG refs_ = 1;
};

// D3D9's capabilities as the front reports them (DXVK's, where it has them).
void FillCaps(D3DCAPS9 &caps);

class Device final : public IDirect3DDevice9
{
public:
    Device(Direct3D *parent, const D3DPRESENT_PARAMETERS &params, D3D9Window window, DWORD behavior);
    ~Device();
    // Opens the back end (the GPU and the adapter device); false with the
    // reason logged.
    bool Init();

    // The device's count, and the objects that keep it alive.
    void ObjectCreated() { ++objects_; }
    void ObjectDestroyed();
    lg::Device *legacy() const { return legacy_; }
    // Without FrontLock, from any thread (BackendPollProgress).
    void PollProgress() { BackendPollProgress(backend_); }
    D3D9Window window() const { return window_; }
    // The window is going: the core lets go of its swapchain and surface.
    void DetachWindow();
    // Under FrontLock (LogDeviceStats).
    void LogStats(bool reset);
    // Under FrontLock (vk_d3d9.h ModernSceneBegin and ModernSceneDone): the
    // scene is render target 0 and the depth surface as they are set now, or
    // the linear-light image standing in for the back buffer.
    ModernResult ModernSceneBegin();
    ModernResult ModernSceneDone(const ModernSceneView &view, bool *glowDrawn);
    ModernResult ModernSceneLit(const ModernSceneView &view, bool *glowDrawn);
    bool ModernWantsSceneDepth() { return BackendModernWantsSceneDepth(backend_); }
    // The depth surface as it is set now.
    ModernResult ModernSceneDepth(const ModernSceneView &view);
    void ModernSurfaceMoved(const float clip[4][4], const float prevClip[4][4])
    {
        BackendModernSurfaceMoved(backend_, legacy_, clip, prevClip);
    }
    void ModernSkinnedSurfaceMoved(uint64_t key, const float clip[4][4], const float prevClip[4][4])
    {
        BackendModernSkinnedSurfaceMoved(backend_, legacy_, key, clip, prevClip);
    }
    void SetModernLitInputs(const ModernLitInputs *inputs);
    bool ModernReflectionProbe(lg::Texture *probe) { return BackendModernReflectionProbe(backend_, legacy_, probe); }
    const Host &host() const { return parent_->host(); }
    HRESULT Unsupported(const char *method);
    DeviceCounters counters() const;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        const FrontLock lock;
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **d3d) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT swapChain, D3DDISPLAYMODE *mode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *params) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9 *bitmap) override;
    void STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override {}
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *params,
                                                        IDirect3DSwapChain9 **swapChain) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index, IDirect3DSwapChain9 **swapChain) override;
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *params) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT *src, const RECT *dst, D3D9Window window, const RGNDATA *dirty) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type,
                                            IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS *status) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enable) override;
    void STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP *ramp) override;
    void STDMETHODCALLTYPE GetGammaRamp(UINT swapChain, D3DGAMMARAMP *ramp) override;
    // Hands gamma_ to the adapter for a fullscreen device, nothing for a windowed one.
    void ApplyGamma();
    // Asks the host for params_'s display mode (Host::fullscreen) when the
    // device is fullscreen, and for the desktop's back when it no longer is.
    void ApplyDisplayMode();
    void LeaveFullscreen();
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                            D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                                  D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **texture,
                                                  HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DCubeTexture9 **texture, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer9 **buffer, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DIndexBuffer9 **buffer, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width, UINT height, D3DFORMAT format,
                                                 D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable,
                                                 IDirect3DSurface9 **surface, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format,
                                                        D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard,
                                                        IDirect3DSurface9 **surface, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst,
                                            const D3D9Point *dstPoint) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *renderTarget, IDirect3DSurface9 *dst) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT swapChain, IDirect3DSurface9 *dst) override;
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst,
                                          const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                          IDirect3DSurface9 **surface, HANDLE *shared) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *surface) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **surface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                                    DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *viewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *viewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *material) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9 *light) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT9 *light) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL *enable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float *plane) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **block) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **block) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *status) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *status) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *passes) override;
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY *entries) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY *entries) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *palette) override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *rect) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *rect) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL software) override;
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override;
    float STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                                                   UINT vertexCount, UINT startIndex, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *data,
                                              UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertexCount,
                                                     UINT count, const void *indices, D3DFORMAT indexFormat,
                                                     const void *data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT dstIndex, UINT count, IDirect3DVertexBuffer9 *dst,
                                              IDirect3DVertexDeclaration9 *decl, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                                      IDirect3DVertexDeclaration9 **decl) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **decl) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD *fvf) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *code, IDirect3DVertexShader9 **shader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *shader) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **shader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const int *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, int *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset,
                                              UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset,
                                              UINT *stride) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT divider) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT *divider) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *buffer) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **buffer) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *code, IDirect3DPixelShader9 **shader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *shader) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **shader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const int *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, int *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL *data, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segments,
                                            const D3DRECTPATCH_INFO *info) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segments, const D3DTRIPATCH_INFO *info) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT handle) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query) override;

    D3DPRESENT_PARAMETERS params_;

    // Sampler numbers: 0-15 for pixel shaders, D3DVERTEXTEXTURESAMPLER0-3
    // (257-260) for vertex shaders. -1 for any other.
    static constexpr uint32_t kSamplerSlots = 20;
    static int SamplerSlot(DWORD sampler);

private:
    template <class Interface>
    HRESULT CreateShaderObject(const DWORD *code, Interface **out, bool pixel);
    // Sets a binding, moving the device's hold from the old object to the new.
    template <class T>
    static void Rebind(T *&slot, T *object);
    void UnbindAll();
    // What the adapter draws into for a render target the program set: the
    // linear-light scene for the back buffer while a scene is drawn there.
    lg::Surface *DrawTarget(DWORD index, Surface *target) const;
    // Sets render target 0 in the adapter again (it resets the viewport and
    // scissor there), with the program's viewport and scissor after it.
    void RebindTarget0();
    // The state D3D9 has after CreateDevice and Reset: the back buffer as
    // render target 0, no depth surface, everything else at its default.
    void DefaultState();
    void Destroy();

    struct Stream
    {
        VertexBuffer *buffer = nullptr;
        UINT offset = 0, stride = 0, divider = 1;
    };

    Direct3D *parent_;
    D3D9Window window_;
    DWORD behavior_;
    Backend *backend_ = nullptr; // the GPU (vk_d3d9_backend.h)
    lg::Device *legacy_ = nullptr; // the back end's Legacy device
    std::unique_ptr<Surface> backBuffer_;
    // While a 3D scene is drawn in linear light (ModernSceneBegin until
    // ModernSceneDone): the back end's image that stands in for the back
    // buffer as render target 0. The program still sees the back buffer.
    lg::Surface *linearScene_ = nullptr;
    bool linearSceneUnfinished_ = false; // logged once (Present)
    std::unique_ptr<SwapChain> swapChain_;
    D3DGAMMARAMP gamma_{};
    bool fullscreen_ = false; // the host was asked for a display mode (ApplyDisplayMode)
    ULONG refs_ = 1;
    uint32_t objects_ = 0;
    bool releasing_ = false; // Release is unbinding: objects going must not delete the device
    DeviceCounters counters_{};
    std::vector<const char *> unsupportedLogged_; // methods already logged (one line each)

    // What the Get* methods answer; the adapter holds the state it draws with.
    Surface *renderTargets_[lg::kMaxRenderTargets] = {};
    Surface *depthStencil_ = nullptr;
    Counted *textureObjects_[kSamplerSlots] = {};
    IDirect3DBaseTexture9 *textures_[kSamplerSlots] = {};
    DWORD samplerStates_[kSamplerSlots][lg::d3d::SAMP_COUNT] = {};
    Stream streams_[lg::kMaxStreams];
    IndexBuffer *indices_ = nullptr;
    VertexShader *vertexShader_ = nullptr;
    PixelShader *pixelShader_ = nullptr;
    VertexDeclaration *declaration_ = nullptr;
    D3DVIEWPORT9 viewport_{};
    RECT scissor_{};
    std::vector<float> vsConstantsF_, psConstantsF_;
    std::vector<int> vsConstantsI_, psConstantsI_;
    std::vector<BOOL> vsConstantsB_, psConstantsB_;
};
} // namespace gfx_vk::d3d9
