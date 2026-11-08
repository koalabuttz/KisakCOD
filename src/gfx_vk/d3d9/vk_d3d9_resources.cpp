// The D3D9 front: reference counts and the resource interfaces (surfaces,
// textures, volumes, buffers, shaders, declarations, queries, the implicit
// swap chain). See vk_d3d9.h.

#include "vk_d3d9_internal.h"

#include <cstring>
#include <type_traits>

namespace gfx_vk::d3d9
{
namespace
{
lg::Box ToBox(const RECT &rect)
{
    lg::Box box;
    box.left = static_cast<uint32_t>(rect.left);
    box.top = static_cast<uint32_t>(rect.top);
    box.right = static_cast<uint32_t>(rect.right);
    box.bottom = static_cast<uint32_t>(rect.bottom);
    box.front = 0;
    box.back = 1;
    return box;
}

bool ValidRect(const RECT &rect)
{
    return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left && rect.bottom > rect.top;
}
} // namespace

// ---- Private data --------------------------------------------------------------------------------

HRESULT PrivateData::Set(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    // D3DSPD_IUNKNOWN stores an interface the store would have to hold a
    // reference on; nothing the renderer does needs it.
    if (flags != 0 || (size && !data))
        return D3DERR_INVALIDCALL;
    Free(guid);
    Entry entry;
    entry.guid = guid;
    entry.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    entries_.push_back(std::move(entry));
    return D3D_OK;
}

HRESULT PrivateData::Get(REFGUID guid, void *data, DWORD *size) const
{
    if (!size)
        return D3DERR_INVALIDCALL;
    for (const Entry &entry : entries_)
    {
        if (!(entry.guid == guid))
            continue;
        const DWORD needed = static_cast<DWORD>(entry.data.size());
        if (!data)
        {
            *size = needed;
            return D3D_OK;
        }
        if (*size < needed)
        {
            *size = needed;
            return D3DERR_MOREDATA;
        }
        if (needed)
            std::memcpy(data, entry.data.data(), needed);
        *size = needed;
        return D3D_OK;
    }
    return D3DERR_NOTFOUND;
}

HRESULT PrivateData::Free(REFGUID guid)
{
    for (size_t i = 0; i < entries_.size(); ++i)
    {
        if (entries_[i].guid == guid)
        {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return D3D_OK;
        }
    }
    return D3DERR_NOTFOUND;
}

// ---- Counts ---------------------------------------------------------------------------------------

Counted::Counted(Device *device, Counted *container, bool deviceChild)
    : device_(device), container_(container), deviceChild_(deviceChild)
{
    if (!container_ && !deviceChild_)
        device_->ObjectCreated();
}

Counted::~Counted()
{
    if (!container_ && !deviceChild_)
        device_->ObjectDestroyed();
}

ULONG Counted::AddRefPublic()
{
    const FrontLock lock;
    if (deviceChild_)
        return device_->AddRef();
    if (container_)
        return container_->AddRefPublic();
    return ++refs_;
}

ULONG Counted::ReleasePublic()
{
    const FrontLock lock;
    if (deviceChild_)
        return device_->Release();
    if (container_)
        return container_->ReleasePublic();
    if (!refs_)
        return 0;
    const ULONG refs = --refs_;
    DestroyIfUnused();
    return refs;
}

void Counted::Bind()
{
    if (deviceChild_)
        return;
    if (container_)
    {
        container_->Bind();
        return;
    }
    ++binds_;
}

void Counted::Unbind()
{
    if (deviceChild_)
        return;
    if (container_)
    {
        container_->Unbind();
        return;
    }
    if (binds_)
        --binds_;
    DestroyIfUnused();
}

void Counted::DestroyIfUnused()
{
    if (!refs_ && !binds_)
        delete this;
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Object<Interface>::QueryInterface(REFIID riid, void **object)
{
    const FrontLock lock;
    if (!object)
        return E_POINTER;
    *object = nullptr;
    bool match = riid == __uuidof(IUnknown) || riid == __uuidof(Interface);
    if constexpr (std::is_base_of_v<IDirect3DResource9, Interface>)
        match = match || riid == __uuidof(IDirect3DResource9);
    if constexpr (std::is_base_of_v<IDirect3DBaseTexture9, Interface>)
        match = match || riid == __uuidof(IDirect3DBaseTexture9);
    if (!match)
        return E_NOINTERFACE;
    *object = static_cast<Interface *>(this);
    AddRefPublic();
    return S_OK;
}

// ---- IDirect3DResource9 ---------------------------------------------------------------------------

template <class Interface>
HRESULT STDMETHODCALLTYPE Resource<Interface>::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    this->device_->AddRef();
    *device = this->device_;
    return D3D_OK;
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Resource<Interface>::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    const FrontLock lock;
    return private_.Set(guid, data, size, flags);
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Resource<Interface>::GetPrivateData(REFGUID guid, void *data, DWORD *size)
{
    const FrontLock lock;
    return private_.Get(guid, data, size);
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Resource<Interface>::FreePrivateData(REFGUID guid)
{
    const FrontLock lock;
    return private_.Free(guid);
}

template <class Interface>
DWORD STDMETHODCALLTYPE Resource<Interface>::SetPriority(DWORD priority)
{
    const FrontLock lock;
    const DWORD old = priority_;
    priority_ = priority;
    return old;
}

template <class Interface>
DWORD STDMETHODCALLTYPE Resource<Interface>::GetPriority()
{
    const FrontLock lock;
    return priority_;
}

// ---- Surfaces -------------------------------------------------------------------------------------

Surface::Surface(Device *device, lg::Surface *surface, DWORD usage, D3DPOOL pool)
    : Resource(device, D3DRTYPE_SURFACE), surface_(surface), owned_(true), usage_(usage), pool_(pool)
{
}

Surface::Surface(Device *device, lg::Surface *surface, Counted *texture, DWORD usage, D3DPOOL pool)
    : Resource(device, D3DRTYPE_SURFACE, texture), surface_(surface), owned_(false), usage_(usage), pool_(pool)
{
}

Surface::Surface(Device *device, lg::Surface *surface)
    : Resource(device, D3DRTYPE_SURFACE, nullptr, true), surface_(surface), owned_(false),
      usage_(D3DUSAGE_RENDERTARGET), pool_(D3DPOOL_DEFAULT)
{
}

Surface::~Surface()
{
    if (owned_)
        lg::DestroySurface(device_->legacy(), surface_);
}

HRESULT STDMETHODCALLTYPE Surface::GetContainer(REFIID riid, void **container)
{
    const FrontLock lock;
    if (!container)
        return D3DERR_INVALIDCALL;
    // A texture level's container is its texture; any other surface's is the
    // device (D3D9 answers the swap chain for the back buffer, which counts as
    // the device here).
    if (container_)
        return container_->Unknown()->QueryInterface(riid, container);
    return device_->QueryInterface(riid, container);
}

HRESULT STDMETHODCALLTYPE Surface::GetDesc(D3DSURFACE_DESC *desc)
{
    const FrontLock lock;
    if (!desc)
        return D3DERR_INVALIDCALL;
    uint32_t width = 0, height = 0;
    lg::d3d::Format format = lg::d3d::FMT_UNKNOWN;
    lg::SurfaceSize(surface_, width, height, format);
    desc->Format = static_cast<D3DFORMAT>(format);
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->MultiSampleQuality = 0;
    desc->Width = width;
    desc->Height = height;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    const FrontLock lock;
    if (!locked)
        return D3DERR_INVALIDCALL;
    locked->pBits = nullptr;
    locked->Pitch = 0;
    lg::Box box;
    if (rect)
    {
        if (!ValidRect(*rect))
            return D3DERR_INVALIDCALL;
        box = ToBox(*rect);
    }
    lg::LockedRect out;
    if (!lg::LockSurface(device_->legacy(), surface_, rect ? &box : nullptr, flags, out))
        return D3DERR_INVALIDCALL;
    locked->pBits = out.bits;
    locked->Pitch = static_cast<INT>(out.pitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Surface::UnlockRect()
{
    const FrontLock lock;
    lg::UnlockSurface(device_->legacy(), surface_);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Surface::GetDC(HDC *)
{
    const FrontLock lock;
    return device_->Unsupported("IDirect3DSurface9::GetDC");
}

HRESULT STDMETHODCALLTYPE Surface::ReleaseDC(HDC)
{
    const FrontLock lock;
    return device_->Unsupported("IDirect3DSurface9::ReleaseDC");
}

// ---- Textures -------------------------------------------------------------------------------------

lg::Texture *TextureHandle(IDirect3DBaseTexture9 *texture)
{
    if (!texture)
        return nullptr;
    switch (texture->GetType())
    {
    case D3DRTYPE_TEXTURE:
        return static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(texture))->state_.texture;
    case D3DRTYPE_CUBETEXTURE:
        return static_cast<CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(texture))->state_.texture;
    case D3DRTYPE_VOLUMETEXTURE:
        return static_cast<VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(texture))->state_.texture;
    default:
        return nullptr;
    }
}

Counted *TextureObject(IDirect3DBaseTexture9 *texture)
{
    if (!texture)
        return nullptr;
    switch (texture->GetType())
    {
    case D3DRTYPE_TEXTURE:
        return static_cast<Texture *>(static_cast<IDirect3DTexture9 *>(texture));
    case D3DRTYPE_CUBETEXTURE:
        return static_cast<CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(texture));
    case D3DRTYPE_VOLUMETEXTURE:
        return static_cast<VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(texture));
    default:
        return nullptr;
    }
}

template <class Interface>
BaseTexture<Interface>::~BaseTexture()
{
    state_.surfaces.clear();
    lg::DestroyTexture(this->device_->legacy(), state_.texture);
}

template <class Interface>
DWORD STDMETHODCALLTYPE BaseTexture<Interface>::SetLOD(DWORD lod)
{
    const FrontLock lock;
    // Only managed textures have a LOD; the adapter samples every level.
    const DWORD old = state_.lod;
    if (state_.pool == D3DPOOL_MANAGED)
        state_.lod = lod < state_.levels ? lod : state_.levels - 1;
    return old;
}

template <class Interface>
HRESULT STDMETHODCALLTYPE BaseTexture<Interface>::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE)
{
    const FrontLock lock;
    return this->device_->Unsupported("IDirect3DBaseTexture9::SetAutoGenFilterType");
}

