// Vulkan Legacy adapter: formats, textures, surfaces, buffers, shaders and
// vertex declarations. See vk_legacy.h.

#include "vk_legacy_internal.h"

namespace gfx_vk::legacy
{
namespace
{
constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R;
constexpr VkComponentSwizzle G = VK_COMPONENT_SWIZZLE_G;
constexpr VkComponentSwizzle B = VK_COMPONENT_SWIZZLE_B;
constexpr VkComponentSwizzle A = VK_COMPONENT_SWIZZLE_A;
constexpr VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY;
constexpr VkComponentSwizzle One = VK_COMPONENT_SWIZZLE_ONE;
constexpr VkComponentSwizzle Zero = VK_COMPONENT_SWIZZLE_ZERO;

// D3D9 formats as DXVK maps them (d3d9_format.cpp). D3D9 reads channels a
// format lacks as 1 (alpha and the colour of float formats), except A8, which
// reads colour as 0.
const FormatInfo kFormats[] = {
    {d3d::FMT_A8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, {I, I, I, I}, 4, false, false, false},
    {d3d::FMT_X8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, {I, I, I, One}, 4, false, false, true},
    {d3d::FMT_A8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, {I, I, I, I}, 4, false, false, false},
    {d3d::FMT_X8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, {I, I, I, One}, 4, false, false, true},
    {d3d::FMT_R5G6B5, VK_FORMAT_R5G6B5_UNORM_PACK16, {I, I, I, One}, 2, false, false, true},
    {d3d::FMT_A1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, {I, I, I, I}, 2, false, false, false},
    {d3d::FMT_X1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, {I, I, I, One}, 2, false, false, true},
    {d3d::FMT_A4R4G4B4, VK_FORMAT_B4G4R4A4_UNORM_PACK16, {G, R, A, B}, 2, false, false, false},
    {d3d::FMT_A8, VK_FORMAT_R8_UNORM, {Zero, Zero, Zero, R}, 1, false, false, false},
    {d3d::FMT_L8, VK_FORMAT_R8_UNORM, {R, R, R, One}, 1, false, false, false},
    {d3d::FMT_A8L8, VK_FORMAT_R8G8_UNORM, {R, R, R, G}, 2, false, false, false},
    {d3d::FMT_V8U8, VK_FORMAT_R8G8_SNORM, {R, G, One, One}, 2, false, false, false},
    {d3d::FMT_G16R16, VK_FORMAT_R16G16_UNORM, {R, G, One, One}, 4, false, false, false},
    {d3d::FMT_A16B16G16R16, VK_FORMAT_R16G16B16A16_UNORM, {I, I, I, I}, 8, false, false, false},
    {d3d::FMT_R16F, VK_FORMAT_R16_SFLOAT, {R, One, One, One}, 2, false, false, false},
    {d3d::FMT_G16R16F, VK_FORMAT_R16G16_SFLOAT, {R, G, One, One}, 4, false, false, false},
    {d3d::FMT_A16B16G16R16F, VK_FORMAT_R16G16B16A16_SFLOAT, {I, I, I, I}, 8, false, false, false},
    {d3d::FMT_R32F, VK_FORMAT_R32_SFLOAT, {R, One, One, One}, 4, false, false, false},
    {d3d::FMT_G32R32F, VK_FORMAT_R32G32_SFLOAT, {R, G, One, One}, 8, false, false, false},
    {d3d::FMT_A32B32G32R32F, VK_FORMAT_R32G32B32A32_SFLOAT, {I, I, I, I}, 16, false, false, false},
    {d3d::FMT_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {I, I, I, I}, 8, true, false, false},
    {d3d::FMT_DXT2, VK_FORMAT_BC2_UNORM_BLOCK, {I, I, I, I}, 16, true, false, false},
    {d3d::FMT_DXT3, VK_FORMAT_BC2_UNORM_BLOCK, {I, I, I, I}, 16, true, false, false},
    {d3d::FMT_DXT4, VK_FORMAT_BC3_UNORM_BLOCK, {I, I, I, I}, 16, true, false, false},
    {d3d::FMT_DXT5, VK_FORMAT_BC3_UNORM_BLOCK, {I, I, I, I}, 16, true, false, false},
    // Depth formats all become the core's depth-stencil format.
    {d3d::FMT_D16, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D16_LOCKABLE, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D24S8, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D24X8, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D24X4S4, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D24FS8, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D32, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D15S1, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
    {d3d::FMT_D32F_LOCKABLE, VK_FORMAT_UNDEFINED, {R, R, R, One}, 4, false, true, false},
};

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

uint32_t BlocksAcross(const FormatInfo &format, uint32_t texels)
{
    return format.compressed ? (texels + 3) / 4 : texels;
}

// Bytes of one level of one face, rows of blocks tightly packed.
void LevelLayout(const Texture &texture, uint32_t level, uint32_t &rowBytes, uint32_t &rows, uint32_t &depth)
{
    const FormatInfo &format = *texture.format;
    rowBytes = BlocksAcross(format, LevelSize(texture.desc.width, level)) * format.blockBytes;
    rows = BlocksAcross(format, LevelSize(texture.desc.height, level));
    depth = texture.desc.kind == TextureKind::Volume ? LevelSize(texture.desc.depth, level) : 1;
}

bool HostOnly(d3d::Pool pool) { return pool == d3d::POOL_SYSTEMMEM || pool == d3d::POOL_SCRATCH; }

ImageKind ToImageKind(TextureKind kind)
{
    switch (kind)
    {
    case TextureKind::Cube:
        return ImageKind::Cube;
    case TextureKind::Volume:
        return ImageKind::Tex3D;
    default:
        return ImageKind::Tex2D;
    }
}

VkFormatFeatureFlags NeededFeatures(uint32_t usage)
{
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & d3d::USAGE_RENDERTARGET)
        features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & d3d::USAGE_DEPTHSTENCIL)
        features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return features;
}

// The Vulkan format a texture of this D3D9 format and usage is made in. A
// render target in a 16-bit packed format the device cannot render to is
// made as B8G8R8A8 (the packed formats' channels in the same order, so the
// format's swizzle still applies): the shader writes and blends the same
// colour, at 8 bits a channel. VK_FORMAT_UNDEFINED when neither will do.
VkFormat ChooseVkFormat(const Device &device, const FormatInfo &format, uint32_t usage)
{
    const VkFormat packed = format.depth ? device.depthFormat : format.vk;
    const VkFormatFeatureFlags needed = NeededFeatures(usage);
    const bool wideStandIn = (usage & d3d::USAGE_RENDERTARGET)
                             && (format.d3d == d3d::FMT_R5G6B5 || format.d3d == d3d::FMT_X1R5G5B5
                                 || format.d3d == d3d::FMT_A1R5G5B5);
    if (wideStandIn && (Features(device.core).forceWideRenderTargets || !FormatSupports(device.core, packed, needed)))
        return FormatSupports(device.core, VK_FORMAT_B8G8R8A8_UNORM, needed) ? VK_FORMAT_B8G8R8A8_UNORM
                                                                             : VK_FORMAT_UNDEFINED;
    return FormatSupports(device.core, packed, needed) ? packed : VK_FORMAT_UNDEFINED;
}

template <typename T>
T *CreateGpuBuffer(Device &device, uint32_t bytes, uint32_t usage, d3d::Pool pool, bool index, d3d::Format format,
                   const char *name)
{
    if (!Usable(device))
        return nullptr;
    if (!bytes)
    {
        Log(device, "%s buffer %s: zero bytes", index ? "index" : "vertex", name ? name : "");
        return nullptr;
    }
    std::unique_ptr<T> buffer(new T());
    buffer->size = bytes;
    buffer->usage = usage;
    buffer->pool = pool;
    buffer->index = index;
    buffer->indexType = format == d3d::FMT_INDEX32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    buffer->name = name ? name : (index ? "legacy indices" : "legacy vertices");
    buffer->dynamic = (usage & d3d::USAGE_DYNAMIC) || pool == d3d::POOL_SYSTEMMEM;
    BufferDesc desc;
    desc.size = bytes;
    desc.usage = index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    desc.memory = buffer->dynamic ? MemoryKind::Upload : MemoryKind::Device;
    desc.name = buffer->name.c_str();
    Buffer *gpu = CreateBuffer(device.core, desc);
    if (!gpu)
    {
        Fail(device, "cannot create buffer %s (%u bytes): %s", buffer->name.c_str(), bytes, LastError(device.core));
        return nullptr;
    }
    if (buffer->dynamic)
        buffer->backings.push_back({gpu, 0});
    else
        buffer->device = gpu;
    return buffer.release();
}

void DestroyGpuBuffer(Device &device, GpuBuffer *buffer)
{
    for (Backing &backing : buffer->backings)
        DestroyBuffer(device.core, backing.buffer);
    DestroyBuffer(device.core, buffer->device);
}

bool Busy(Device &device, const Backing &backing)
{
    return backing.lastUse && !SerialFinished(device.core, backing.lastUse);
}

void *LockGpuBuffer(Device &device, GpuBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    if (!Usable(device) || !buffer)
        return nullptr;
    if (buffer->locked)
    {
        Log(device, "Lock: %s is already locked", buffer->name.c_str());
        return nullptr;
    }
    if (offset > buffer->size || (bytes && bytes > buffer->size - offset))
    {
        Log(device, "Lock: %u bytes at %u are outside %s (%u bytes)", bytes, offset, buffer->name.c_str(),
            buffer->size);
        return nullptr;
    }
    if (!bytes)
        bytes = buffer->size - offset;
    buffer->locked = true;
    buffer->lockReadOnly = (flags & d3d::LOCK_READONLY) != 0;
    buffer->lockOffset = offset;
    buffer->lockBytes = bytes;
    if (!buffer->dynamic)
    {
        if (buffer->shadow.empty())
            buffer->shadow.assign(buffer->size, 0);
        return buffer->shadow.data() + offset;
    }

    const bool discard = (flags & d3d::LOCK_DISCARD) != 0;
    const bool keep = !discard && !(flags & (d3d::LOCK_NOOVERWRITE | d3d::LOCK_READONLY));
    if ((discard || keep) && Busy(device, buffer->backings[buffer->current]))
    {
        // The GPU may still read this memory: write to another backing. A
        // plain lock keeps the contents, so they are copied over.
        uint32_t next = UINT32_MAX;
        for (uint32_t i = 0; i < buffer->backings.size(); ++i)
        {
            if (i != buffer->current && !Busy(device, buffer->backings[i]))
            {
                next = i;
                break;
            }
        }
        if (next == UINT32_MAX)
        {
            BufferDesc desc;
            desc.size = buffer->size;
            desc.usage = buffer->index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            desc.memory = MemoryKind::Upload;
            desc.name = buffer->name.c_str();
            Buffer *gpu = CreateBuffer(device.core, desc);
            if (!gpu)
            {
                buffer->locked = false;
                Fail(device, "cannot rename buffer %s: %s", buffer->name.c_str(), LastError(device.core));
                return nullptr;
            }
            buffer->backings.push_back({gpu, 0});
            next = static_cast<uint32_t>(buffer->backings.size() - 1);
        }
        if (keep)
            std::memcpy(BufferMapped(buffer->backings[next].buffer),
                        BufferMapped(buffer->backings[buffer->current].buffer), buffer->size);
        buffer->current = next;
        ++device.stats.dynamicRenames;
    }
    return static_cast<uint8_t *>(BufferMapped(buffer->backings[buffer->current].buffer)) + offset;
}

void UnlockGpuBuffer(Device &device, GpuBuffer *buffer)
{
    if (!buffer || !buffer->locked)
        return;
    buffer->locked = false;
    if (buffer->lockReadOnly || !Usable(device))
        return;
    if (buffer->dynamic)
    {
        FlushMapped(device.core, buffer->backings[buffer->current].buffer, buffer->lockOffset, buffer->lockBytes);
        return;
    }
    // Draws recorded so far read the old contents, later ones the new.
    EndPassIfOpen(device);
    void *data = StageBufferWrite(device.core, buffer->device, buffer->lockOffset, buffer->lockBytes);
    if (!data)
    {
        Fail(device, "cannot upload buffer %s: %s", buffer->name.c_str(), LastError(device.core));
        return;
    }
    std::memcpy(data, buffer->shadow.data() + buffer->lockOffset, buffer->lockBytes);
}

} // namespace

