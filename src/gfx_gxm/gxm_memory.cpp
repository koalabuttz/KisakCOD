#include "gxm_memory.h"

#include "gxm_heap.h"

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <cstdio>
#include <mutex>

namespace gfx_gxm
{
namespace
{
struct Pool
{
    SceUID block = -1;
    uint8_t *base = nullptr;
    uint32_t size = 0;
    uint32_t usseOffset = 0; // the base's offset, for the USSE pools
    Heap heap;
};

std::mutex g_mutex;
Pool g_pools[static_cast<int>(MemoryKind::Count)];

Pool &PoolFor(MemoryKind kind)
{
    return g_pools[static_cast<int>(kind)];
}

uint32_t RoundUp(uint32_t value, uint32_t granule)
{
    return (value + granule - 1) & ~(granule - 1);
}

bool Reserve(MemoryKind kind, uint32_t bytes, std::string &error)
{
    if (!bytes)
        return true;
    const bool cdram = kind == MemoryKind::Cdram;
    uint32_t size = RoundUp(bytes, cdram ? 256 * 1024 : 4096);
    const SceKernelMemBlockType type =
        cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    char name[32];
    std::snprintf(name, sizeof(name), "kisak_gxm_%s", MemoryKindName(kind));
    SceUID block = sceKernelAllocMemBlock(name, type, size, nullptr);
    // CDRAM is asked for nearly whole; what the system holds back is not
    // reported, so step down 4 MB at a time to what it gives, keeping half.
    const uint32_t floor = size / 2;
    while (block < 0 && cdram && size > floor + (4u << 20))
    {
        size -= 4u << 20;
        block = sceKernelAllocMemBlock(name, type, size, nullptr);
    }
    if (block < 0)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "cannot reserve %u KB of %s memory (0x%08x)", size >> 10,
                      MemoryKindName(kind), static_cast<unsigned>(block));
        error = line;
        return false;
    }
    void *base = nullptr;
    sceKernelGetMemBlockBase(block, &base);

    int result = 0;
    uint32_t usseOffset = 0;
    switch (kind)
    {
    case MemoryKind::VertexUsse:
        result = sceGxmMapVertexUsseMemory(base, size, &usseOffset);
        break;
    case MemoryKind::FragmentUsse:
        result = sceGxmMapFragmentUsseMemory(base, size, &usseOffset);
        break;
    default:
        result = sceGxmMapMemory(base, size,
                                 static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ |
                                                                      SCE_GXM_MEMORY_ATTRIB_WRITE));
        break;
    }
    if (result < 0)
    {
        sceKernelFreeMemBlock(block);
        char line[160];
        std::snprintf(line, sizeof(line), "cannot map %u KB of %s memory for the GPU (0x%08x)", size >> 10,
                      MemoryKindName(kind), static_cast<unsigned>(result));
        error = line;
        return false;
    }
    Pool &pool = PoolFor(kind);
    pool.block = block;
    pool.base = static_cast<uint8_t *>(base);
    pool.size = size;
    pool.usseOffset = usseOffset;
    pool.heap.Reset(size);
    return true;
}

void Release(MemoryKind kind)
{
    Pool &pool = PoolFor(kind);
    if (pool.block < 0)
        return;
    switch (kind)
    {
    case MemoryKind::VertexUsse:
        sceGxmUnmapVertexUsseMemory(pool.base);
        break;
    case MemoryKind::FragmentUsse:
        sceGxmUnmapFragmentUsseMemory(pool.base);
        break;
    default:
        sceGxmUnmapMemory(pool.base);
        break;
    }
    sceKernelFreeMemBlock(pool.block);
    pool = Pool{};
}
} // namespace

const char *MemoryKindName(MemoryKind kind)
{
    switch (kind)
    {
    case MemoryKind::Cdram:
        return "cdram";
    case MemoryKind::Main:
        return "main";
    case MemoryKind::VertexUsse:
        return "vertex_usse";
    case MemoryKind::FragmentUsse:
        return "fragment_usse";
    default:
        return "?";
    }
}

bool InitMemory(const MemorySizes &sizes, std::string &error)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t bytes[] = {sizes.cdram, sizes.main, sizes.vertexUsse, sizes.fragmentUsse};
    for (int i = 0; i < static_cast<int>(MemoryKind::Count); ++i)
    {
        if (!Reserve(static_cast<MemoryKind>(i), bytes[i], error))
        {
            for (int j = 0; j < i; ++j)
                Release(static_cast<MemoryKind>(j));
            return false;
        }
    }
    return true;
}

void ShutdownMemory()
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < static_cast<int>(MemoryKind::Count); ++i)
        Release(static_cast<MemoryKind>(i));
}

void *Allocate(MemoryKind kind, uint32_t size, uint32_t align)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    Pool &pool = PoolFor(kind);
    if (!pool.base)
        return nullptr;
    const uint32_t offset = pool.heap.Allocate(size, align);
    return offset == Heap::kNoSpace ? nullptr : pool.base + offset;
}

bool Free(MemoryKind kind, void *memory)
{
    if (!memory)
        return true;
    const std::lock_guard<std::mutex> lock(g_mutex);
    Pool &pool = PoolFor(kind);
    const uint8_t *bytes = static_cast<const uint8_t *>(memory);
    if (!pool.base || bytes < pool.base || bytes >= pool.base + pool.size)
        return false;
    return pool.heap.Free(static_cast<uint32_t>(bytes - pool.base));
}

uint32_t UsseOffset(MemoryKind kind, const void *memory)
{
    const Pool &pool = PoolFor(kind);
    return pool.usseOffset + static_cast<uint32_t>(static_cast<const uint8_t *>(memory) - pool.base);
}

PoolStats Stats(MemoryKind kind)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    const Pool &pool = PoolFor(kind);
    return PoolStats{pool.size, pool.heap.Used(), pool.heap.Peak(), pool.heap.LargestFree(),
                     pool.heap.Allocations()};
}

std::string DescribeMemory()
{
    std::string out = "gpu memory:";
    for (int i = 0; i < static_cast<int>(MemoryKind::Count); ++i)
    {
        const MemoryKind kind = static_cast<MemoryKind>(i);
        const PoolStats stats = Stats(kind);
        if (!stats.size)
            continue;
        char part[128];
        if (stats.size >= (4u << 20))
            std::snprintf(part, sizeof(part), " %s %.1f of %.1f MB (%u blocks, peak %.1f)", MemoryKindName(kind),
                          stats.used / 1048576.0, stats.size / 1048576.0, stats.allocations, stats.peak / 1048576.0);
        else
            std::snprintf(part, sizeof(part), " %s %u of %u KB (%u blocks)", MemoryKindName(kind), stats.used >> 10,
                          stats.size >> 10, stats.allocations);
        out += part;
    }
    return out;
}
} // namespace gfx_gxm
