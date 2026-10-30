#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <universal/assertive.h>
#include <universal/physicalmemory.h>
#include <universal/profile.h>

#include "sys_memory.h"
#include "sys_posix.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

// Defined by the engine and shared with the database; the layout is fixed by
// <universal/physicalmemory.h>.
PhysicalMemory g_mem;
int g_overAllocatedSize;

namespace
{
    // 128 MB, matching the arena size the Windows build requested.
    constexpr uint32_t kArenaSize = 0x8000000u;

    uint8_t *s_arenaBase = nullptr;
    uint32_t s_pageSize = 4096;
    // Number of bytes from the arena base that have been made writable.
    uint32_t s_committedBytes = 0;

    // Makes [offset, offset + length) writable, growing the committed watermark.
    // Delegates the range checking and page rounding to the tested primitive.
    void EnsureCommitted(uint32_t offset, uint32_t length)
    {
        if (!s_arenaBase)
            return;

        if (!Posix_ArenaCommit(s_arenaBase, kArenaSize, &s_committedBytes, offset, length))
        {
            Com_PrintError(CON_CHANNEL_SYSTEM,
                "physical memory commit failed for offset %u length %u: %s\n",
                offset, length, strerror(errno));
            Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);
        }
    }
}

void Posix_ReleasePhysicalMemory()
{
    if (!s_arenaBase)
        return;

    if (munmap(s_arenaBase, kArenaSize) != 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "munmap of physical memory arena failed: %s\n", strerror(errno));
    }

    s_arenaBase = nullptr;
    s_committedBytes = 0;
    g_mem.buf = nullptr;
}

uint32_t Posix_PhysicalMemorySize()
{
    return kArenaSize;
}

uint8_t *Posix_PhysicalMemoryBase()
{
    return s_arenaBase;
}

void PMem_Init()
{
    if (s_arenaBase)
        return;

    const long pageSize = sysconf(_SC_PAGESIZE);
    s_pageSize = (pageSize > 0) ? (uint32_t)pageSize : 4096u;

    // Reserve the whole arena without backing it. PROT_NONE means any accidental
    // access before commit faults instead of silently zero-filling.
    void *reserved = mmap(nullptr, kArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM,
            "failed to reserve %u bytes of physical memory: %s\n", kArenaSize, strerror(errno));
        Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);
    }

    s_arenaBase = (uint8_t *)reserved;
    s_committedBytes = 0;

    PMem_InitPhysicalMemory(&g_mem, s_arenaBase, kArenaSize);
}

void PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint32_t memorySize)
{
    iassert(pmem != nullptr);
    iassert(memory != nullptr);

    memset((uint8_t *)pmem, 0, sizeof(PhysicalMemory));
    pmem->buf = memory;
    // The high-water mark starts at the end; the low-water mark is 0.
    pmem->prim[1].pos = memorySize;
}

void PMem_BeginAlloc(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);

    PMem_BeginAllocInPrim(&g_mem.prim[allocType], name);
}

void PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    iassert(prim->allocName == nullptr);
    if (prim->allocListCount >= 32)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "PMem_BeginAllocInPrim: allocation list full\n");
        Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);
    }

    prim->allocName = name;
    PhysicalMemoryAllocation *allocEntry = &prim->allocList[prim->allocListCount++];
    allocEntry->name = name;
    allocEntry->pos = prim->pos;
}

void PMem_EndAlloc(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);

    PMem_EndAllocInPrim(&g_mem.prim[allocType], name);
}

void PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    iassert(prim->allocName == name);
    iassert(prim->allocListCount > 0);

    prim->allocName = nullptr;

    // Commit everything this allocation actually consumed.
    EnsureCommitted(0, prim->pos);
}

void PMem_Free(const char *name, uint32_t allocType)
{
    if (allocType >= 2)
        Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);

    PMem_FreeInPrim(&g_mem.prim[allocType], name);
}

void PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    for (uint32_t allocIndex = 0; allocIndex < prim->allocListCount; ++allocIndex)
    {
        if (prim->allocList[allocIndex].name == name)
        {
            PMem_FreeIndex(prim, allocIndex);
            return;
        }
    }
}

