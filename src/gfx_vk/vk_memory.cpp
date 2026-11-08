// gfx_vk memory: pooled device memory, buffers, images, and destruction
// deferred until the GPU has finished with them. See vk_core.h.

#include "vk_core_internal.h"

#include <algorithm>
#include <cstdio>

namespace gfx_vk
{
namespace
{
// Block sizes: big enough that a map's textures need a few dozen blocks, small
// enough that a half-empty block wastes little. Larger requests get their own
// (dedicated) allocation.
constexpr VkDeviceSize kDeviceBlockSize = 64ull << 20;
constexpr VkDeviceSize kHostBlockSize = 16ull << 20;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

// The memory type for a request: the wanted flags, then fallbacks.
uint32_t FindMemoryType(const Core &core, uint32_t typeBits, MemoryKind kind)
{
    std::vector<VkMemoryPropertyFlags> wanted;
    switch (kind)
    {
    case MemoryKind::Device:
        wanted = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
        break;
    case MemoryKind::Upload:
        // Device-local and host-visible (resizable BAR, unified memory) first.
        wanted = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                      | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT};
        break;
    case MemoryKind::Readback:
        wanted = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT};
        break;
    }
    for (VkMemoryPropertyFlags flags : wanted)
    {
        for (uint32_t i = 0; i < core.memory.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) && (core.memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
    }
    return UINT32_MAX;
}

bool IsHostVisible(const Core &core, uint32_t type)
{
    return (core.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

bool IsCoherent(const Core &core, uint32_t type)
{
    return (core.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

bool NewBlock(Core &core, MemoryPool &pool, VkDeviceSize size, bool dedicated, uint32_t &blockIndex)
{
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = size;
    info.memoryTypeIndex = pool.memoryType;
    MemoryBlock block;
    block.size = size;
    block.dedicated = dedicated;
    if (!Check(core, core.vk.vkAllocateMemory(core.dev, &info, nullptr, &block.memory), "vkAllocateMemory"))
        return false;
    if (pool.host
        && !Check(core, core.vk.vkMapMemory(core.dev, block.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped), "vkMapMemory"))
    {
        core.vk.vkFreeMemory(core.dev, block.memory, nullptr);
        return false;
    }
    block.freeRanges[0] = size;
    if (pool.host)
        core.stats.hostMemoryBytes += size;
    else
        core.stats.deviceMemoryBytes += size;
    ++core.stats.memoryBlocks;

    // Reuse an empty slot left by a freed dedicated block.
    for (uint32_t i = 0; i < pool.blocks.size(); ++i)
    {
        if (!pool.blocks[i].memory)
        {
            pool.blocks[i] = std::move(block);
            blockIndex = i;
            return true;
        }
    }
    pool.blocks.push_back(std::move(block));
    blockIndex = static_cast<uint32_t>(pool.blocks.size() - 1);
    return true;
}

// First fit in a block's free list; splits the range it takes.
bool TakeRange(MemoryBlock &block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize &offset)
{
    for (auto it = block.freeRanges.begin(); it != block.freeRanges.end(); ++it)
    {
        const VkDeviceSize start = AlignUp(it->first, alignment);
        const VkDeviceSize end = it->first + it->second;
        if (start + size > end)
            continue;
        const VkDeviceSize rangeStart = it->first;
        block.freeRanges.erase(it);
        if (start > rangeStart)
            block.freeRanges[rangeStart] = start - rangeStart;
        if (start + size < end)
            block.freeRanges[start + size] = end - (start + size);
        offset = start;
        block.used += size;
        return true;
    }
    return false;
}

void ReturnRange(MemoryBlock &block, VkDeviceSize offset, VkDeviceSize size)
{
    block.used -= size;
    auto next = block.freeRanges.lower_bound(offset);
    VkDeviceSize start = offset;
    VkDeviceSize length = size;
    if (next != block.freeRanges.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset)
        {
            start = previous->first;
            length += previous->second;
            block.freeRanges.erase(previous);
        }
    }
    if (next != block.freeRanges.end() && next->first == offset + size)
    {
        length += next->second;
        block.freeRanges.erase(next);
    }
    block.freeRanges[start] = length;
}

void ReleaseBlock(Core &core, MemoryPool &pool, MemoryBlock &block)
{
    if (block.mapped)
        core.vk.vkUnmapMemory(core.dev, block.memory);
    core.vk.vkFreeMemory(core.dev, block.memory, nullptr);
    if (pool.host)
        core.stats.hostMemoryBytes -= block.size;
    else
        core.stats.deviceMemoryBytes -= block.size;
    --core.stats.memoryBlocks;
    block = MemoryBlock();
}

VkImageType ImageType(ImageKind kind)
{
    return kind == ImageKind::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}

VkImageViewType ViewType(ImageKind kind)
{
    switch (kind)
    {
    case ImageKind::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case ImageKind::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
} // namespace

uint32_t FormatBlockBytes(VkFormat format, uint32_t *blockWidth, uint32_t *blockHeight)
{
    uint32_t w = 1;
    uint32_t h = 1;
    uint32_t bytes = 0;
    switch (format)
    {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
        w = h = 4;
        bytes = 8;
        break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
        w = h = 4;
        bytes = 16;
        break;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
        bytes = 1;
        break;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_D16_UNORM:
        bytes = 2;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        bytes = 4;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        bytes = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        bytes = 16;
        break;
    default:
        bytes = 0;
        break;
    }
    if (blockWidth)
        *blockWidth = w;
    if (blockHeight)
        *blockHeight = h;
    return bytes;
}

VkImageAspectFlags FormatAspect(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool Allocate(Core &core, const VkMemoryRequirements &requirements, MemoryKind kind, bool image, Allocation &out)
{
    const uint32_t type = FindMemoryType(core, requirements.memoryTypeBits, kind);
    if (type == UINT32_MAX)
        return Fail(core, "no memory type fits the request (type bits %08x)", requirements.memoryTypeBits);
    // Only Upload and Readback memory is mapped, even where every memory type
    // is host-visible (integrated GPUs): mapping textures would spend a 32-bit
    // process's address space for nothing.
    const bool host = kind != MemoryKind::Device;
    if (host && !IsHostVisible(core, type))
        return Fail(core, "no host-visible memory type for a mapped buffer");

    uint32_t poolIndex = UINT32_MAX;
    for (uint32_t i = 0; i < core.memoryPools.size(); ++i)
    {
        const MemoryPool &pool = core.memoryPools[i];
        if (pool.memoryType == type && pool.images == image && pool.host == host)
            poolIndex = i;
    }
    if (poolIndex == UINT32_MAX)
    {
        MemoryPool pool;
        pool.memoryType = type;
        pool.images = image;
        pool.host = host;
        core.memoryPools.push_back(std::move(pool));
        poolIndex = static_cast<uint32_t>(core.memoryPools.size() - 1);
    }
    MemoryPool &pool = core.memoryPools[poolIndex];

    const VkDeviceSize blockSize = host ? kHostBlockSize : kDeviceBlockSize;
    uint32_t blockIndex = UINT32_MAX;
    VkDeviceSize offset = 0;
    if (requirements.size > blockSize / 2)
    {
        if (!NewBlock(core, pool, requirements.size, true, blockIndex))
            return false;
        TakeRange(pool.blocks[blockIndex], requirements.size, 1, offset);
    }
    else
    {
        for (uint32_t i = 0; i < pool.blocks.size() && blockIndex == UINT32_MAX; ++i)
        {
            MemoryBlock &block = pool.blocks[i];
            if (block.memory && !block.dedicated && TakeRange(block, requirements.size, requirements.alignment, offset))
                blockIndex = i;
        }
        if (blockIndex == UINT32_MAX)
        {
            if (!NewBlock(core, pool, blockSize, false, blockIndex))
                return false;
            TakeRange(pool.blocks[blockIndex], requirements.size, requirements.alignment, offset);
        }
    }
    MemoryBlock &block = pool.blocks[blockIndex];
    out.pool = poolIndex;
    out.block = blockIndex;
    out.memory = block.memory;
    out.offset = offset;
    out.size = requirements.size;
    out.mapped = block.mapped ? static_cast<uint8_t *>(block.mapped) + offset : nullptr;
    out.coherent = IsCoherent(core, type);
    return true;
}

void Free(Core &core, Allocation &allocation)
{
    if (allocation.pool == UINT32_MAX)
        return;
    MemoryPool &pool = core.memoryPools[allocation.pool];
    MemoryBlock &block = pool.blocks[allocation.block];
    ReturnRange(block, allocation.offset, allocation.size);
    // Dedicated blocks go back at once; pooled blocks stay for reuse.
    if (block.dedicated)
        ReleaseBlock(core, pool, block);
    allocation = Allocation();
}

void DestroyMemoryPools(Core &core)
{
    for (MemoryPool &pool : core.memoryPools)
    {
        for (MemoryBlock &block : pool.blocks)
        {
            if (block.memory)
                ReleaseBlock(core, pool, block);
        }
    }
    core.memoryPools.clear();
}

void DestroyBufferNow(Core &core, Buffer *buffer)
{
    if (buffer->buffer)
        core.vk.vkDestroyBuffer(core.dev, buffer->buffer, nullptr);
    Free(core, buffer->allocation);
    --core.stats.buffersAlive;
    delete buffer;
}

void DestroyImageNow(Core &core, Image *image)
{
    if (image->sampleView)
        core.vk.vkDestroyImageView(core.dev, image->sampleView, nullptr);
    if (image->view)
        core.vk.vkDestroyImageView(core.dev, image->view, nullptr);
    if (image->image)
        core.vk.vkDestroyImage(core.dev, image->image, nullptr);
    Free(core, image->allocation);
    --core.stats.imagesAlive;
    delete image;
}

void RunDeferred(Core &core, bool everything)
{
    const uint64_t done = everything ? UINT64_MAX : PollCompletedSerial(core);
    size_t kept = 0;
    for (size_t i = 0; i < core.deferred.size(); ++i)
    {
        Deferred &item = core.deferred[i];
        if (item.serial > done)
        {
            core.deferred[kept++] = item;
            continue;
        }
        switch (item.kind)
        {
        case DeferredKind::Buffer:
            DestroyBufferNow(core, static_cast<Buffer *>(item.object));
            break;
        case DeferredKind::Image:
            DestroyImageNow(core, static_cast<Image *>(item.object));
            break;
        case DeferredKind::Pipeline:
            core.vk.vkDestroyPipeline(core.dev, (VkPipeline)item.handle, nullptr);
            break;
        case DeferredKind::ShaderModule:
            core.vk.vkDestroyShaderModule(core.dev, (VkShaderModule)item.handle, nullptr);
            break;
        }
        ++core.stats.deferredDestroyed;
    }
    core.deferred.resize(kept);
}

// ---- Public API -------------------------------------------------------------------------

Buffer *CreateBuffer(Core *core, const BufferDesc &desc)
{
    if (!desc.size)
    {
        Fail(*core, "CreateBuffer: size 0 (%s)", desc.name ? desc.name : "unnamed");
        return nullptr;
    }
    Buffer *buffer = new Buffer();
    buffer->desc = desc;
    buffer->name = desc.name ? desc.name : "buffer";
    buffer->desc.name = nullptr;
    if (desc.memory == MemoryKind::Device)
        buffer->desc.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = buffer->desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(*core, core->vk.vkCreateBuffer(core->dev, &info, nullptr, &buffer->buffer), "vkCreateBuffer"))
    {
        delete buffer;
        return nullptr;
    }
    ++core->stats.buffersAlive;
    VkMemoryRequirements requirements{};
    core->vk.vkGetBufferMemoryRequirements(core->dev, buffer->buffer, &requirements);
    if (!Allocate(*core, requirements, desc.memory, false, buffer->allocation)
        || !Check(*core,
                  core->vk.vkBindBufferMemory(core->dev, buffer->buffer, buffer->allocation.memory,
                                              buffer->allocation.offset),
                  "vkBindBufferMemory"))
    {
        DestroyBufferNow(*core, buffer);
        return nullptr;
    }
    return buffer;
}

void DestroyBuffer(Core *core, Buffer *buffer)
{
    if (buffer)
        core->deferred.push_back({DeferredKind::Buffer, PendingSerial(*core), buffer, 0});
}

VkBuffer BufferHandle(const Buffer *buffer) { return buffer->buffer; }
VkDeviceSize BufferSize(const Buffer *buffer) { return buffer->desc.size; }
void *BufferMapped(const Buffer *buffer) { return buffer->allocation.mapped; }

void FlushMapped(Core *core, Buffer *buffer, VkDeviceSize offset, VkDeviceSize size)
{
    if (buffer->allocation.coherent || !buffer->allocation.mapped)
        return;
    // Ranges must be aligned to nonCoherentAtomSize within the memory object.
    const VkDeviceSize atom = std::max<VkDeviceSize>(core->properties.limits.nonCoherentAtomSize, 1);
    const VkDeviceSize start = (buffer->allocation.offset + offset) / atom * atom;
    VkDeviceSize end = AlignUp(buffer->allocation.offset + offset + size, atom);
    const MemoryBlock &block = core->memoryPools[buffer->allocation.pool].blocks[buffer->allocation.block];
    end = std::min(end, block.size);
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer->allocation.memory;
    range.offset = start;
    range.size = end - start;
    core->vk.vkFlushMappedMemoryRanges(core->dev, 1, &range);
}

Image *CreateImage(Core *core, const ImageDesc &desc)
{
    const char *name = desc.name ? desc.name : "image";
    if (!desc.width || !desc.height || !desc.depth || !desc.mipLevels || desc.format == VK_FORMAT_UNDEFINED)
    {
        Fail(*core, "CreateImage: empty size, no levels or no format (%s)", name);
        return nullptr;
    }
    Image *image = new Image();
    image->desc = desc;
    image->name = name;
    image->desc.name = nullptr;
    if (desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        image->desc.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->layers = desc.kind == ImageKind::Cube ? 6 : 1;
    image->aspect = FormatAspect(desc.format);

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = desc.kind == ImageKind::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = ImageType(desc.kind);
    info.format = desc.format;
    info.extent = {desc.width, desc.height, desc.kind == ImageKind::Tex3D ? desc.depth : 1};
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = image->layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = image->desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(*core, core->vk.vkCreateImage(core->dev, &info, nullptr, &image->image), "vkCreateImage"))
    {
        delete image;
        return nullptr;
    }
    ++core->stats.imagesAlive;
    VkMemoryRequirements requirements{};
    core->vk.vkGetImageMemoryRequirements(core->dev, image->image, &requirements);
    if (!Allocate(*core, requirements, MemoryKind::Device, true, image->allocation)
        || !Check(*core,
                  core->vk.vkBindImageMemory(core->dev, image->image, image->allocation.memory,
                                             image->allocation.offset),
                  "vkBindImageMemory"))
    {
        DestroyImageNow(*core, image);
        return nullptr;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image->image;
    viewInfo.viewType = ViewType(desc.kind);
    viewInfo.format = desc.format;
    // The attachment and copy view: every aspect, identity swizzle.
    viewInfo.subresourceRange = {image->aspect, 0, desc.mipLevels, 0, image->layers};
    if (!Check(*core, core->vk.vkCreateImageView(core->dev, &viewInfo, nullptr, &image->view), "vkCreateImageView"))
    {
        DestroyImageNow(*core, image);
        return nullptr;
    }
    // Shaders sample depth only, through the requested swizzle.
    const VkComponentMapping &swizzle = desc.sampleSwizzle;
    const bool swizzled = swizzle.r != VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.g != VK_COMPONENT_SWIZZLE_IDENTITY
        || swizzle.b != VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.a != VK_COMPONENT_SWIZZLE_IDENTITY;
    const bool depthStencil = (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (image->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if ((desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (swizzled || depthStencil))
    {
        viewInfo.components = swizzle;
        if (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (!Check(*core, core->vk.vkCreateImageView(core->dev, &viewInfo, nullptr, &image->sampleView),
                   "vkCreateImageView"))
        {
            DestroyImageNow(*core, image);
            return nullptr;
        }
    }
    return image;
}

void DestroyImage(Core *core, Image *image)
{
    if (image)
        core->deferred.push_back({DeferredKind::Image, PendingSerial(*core), image, 0});
}

const ImageDesc &ImageInfo(const Image *image) { return image->desc; }
VkImage ImageHandle(const Image *image) { return image->image; }
VkImageView ImageView(const Image *image) { return image->view; }
VkImageView ImageSampleView(const Image *image) { return image->sampleView ? image->sampleView : image->view; }
bool ImageReadyToSample(const Image *image)
{
    return image->layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL && !image->uploadPending;
}
} // namespace gfx_vk