template <class Interface>
HRESULT BaseTexture<Interface>::LevelDesc(UINT level, D3DSURFACE_DESC *desc) const
{
    if (!desc || level >= state_.levels)
        return D3DERR_INVALIDCALL;
    desc->Format = state_.format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = state_.usage;
    desc->Pool = state_.pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->MultiSampleQuality = 0;
    desc->Width = state_.width >> level ? state_.width >> level : 1;
    desc->Height = state_.height >> level ? state_.height >> level : 1;
    return D3D_OK;
}

template <class Interface>
HRESULT BaseTexture<Interface>::LevelSurface(UINT face, UINT level, IDirect3DSurface9 **surface)
{
    if (!surface)
        return D3DERR_INVALIDCALL;
    *surface = nullptr;
    if (face >= state_.faces || level >= state_.levels)
        return D3DERR_INVALIDCALL;
    std::unique_ptr<Surface> &slot = state_.surfaces[face * state_.levels + level];
    if (!slot)
    {
        lg::Surface *handle = lg::GetSurfaceLevel(this->device_->legacy(), state_.texture, face, level);
        if (!handle)
            return D3DERR_INVALIDCALL;
        slot.reset(new Surface(this->device_, handle, this, state_.usage, state_.pool));
    }
    slot->AddRef();
    *surface = slot.get();
    return D3D_OK;
}