void PMem_FreeIndex(PhysicalMemoryPrim *prim, uint32_t allocIndex)
{
    iassert(prim->allocName == nullptr);
    iassert(allocIndex < prim->allocListCount);
    iassert(prim->allocList[allocIndex].name != nullptr);

    prim->allocList[allocIndex].name = nullptr;

    if (allocIndex != prim->allocListCount - 1)
    {
        // Freeing an interior allocation leaves a hole. This arena only ever
        // grows or unwinds from the top, so callers must free in reverse order;
        // report it plainly rather than corrupting the watermark.
        Com_PrintError(CON_CHANNEL_SYSTEM,
            "PMem_FreeIndex: freeing '%s' caused a memory hole\n",
            prim->allocList[allocIndex].name ? prim->allocList[allocIndex].name : "(unknown)");
        return;
    }

    // Unwind from the top, collapsing any previously created holes.
    while (prim->allocListCount > 0 && prim->allocList[prim->allocListCount - 1].name == nullptr)
    {
        prim->pos = prim->allocList[prim->allocListCount - 1].pos;
        --prim->allocListCount;
    }
}

int PMem_GetOverAllocatedSize()
{
    return g_overAllocatedSize;
}

uint32_t PMem_GetFreeAmount()
{
    return g_mem.prim[1].pos - g_mem.prim[0].pos;
}

uint8_t *PMem_Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t allocType)
{
    (void)type; // retained for signature compatibility; not used for placement

    PhysicalMemoryPrim *prim = &g_mem.prim[allocType];
    iassert(prim->allocName != nullptr);
    iassert(size != 0);
    iassert(alignment != 0);

    const uint32_t alignmentMask = alignment - 1;
    uint32_t lowPos;

    if (allocType)
    {
        iassert(allocType == 1); // PHYS_ALLOC_HIGH

        lowPos = ~alignmentMask & (prim->pos - size);
        g_overAllocatedSize = (int)(g_mem.prim[0].pos - lowPos);
        if (g_overAllocatedSize > 0)
            Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);

        EnsureCommitted(lowPos, size);
        g_mem.prim[allocType].pos = lowPos;
    }
    else
    {
        lowPos = ~alignmentMask & (alignmentMask + prim->pos);
        g_overAllocatedSize = (int)(size + lowPos - g_mem.prim[1].pos);
        if (g_overAllocatedSize > 0)
        {
            Com_PrintError(CON_CHANNEL_SYSTEM, "Need %i more bytes of ram for alloc to succeed\n", g_overAllocatedSize);
            Sys_OutOfMemErrorInternal("src/linux/sys_memory.cpp", __LINE__);
        }

        EnsureCommitted(lowPos, size);
        g_mem.prim[allocType].pos = size + lowPos;
    }

    return &g_mem.buf[lowPos];
}

void PMem_DumpMemStats()
{
    auto ConvertToMB = [](uint32_t bytes) { return (double)bytes * (1.0 / 1048576.0); };

    for (uint32_t i = 0; i < g_mem.prim[1].allocListCount; ++i)
    {
        const uint32_t bottom = (i == g_mem.prim[1].allocListCount - 1)
            ? g_mem.prim[1].pos
            : g_mem.prim[1].allocList[i + 1].pos;

        Com_Printf(CON_CHANNEL_SYSTEM, "%-18.18s %5.1f\n",
            g_mem.prim[1].allocList[i].name, ConvertToMB(g_mem.prim[1].allocList[i].pos - bottom));
    }

    Com_Printf(CON_CHANNEL_SYSTEM, "free physical      %5.1f\n", ConvertToMB(PMem_GetFreeAmount()));

    uint32_t top = g_mem.prim[0].pos;
    for (int j = (int)g_mem.prim[0].allocListCount - 1; j >= 0; --j)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "%-18.18s %5.1f\n",
            g_mem.prim[0].allocList[j].name, ConvertToMB(top - g_mem.prim[0].allocList[j].pos));
        top = g_mem.prim[0].allocList[j].pos;
    }

    Com_Printf(CON_CHANNEL_SYSTEM, "------------------------\n");
}

// ---------------------------------------------------------------------------
// System memory reporting and out-of-memory handling.
// ---------------------------------------------------------------------------

int Sys_SystemMemoryMB()
{
    struct sysinfo info;
    if (sysinfo(&info) != 0)
        return 1024;

    const uint64_t totalBytes = (uint64_t)info.totalram * (uint64_t)info.mem_unit;
    uint64_t megabytes = totalBytes / (1024ull * 1024ull);

    // The engine caps its arena planning at 1 GB.
    if (megabytes > 1024)
        megabytes = 1024;
    if (megabytes == 0)
        megabytes = 1;

    return (int)megabytes;
}

void Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    Sys_EnterCriticalSection(CRITSECT_FATAL_ERROR);

    Com_PrintError(CON_CHANNEL_SYSTEM, "Out of memory: filename '%s', line %d\n",
        filename ? filename : "(unknown)", line);

    Sys_SetErrorText("Out of memory");

    // Nothing sane can continue once the arena is exhausted.
    fflush(stdout);
    _exit(-1);
}