const FormatInfo *LookupFormat(d3d::Format format)
{
    for (const FormatInfo &info : kFormats)
    {
        if (info.d3d == format)
            return &info;
    }
    return nullptr;
}

uint32_t LevelSize(uint32_t size, uint32_t level) { return std::max(size >> level, 1u); }

// ---- Textures ----------------------------------------------------------------------------

Texture *CreateTextureInternal(Device &device, const TextureDesc &desc, uint32_t extraUsage)
{
    const FormatInfo *format = LookupFormat(desc.format);
    const char *name = desc.name ? desc.name : "legacy texture";
    if (!format)
    {
        Log(device, "texture %s: format %u is not supported", name, static_cast<uint32_t>(desc.format));
        return nullptr;
    }
    const bool volume = desc.kind == TextureKind::Volume;
    if (!desc.width || !desc.height || (volume && !desc.depth) || (desc.kind == TextureKind::Cube && desc.width != desc.height))
    {
        Log(device, "texture %s: bad size %ux%ux%u", name, desc.width, desc.height, desc.depth);
        return nullptr;
    }
    std::unique_ptr<Texture> texture(new Texture());
    texture->desc = desc;
    texture->desc.depth = volume ? desc.depth : 1;
    const uint32_t chain = FullChain(desc.width, desc.height, texture->desc.depth);
    texture->desc.levels = desc.levels ? std::min(desc.levels, chain) : chain;
    texture->name = name;
    texture->desc.name = nullptr; // the caller's string may not outlive the texture
    texture->format = format;
    texture->vkFormat = format->depth ? device.depthFormat : format->vk;
    texture->faces = desc.kind == TextureKind::Cube ? 6 : 1;
    texture->subresources.resize(texture->faces * texture->desc.levels);
    texture->surfaces.resize(texture->subresources.size());

    if (!HostOnly(desc.pool))
    {
        if (format->depth && !(desc.usage & d3d::USAGE_DEPTHSTENCIL))
        {
            Log(device, "texture %s: depth formats need D3DUSAGE_DEPTHSTENCIL", name);
            return nullptr;
        }
        if ((desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL))
            && (desc.kind != TextureKind::Tex2D || texture->desc.levels != 1))
        {
            // Attachments use the image's whole view, which must be one 2D level.
            Log(device, "texture %s: render targets and depth surfaces must be 2D with one level", name);
            return nullptr;
        }
        const VkFormat chosen = ChooseVkFormat(device, *format, desc.usage);
        if (chosen == VK_FORMAT_UNDEFINED)
        {
            Log(device, "texture %s: the device cannot use %s for usage 0x%x", name, FormatName(texture->vkFormat),
                desc.usage);
            return nullptr;
        }
        texture->vkFormat = chosen;
        ImageDesc image;
        image.kind = ToImageKind(desc.kind);
        image.format = texture->vkFormat;
        image.width = desc.width;
        image.height = desc.height;
        image.depth = texture->desc.depth;
        image.mipLevels = texture->desc.levels;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extraUsage;
        if (desc.usage & d3d::USAGE_RENDERTARGET)
            image.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.usage & d3d::USAGE_DEPTHSTENCIL)
            image.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sampleSwizzle = format->swizzle;
        image.name = texture->name.c_str();
        texture->image = CreateImage(device.core, image);
        if (!texture->image)
        {
            Fail(device, "cannot create texture %s: %s", name, LastError(device.core));
            return nullptr;
        }
    }
    return texture.release();
}