template <class Interface>
HRESULT BaseTexture<Interface>::Lock(UINT face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if (!locked)
        return D3DERR_INVALIDCALL;
    locked->pBits = nullptr;
    locked->Pitch = 0;
    if (face >= state_.faces || level >= state_.levels)
        return D3DERR_INVALIDCALL;
    lg::Box box;
    if (rect)
    {
        if (!ValidRect(*rect))
            return D3DERR_INVALIDCALL;
        box = ToBox(*rect);
    }
    lg::LockedRect out;
    if (!lg::LockRect(this->device_->legacy(), state_.texture, face, level, rect ? &box : nullptr, flags, out))
        return D3DERR_INVALIDCALL;
    locked->pBits = out.bits;
    locked->Pitch = static_cast<INT>(out.pitch);
    return D3D_OK;
}

template <class Interface>
HRESULT BaseTexture<Interface>::Unlock(UINT face, UINT level)
{
    if (face >= state_.faces || level >= state_.levels)
        return D3DERR_INVALIDCALL;
    lg::UnlockRect(this->device_->legacy(), state_.texture, face, level);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    const FrontLock lock;
    return LevelDesc(level, desc);
}

HRESULT STDMETHODCALLTYPE Texture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface)
{
    const FrontLock lock;
    return LevelSurface(0, level, surface);
}

