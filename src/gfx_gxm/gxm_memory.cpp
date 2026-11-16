#include "gxm_memory.h"

#include "gxm_heap.h"

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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
bool g_mainCached = false;

Pool &PoolFor(MemoryKind kind)
{
    return g_pools[static_cast<int>(kind)];
}

uint32_t RoundUp(uint32_t value, uint32_t granule)
{
    return (value + granule - 1) & ~(granule - 1);
}

bool Reserve(MemoryKind kind, uint32_t bytes, bool cached, std::string &error)
{
    if (!bytes)
        return true;
    const bool cdram = kind == MemoryKind::Cdram;
    uint32_t size = RoundUp(bytes, cdram ? 256 * 1024 : 4096);
    const SceKernelMemBlockType type = cdram    ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW
                                       : cached ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RW
                                                : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
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
        const bool cached = static_cast<MemoryKind>(i) == MemoryKind::Main && sizes.mainCached;
        if (!Reserve(static_cast<MemoryKind>(i), bytes[i], cached, error))
        {
            for (int j = 0; j < i; ++j)
                Release(static_cast<MemoryKind>(j));
            return false;
        }
    }
    g_mainCached = sizes.mainCached && sizes.main;
    return true;
}

void ShutdownMemory()
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < static_cast<int>(MemoryKind::Count); ++i)
        Release(static_cast<MemoryKind>(i));
}

void *Allocate(MemoryKind kind, uint32_t size, uint32_t align, bool fromTop)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    Pool &pool = PoolFor(kind);
    if (!pool.base)
        return nullptr;
    const uint32_t offset = pool.heap.Allocate(size, align, fromTop);
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
            std::snprintf(part, sizeof(part), " %s%s %.1f of %.1f MB (%u blocks, peak %.1f, largest free %.1f)",
                          MemoryKindName(kind), kind == MemoryKind::Main && g_mainCached ? " (cached)" : "",
                          stats.used / 1048576.0, stats.size / 1048576.0, stats.allocations, stats.peak / 1048576.0,
                          stats.largestFree / 1048576.0);
        else
            std::snprintf(part, sizeof(part), " %s %u of %u KB (%u blocks)", MemoryKindName(kind), stats.used >> 10,
                          stats.size >> 10, stats.allocations);
        out += part;
    }
    return out;
}

std::string DescribeFreeRanges()
{
    std::string out = "gpu memory free ranges, largest first (MB):";
    for (const MemoryKind kind : {MemoryKind::Cdram, MemoryKind::Main})
    {
        std::vector<uint32_t> ranges;
        uint32_t count = 0;
        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            const Pool &pool = PoolFor(kind);
            if (!pool.size)
                continue;
            ranges = pool.heap.LargestFreeRanges(6);
            count = pool.heap.FreeRanges();
        }
        char part[64];
        std::snprintf(part, sizeof(part), " %s %u ranges:", MemoryKindName(kind), count);
        out += part;
        for (const uint32_t bytes : ranges)
        {
            std::snprintf(part, sizeof(part), " %.2f", bytes / 1048576.0);
            out += part;
        }
        out += ";";
    }
    return out;
}

namespace
{
const uint32_t kSpeedBytes = 256u << 10;
const int kSpeedPasses = 4;

// The shortest of a few passes, in microseconds: a pass an interrupt or
// another thread broke into says nothing about the memory.
template <typename Pass> uint32_t ShortestPass(Pass pass)
{
    uint32_t best = ~0u;
    for (int i = 0; i < kSpeedPasses; ++i)
    {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        pass();
        const uint32_t took = static_cast<uint32_t>(sceKernelGetProcessTimeWide() - start);
        if (took < best)
            best = took;
    }
    return best ? best : 1;
}

uint32_t CopyMicros(void *to, const void *from)
{
    return ShortestPass([to, from] { std::memcpy(to, from, kSpeedBytes); });
}

uint32_t WordWriteMicros(void *memory)
{
    return ShortestPass([memory] {
        volatile uint32_t *words = static_cast<volatile uint32_t *>(memory);
        for (uint32_t i = 0; i < kSpeedBytes / 4; ++i)
            words[i] = i;
    });
}

uint32_t WordReadMicros(const void *memory, uint32_t *sum)
{
    return ShortestPass([memory, sum] {
        const volatile uint32_t *words = static_cast<const volatile uint32_t *>(memory);
        uint32_t total = 0;
        for (uint32_t i = 0; i < kSpeedBytes / 4; ++i)
            total += words[i];
        *sum += total;
    });
}

double MegabytesASecond(uint32_t micros)
{
    return kSpeedBytes / 1048576.0 * 1e6 / micros;
}

double NanosAWord(uint32_t micros)
{
    return micros * 1000.0 / (kSpeedBytes / 4);
}

void DescribeSpeedOf(const char *name, void *memory, void *heap, std::string &out)
{
    uint32_t sum = 0;
    const uint32_t in = CopyMicros(memory, heap);
    const uint32_t back = CopyMicros(heap, memory);
    const uint32_t write = WordWriteMicros(memory);
    const uint32_t read = WordReadMicros(memory, &sum);
    char part[192];
    std::snprintf(part, sizeof(part), "%s %s: copy in %.0f MB/s, copy out %.0f MB/s, a word written %.1f ns, read %.1f ns",
                  out.back() == ':' ? "" : ";", name, MegabytesASecond(in), MegabytesASecond(back),
                  NanosAWord(write), NanosAWord(read));
    out += part;
}
} // namespace

std::string DescribeMemorySpeed()
{
    char *heapA = static_cast<char *>(std::malloc(kSpeedBytes));
    char *heapB = static_cast<char *>(std::malloc(kSpeedBytes));
    if (!heapA || !heapB)
    {
        std::free(heapA);
        std::free(heapB);
        return "gpu memory speed: no heap for the measurement";
    }
    std::memset(heapA, 0x5a, kSpeedBytes);
    std::memset(heapB, 0, kSpeedBytes);
    char head[64];
    std::snprintf(head, sizeof(head), "gpu memory speed, %u KB at a time:", kSpeedBytes >> 10);
    std::string out = head;
    DescribeSpeedOf("the heap", heapB, heapA, out);
    for (const MemoryKind kind : {MemoryKind::Main, MemoryKind::Cdram})
    {
        void *memory = Allocate(kind, kSpeedBytes, 64);
        if (!memory)
            continue;
        const std::string name =
            std::string(MemoryKindName(kind)) + (kind == MemoryKind::Main && g_mainCached ? " (cached)" : "");
        DescribeSpeedOf(name.c_str(), memory, heapA, out);
        Free(kind, memory);
    }
    std::free(heapA);
    std::free(heapB);
    return out;
}
} // namespace gfx_gxm