void DestroyTextureNow(Device &device, Texture *texture)
{
    if (!texture)
        return;
    DestroyImage(device.core, texture->image);
    delete texture;
}

Texture *CreateTexture(Device *device, const TextureDesc &desc)
{
    if (!Usable(*device))
        return nullptr;
    return CreateTextureInternal(*device, desc, 0);
}

void DestroyTexture(Device *device, Texture *texture)
{
    if (!texture || texture == device->backBuffer)
        return;
    ForgetTexture(*device, texture);
    DestroyTextureNow(*device, texture);
}

const TextureDesc &Describe(const Texture *texture) { return texture->desc; }

bool FormatSupported(Device *device, d3d::Format format, uint32_t usage)
{
    const FormatInfo *info = LookupFormat(format);
    if (!info || (info->depth && (usage & d3d::USAGE_RENDERTARGET)) || (!info->depth && (usage & d3d::USAGE_DEPTHSTENCIL))
        || (info->compressed && (usage & d3d::USAGE_RENDERTARGET)))
        return false;
    if (!device)
        return true;
    return ChooseVkFormat(*device, *info, usage) != VK_FORMAT_UNDEFINED;
}

bool LockRect(Device *device_, Texture *texture, uint32_t face, uint32_t level, const Box *box, uint32_t flags,
              LockedRect &out)
{
    Device &device = *device_;
    out = LockedRect{};
    if (!Usable(device) || !texture)
        return false;
    const TextureDesc &desc = texture->desc;
    if (face >= texture->faces || level >= desc.levels)
        return false;
    if (texture->format->depth || (desc.usage & d3d::USAGE_RENDERTARGET))
    {
        Log(device, "LockRect: %s is a render target or depth surface", texture->name.c_str());
        return false;
    }
    Subresource &sub = texture->subresources[face * desc.levels + level];
    if (sub.locked)
    {
        Log(device, "LockRect: %s level %u is already locked", texture->name.c_str(), level);
        return false;
    }
    Box whole;
    whole.right = LevelSize(desc.width, level);
    whole.bottom = LevelSize(desc.height, level);
    whole.back = desc.kind == TextureKind::Volume ? LevelSize(desc.depth, level) : 1;
    Box region = whole;
    if (box && box->right)
    {
        region = *box;
        if (!region.back)
        {
            region.front = 0;
            region.back = whole.back;
        }
    }
    const bool blockAligned = !texture->format->compressed
        || ((region.left % 4) == 0 && (region.top % 4) == 0
            && ((region.right % 4) == 0 || region.right == whole.right)
            && ((region.bottom % 4) == 0 || region.bottom == whole.bottom));
    if (region.left >= region.right || region.top >= region.bottom || region.front >= region.back
        || region.right > whole.right || region.bottom > whole.bottom || region.back > whole.back || !blockAligned)
    {
        Log(device, "LockRect: bad region on %s level %u", texture->name.c_str(), level);
        return false;
    }
    uint32_t rowBytes = 0, rows = 0, depth = 0;
    LevelLayout(*texture, level, rowBytes, rows, depth);
    const size_t slice = static_cast<size_t>(rowBytes) * rows;
    if (sub.cpu.empty())
        sub.cpu.assign(slice * depth, 0);
    sub.locked = true;
    sub.readOnly = (flags & d3d::LOCK_READONLY) != 0;
    sub.box = region;
    const uint32_t blockX = texture->format->compressed ? region.left / 4 : region.left;
    const uint32_t blockY = texture->format->compressed ? region.top / 4 : region.top;
    out.bits = sub.cpu.data() + region.front * slice + static_cast<size_t>(blockY) * rowBytes
        + static_cast<size_t>(blockX) * texture->format->blockBytes;
    out.pitch = rowBytes;
    out.slicePitch = static_cast<uint32_t>(slice);
    return true;
}