HRESULT STDMETHODCALLTYPE Texture::LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    const FrontLock lock;
    return Lock(0, level, locked, rect, flags);
}

HRESULT STDMETHODCALLTYPE Texture::UnlockRect(UINT level)
{
    const FrontLock lock;
    return Unlock(0, level);
}

HRESULT STDMETHODCALLTYPE Texture::AddDirtyRect(const RECT *)
{
    const FrontLock lock;
    // Every unlock uploads what was written, so there is nothing to mark.
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    const FrontLock lock;
    return LevelDesc(level, desc);
}

HRESULT STDMETHODCALLTYPE CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level,
                                                         IDirect3DSurface9 **surface)
{
    const FrontLock lock;
    return LevelSurface(static_cast<UINT>(face), level, surface);
}

HRESULT STDMETHODCALLTYPE CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked,
                                                const RECT *rect, DWORD flags)
{
    const FrontLock lock;
    return Lock(static_cast<UINT>(face), level, locked, rect, flags);
}

HRESULT STDMETHODCALLTYPE CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level)
{
    const FrontLock lock;
    return Unlock(static_cast<UINT>(face), level);
}

HRESULT STDMETHODCALLTYPE CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES, const RECT *)
{
    const FrontLock lock;
    return D3D_OK;
}

// ---- Volumes --------------------------------------------------------------------------------------

Volume::Volume(Device *device, VolumeTexture *texture, UINT level)
    : Object(device, texture), texture_(texture), level_(level)
{
}