void UnlockRect(Device *device_, Texture *texture, uint32_t face, uint32_t level)
{
    Device &device = *device_;
    if (!texture || face >= texture->faces || level >= texture->desc.levels)
        return;
    Subresource &sub = texture->subresources[face * texture->desc.levels + level];
    if (!sub.locked)
        return;
    sub.locked = false;
    if (!texture->image || sub.readOnly || !Usable(device))
        return;

    EndPassIfOpen(device);
    const TimeInto timer(device.stats.uploadMicros);
    ++device.stats.textureUploads;
    uint32_t rowBytes = 0, rows = 0, depth = 0;
    LevelLayout(*texture, level, rowBytes, rows, depth);
    const size_t slice = static_cast<size_t>(rowBytes) * rows;
    const FormatInfo &format = *texture->format;
    ImageRegion region;
    region.mipLevel = level;
    region.layer = face;
    region.x = sub.box.left;
    region.y = sub.box.top;
    region.z = sub.box.front;
    region.width = sub.box.right - sub.box.left;
    region.height = sub.box.bottom - sub.box.top;
    region.depth = sub.box.back - sub.box.front;
    uint32_t stageRow = 0, stageSlice = 0;
    uint8_t *staged =
        static_cast<uint8_t *>(StageImageWrite(device.core, texture->image, region, &stageRow, &stageSlice));
    if (!staged)
    {
        Fail(device, "cannot upload %s level %u: %s", texture->name.c_str(), level, LastError(device.core));
        return;
    }
    const uint32_t blockX = format.compressed ? region.x / 4 : region.x;
    const uint32_t blockY = format.compressed ? region.y / 4 : region.y;
    const uint32_t blockRows = BlocksAcross(format, region.height);
    for (uint32_t z = 0; z < region.depth; ++z)
    {
        for (uint32_t row = 0; row < blockRows; ++row)
        {
            const uint8_t *src = sub.cpu.data() + (region.z + z) * slice + static_cast<size_t>(blockY + row) * rowBytes
                + static_cast<size_t>(blockX) * format.blockBytes;
            std::memcpy(staged + static_cast<size_t>(z) * stageSlice + static_cast<size_t>(row) * stageRow, src,
                        stageRow);
        }
    }
    device.stats.uploadBytes += static_cast<uint64_t>(stageRow) * blockRows * region.depth;
    // The GPU keeps the texture; D3D9's managed copy is not kept, to save
    // memory (a 32-bit process has little). See docs/vulkan-legacy-adapter.md.
    std::vector<uint8_t>().swap(sub.cpu);
}

// ---- Surfaces ----------------------------------------------------------------------------

Surface *GetSurfaceLevel(Device *device, Texture *texture, uint32_t face, uint32_t level)
{
    (void)device;
    if (!texture || face >= texture->faces || level >= texture->desc.levels)
        return nullptr;
    if (texture == device->backBuffer)
        return &device->backBufferSurface;
    std::unique_ptr<Surface> &surface = texture->surfaces[face * texture->desc.levels + level];
    if (!surface)
    {
        surface.reset(new Surface());
        surface->texture = texture;
        surface->face = face;
        surface->level = level;
    }
    return surface.get();
}

namespace
{
Surface *Standalone(Device &device, uint32_t width, uint32_t height, d3d::Format format, uint32_t usage,
                    d3d::Pool pool, const char *name)
{
    if (!Usable(device))
        return nullptr;
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.levels = 1;
    desc.usage = usage;
    desc.format = format;
    desc.pool = pool;
    desc.name = name;
    Texture *texture = CreateTextureInternal(device, desc, 0);
    if (!texture)
        return nullptr;
    Surface *surface = GetSurfaceLevel(&device, texture, 0, 0);
    surface->standalone = true;
    return surface;
}
} // namespace

Surface *CreateRenderTarget(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name)
{
    return Standalone(*device, width, height, format, d3d::USAGE_RENDERTARGET, d3d::POOL_DEFAULT,
                      name ? name : "legacy render target");
}