HRESULT STDMETHODCALLTYPE Volume::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    device_->AddRef();
    *device = device_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Volume::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    const FrontLock lock;
    return private_.Set(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE Volume::GetPrivateData(REFGUID guid, void *data, DWORD *size)
{
    const FrontLock lock;
    return private_.Get(guid, data, size);
}

HRESULT STDMETHODCALLTYPE Volume::FreePrivateData(REFGUID guid)
{
    const FrontLock lock;
    return private_.Free(guid);
}

HRESULT STDMETHODCALLTYPE Volume::GetContainer(REFIID riid, void **container)
{
    const FrontLock lock;
    if (!container)
        return D3DERR_INVALIDCALL;
    return texture_->QueryInterface(riid, container);
}

HRESULT STDMETHODCALLTYPE Volume::GetDesc(D3DVOLUME_DESC *desc)
{
    const FrontLock lock;
    return texture_->GetLevelDesc(level_, desc);
}

HRESULT STDMETHODCALLTYPE Volume::LockBox(D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags)
{
    const FrontLock lock;
    return texture_->LockBox(level_, locked, box, flags);
}

HRESULT STDMETHODCALLTYPE Volume::UnlockBox()
{
    const FrontLock lock;
    return texture_->UnlockBox(level_);
}

HRESULT STDMETHODCALLTYPE VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *desc)
{
    const FrontLock lock;
    if (!desc || level >= state_.levels)
        return D3DERR_INVALIDCALL;
    desc->Format = state_.format;
    desc->Type = D3DRTYPE_VOLUME;
    desc->Usage = state_.usage;
    desc->Pool = state_.pool;
    desc->Width = state_.width >> level ? state_.width >> level : 1;
    desc->Height = state_.height >> level ? state_.height >> level : 1;
    desc->Depth = state_.depth >> level ? state_.depth >> level : 1;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::GetVolumeLevel(UINT level, IDirect3DVolume9 **volume)
{
    const FrontLock lock;
    if (!volume)
        return D3DERR_INVALIDCALL;
    *volume = nullptr;
    if (level >= state_.levels)
        return D3DERR_INVALIDCALL;
    if (volumes_.size() < state_.levels)
        volumes_.resize(state_.levels);
    if (!volumes_[level])
        volumes_[level].reset(new Volume(device_, this, level));
    volumes_[level]->AddRef();
    *volume = volumes_[level].get();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags)
{
    const FrontLock lock;
    if (!locked)
        return D3DERR_INVALIDCALL;
    locked->pBits = nullptr;
    locked->RowPitch = 0;
    locked->SlicePitch = 0;
    if (level >= state_.levels)
        return D3DERR_INVALIDCALL;
    lg::Box region;
    if (box)
    {
        if (box->Right <= box->Left || box->Bottom <= box->Top || box->Back <= box->Front)
            return D3DERR_INVALIDCALL;
        region.left = box->Left;
        region.top = box->Top;
        region.front = box->Front;
        region.right = box->Right;
        region.bottom = box->Bottom;
        region.back = box->Back;
    }
    lg::LockedRect out;
    if (!lg::LockRect(device_->legacy(), state_.texture, 0, level, box ? &region : nullptr, flags, out))
        return D3DERR_INVALIDCALL;
    locked->pBits = out.bits;
    locked->RowPitch = static_cast<INT>(out.pitch);
    locked->SlicePitch = static_cast<INT>(out.slicePitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::UnlockBox(UINT level)
{
    const FrontLock lock;
    return Unlock(0, level);
}

HRESULT STDMETHODCALLTYPE VolumeTexture::AddDirtyBox(const D3DBOX *)
{
    const FrontLock lock;
    return D3D_OK;
}

// ---- Buffers --------------------------------------------------------------------------------------

VertexBuffer::VertexBuffer(Device *device, lg::VertexBuffer *buffer, UINT size, DWORD usage, DWORD fvf,
                           D3DPOOL pool)
    : Resource(device, D3DRTYPE_VERTEXBUFFER), buffer_(buffer), size_(size), usage_(usage), fvf_(fvf), pool_(pool)
{
}

VertexBuffer::~VertexBuffer()
{
    lg::DestroyVertexBuffer(device_->legacy(), buffer_);
}

HRESULT STDMETHODCALLTYPE VertexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    const FrontLock lock;
    if (!data)
        return D3DERR_INVALIDCALL;
    *data = lg::Lock(device_->legacy(), buffer_, offset, size, flags);
    return *data ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE VertexBuffer::Unlock()
{
    const FrontLock lock;
    lg::Unlock(device_->legacy(), buffer_);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc)
{
    const FrontLock lock;
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc->Format = D3DFMT_VERTEXDATA;
    desc->Type = D3DRTYPE_VERTEXBUFFER;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Size = size_;
    desc->FVF = fvf_;
    return D3D_OK;
}

IndexBuffer::IndexBuffer(Device *device, lg::IndexBuffer *buffer, UINT size, DWORD usage, D3DFORMAT format,
                         D3DPOOL pool)
    : Resource(device, D3DRTYPE_INDEXBUFFER), buffer_(buffer), size_(size), usage_(usage), format_(format),
      pool_(pool)
{
}

IndexBuffer::~IndexBuffer()
{
    lg::DestroyIndexBuffer(device_->legacy(), buffer_);
}

HRESULT STDMETHODCALLTYPE IndexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    const FrontLock lock;
    if (!data)
        return D3DERR_INVALIDCALL;
    *data = lg::Lock(device_->legacy(), buffer_, offset, size, flags);
    return *data ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE IndexBuffer::Unlock()
{
    const FrontLock lock;
    lg::Unlock(device_->legacy(), buffer_);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *desc)
{
    const FrontLock lock;
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc->Format = format_;
    desc->Type = D3DRTYPE_INDEXBUFFER;
    desc->Usage = usage_;
    desc->Pool = pool_;
    desc->Size = size_;
    return D3D_OK;
}

// ---- Shaders and declarations -------------------------------------------------------------------

template <class Interface>
Shader<Interface>::Shader(Device *device, lg::Shader *shader, const DWORD *code, size_t bytes)
    : Object<Interface>(device), shader_(shader),
      code_(reinterpret_cast<const uint8_t *>(code), reinterpret_cast<const uint8_t *>(code) + bytes)
{
}

template <class Interface>
Shader<Interface>::~Shader()
{
    lg::DestroyShader(this->device_->legacy(), shader_);
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Shader<Interface>::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    this->device_->AddRef();
    *device = this->device_;
    return D3D_OK;
}

template <class Interface>
HRESULT STDMETHODCALLTYPE Shader<Interface>::GetFunction(void *data, UINT *size)
{
    const FrontLock lock;
    if (!size)
        return D3DERR_INVALIDCALL;
    const UINT bytes = static_cast<UINT>(code_.size());
    if (!data)
    {
        *size = bytes;
        return D3D_OK;
    }
    if (*size < bytes)
        return D3DERR_INVALIDCALL;
    std::memcpy(data, code_.data(), bytes);
    *size = bytes;
    return D3D_OK;
}

VertexDeclaration::VertexDeclaration(Device *device, lg::VertexDecl *decl, const D3DVERTEXELEMENT9 *elements,
                                     UINT count)
    : Object(device), decl_(decl), elements_(elements, elements + count)
{
    const D3DVERTEXELEMENT9 end = D3DDECL_END();
    elements_.push_back(end);
}

VertexDeclaration::~VertexDeclaration()
{
    lg::DestroyVertexDecl(device_->legacy(), decl_);
}

HRESULT STDMETHODCALLTYPE VertexDeclaration::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    device_->AddRef();
    *device = device_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count)
{
    const FrontLock lock;
    if (!count)
        return D3DERR_INVALIDCALL;
    const UINT total = static_cast<UINT>(elements_.size());
    if (elements)
        std::memcpy(elements, elements_.data(), total * sizeof(D3DVERTEXELEMENT9));
    *count = total;
    return D3D_OK;
}

// ---- Queries --------------------------------------------------------------------------------------

Query::Query(Device *device, lg::Query *query, D3DQUERYTYPE type) : Object(device), query_(query), type_(type) {}

Query::~Query()
{
    lg::DestroyQuery(device_->legacy(), query_);
}

HRESULT STDMETHODCALLTYPE Query::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    device_->AddRef();
    *device = device_;
    return D3D_OK;
}

DWORD STDMETHODCALLTYPE Query::GetDataSize()
{
    const FrontLock lock;
    return type_ == D3DQUERYTYPE_EVENT ? sizeof(BOOL) : sizeof(DWORD);
}

HRESULT STDMETHODCALLTYPE Query::Issue(DWORD flags)
{
    const FrontLock lock;
    if (flags == D3DISSUE_END)
        lg::IssueQuery(device_->legacy(), query_, false);
    else if (flags == D3DISSUE_BEGIN && type_ == D3DQUERYTYPE_OCCLUSION)
        lg::IssueQuery(device_->legacy(), query_, true);
    else if (flags != D3DISSUE_BEGIN)
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Query::GetData(void *data, DWORD size, DWORD flags)
{
    const FrontLock lock;
    if (size && !data)
        return D3DERR_INVALIDCALL;
    uint32_t result = 0;
    if (!lg::GetQueryData(device_->legacy(), query_, (flags & D3DGETDATA_FLUSH) != 0, result))
        return S_FALSE;
    if (type_ == D3DQUERYTYPE_EVENT)
    {
        if (size >= sizeof(BOOL))
            *static_cast<BOOL *>(data) = TRUE;
    }
    else if (size >= sizeof(DWORD))
    {
        *static_cast<DWORD *>(data) = result;
    }
    return S_OK;
}

// ---- The implicit swap chain ----------------------------------------------------------------------

SwapChain::SwapChain(Device *device) : Object(device, nullptr, true) {}

HRESULT STDMETHODCALLTYPE SwapChain::Present(const RECT *src, const RECT *dst, D3D9Window window, const RGNDATA *dirty,
                                             DWORD)
{
    const FrontLock lock;
    return device_->Present(src, dst, window, dirty);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFrontBufferData(IDirect3DSurface9 *dst)
{
    const FrontLock lock;
    return device_->GetFrontBufferData(0, dst);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface)
{
    const FrontLock lock;
    return device_->GetBackBuffer(0, index, type, surface);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRasterStatus(D3DRASTER_STATUS *status)
{
    const FrontLock lock;
    return device_->GetRasterStatus(0, status);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDisplayMode(D3DDISPLAYMODE *mode)
{
    const FrontLock lock;
    return device_->GetDisplayMode(0, mode);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDevice(IDirect3DDevice9 **device)
{
    const FrontLock lock;
    if (!device)
        return D3DERR_INVALIDCALL;
    device_->AddRef();
    *device = device_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *params)
{
    const FrontLock lock;
    if (!params)
        return D3DERR_INVALIDCALL;
    *params = device_->params_;
    return D3D_OK;
}

template class Object<IDirect3DSurface9>;
template class Object<IDirect3DTexture9>;
template class Object<IDirect3DCubeTexture9>;
template class Object<IDirect3DVolumeTexture9>;
template class Object<IDirect3DVolume9>;
template class Object<IDirect3DVertexBuffer9>;
template class Object<IDirect3DIndexBuffer9>;
template class Object<IDirect3DVertexShader9>;
template class Object<IDirect3DPixelShader9>;
template class Object<IDirect3DVertexDeclaration9>;
template class Object<IDirect3DQuery9>;
template class Object<IDirect3DSwapChain9>;
template class Resource<IDirect3DSurface9>;
template class Resource<IDirect3DTexture9>;
template class Resource<IDirect3DCubeTexture9>;
template class Resource<IDirect3DVolumeTexture9>;
template class Resource<IDirect3DVertexBuffer9>;
template class Resource<IDirect3DIndexBuffer9>;
template class BaseTexture<IDirect3DTexture9>;
template class BaseTexture<IDirect3DCubeTexture9>;
template class BaseTexture<IDirect3DVolumeTexture9>;
template class Shader<IDirect3DVertexShader9>;
template class Shader<IDirect3DPixelShader9>;
} // namespace gfx_vk::d3d9