Surface *CreateDepthStencil(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name)
{
    return Standalone(*device, width, height, format, d3d::USAGE_DEPTHSTENCIL, d3d::POOL_DEFAULT,
                      name ? name : "legacy depth surface");
}

Surface *CreatePlainSurface(Device *device, uint32_t width, uint32_t height, d3d::Format format, d3d::Pool pool,
                            const char *name)
{
    return Standalone(*device, width, height, format, 0, pool, name ? name : "legacy plain surface");
}

void DestroySurface(Device *device, Surface *surface)
{
    if (!surface || !surface->standalone)
        return;
    DestroyTexture(device, surface->texture);
}

void SurfaceSize(const Surface *surface, uint32_t &width, uint32_t &height, d3d::Format &format)
{
    const TextureDesc &desc = surface->texture->desc;
    width = LevelSize(desc.width, surface->level);
    height = LevelSize(desc.height, surface->level);
    format = desc.format;
}

bool LockSurface(Device *device, Surface *surface, const Box *box, uint32_t flags, LockedRect &out)
{
    out = LockedRect{};
    return surface && LockRect(device, surface->texture, surface->face, surface->level, box, flags, out);
}

void UnlockSurface(Device *device, Surface *surface)
{
    if (surface)
        UnlockRect(device, surface->texture, surface->face, surface->level);
}

// ---- Buffers -----------------------------------------------------------------------------

VertexBuffer *CreateVertexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, const char *name)
{
    return CreateGpuBuffer<VertexBuffer>(*device, bytes, usage, pool, false, d3d::FMT_UNKNOWN, name);
}

void DestroyVertexBuffer(Device *device, VertexBuffer *buffer)
{
    if (!buffer)
        return;
    for (StreamBinding &stream : device->streams)
    {
        if (stream.buffer == buffer)
            stream = StreamBinding{};
    }
    DestroyGpuBuffer(*device, buffer);
    delete buffer;
}

IndexBuffer *CreateIndexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Format format, d3d::Pool pool,
                               const char *name)
{
    if (format != d3d::FMT_INDEX16 && format != d3d::FMT_INDEX32)
    {
        Log(*device, "index buffer %s: format %u is not INDEX16 or INDEX32", name ? name : "", format);
        return nullptr;
    }
    return CreateGpuBuffer<IndexBuffer>(*device, bytes, usage, pool, true, format, name);
}

void DestroyIndexBuffer(Device *device, IndexBuffer *buffer)
{
    if (!buffer)
        return;
    if (device->indices == buffer)
        device->indices = nullptr;
    DestroyGpuBuffer(*device, buffer);
    delete buffer;
}

void *Lock(Device *device, VertexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    return LockGpuBuffer(*device, buffer, offset, bytes, flags);
}
void Unlock(Device *device, VertexBuffer *buffer) { UnlockGpuBuffer(*device, buffer); }
void *Lock(Device *device, IndexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags)
{
    return LockGpuBuffer(*device, buffer, offset, bytes, flags);
}
void Unlock(Device *device, IndexBuffer *buffer) { UnlockGpuBuffer(*device, buffer); }

VkBuffer DrawBuffer(Device &device, GpuBuffer &buffer)
{
    if (!buffer.dynamic)
        return BufferHandle(buffer.device);
    Backing &backing = buffer.backings[buffer.current];
    backing.lastUse = SubmissionSerial(device.core);
    return BufferHandle(backing.buffer);
}

// ---- Shaders and declarations -------------------------------------------------------------

Shader *CreateShader(Device *device_, const void *bytecode, size_t bytes, std::string &error)
{
    Device &device = *device_;
    error.clear();
    if (!Usable(device))
    {
        error = device.error;
        return nullptr;
    }
    const TimeInto timer(device.stats.shaderMicros);
    ++device.stats.shaders;
    std::unique_ptr<Shader> shader(new Shader());
    if (!ls::Translate(bytecode, bytes, shader->translated, error))
    {
        Log(device, "shader translation failed: %s", error.c_str());
        return nullptr;
    }
    shader->id = device.nextId++;
    shader->stage = shader->translated.reflection.stage;
    if (shader->stage == ls::Stage::Vertex)
    {
        std::vector<uint32_t> spirv;
        // Vertex textures are never depth-compared here; where the device
        // fixes depth compare in the shader, say so (see fixedDepthCompare).
        const uint32_t noDepthSamplers = 0;
        if (!ls::BuildSpirv(shader->translated, nullptr, spirv, error,
                            Features(device.core).fixedDepthCompare ? &noDepthSamplers : nullptr))
        {
            Log(device, "vertex shader %08x: %s", shader->translated.bytecodeHash, error.c_str());
            return nullptr;
        }
        shader->module = CreateShaderModule(device.core, spirv.data(), spirv.size() * sizeof(uint32_t));
        if (!shader->module)
        {
            error = LastError(device.core);
            Fail(device, "vertex shader %08x: %s", shader->translated.bytecodeHash, error.c_str());
            return nullptr;
        }
    }
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
    ForgetPipelines(*device, shader->id);
    DestroyShaderModule(device->core, shader->module);
    for (const auto &entry : shader->linked)
        DestroyShaderModule(device->core, entry.second);
    delete shader;
}

VertexDecl *CreateVertexDecl(Device *device, const VertexElement *elements, uint32_t count)
{
    if (!Usable(*device))
        return nullptr;
    for (uint32_t i = 0; i < count; ++i)
    {
        const VertexElement &element = elements[i];
        if (element.stream >= kMaxStreams || element.type >= d3d::DECLTYPE_UNUSED || element.method != 0)
        {
            Log(*device, "vertex declaration element %u: stream %u type %u method %u not supported", i,
                element.stream, element.type, element.method);
            return nullptr;
        }
    }
    VertexDecl *decl = new VertexDecl();
    decl->id = device->nextId++;
    decl->elements.assign(elements, elements + count);
    return decl;
}

void DestroyVertexDecl(Device *device, VertexDecl *decl)
{
    if (!decl)
        return;
    if (device->decl == decl)
        device->decl = nullptr;
    ForgetPipelines(*device, decl->id);
    delete decl;
}
} // namespace gfx_vk::legacy
