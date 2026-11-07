// mmap and friends over Vita memory blocks; see vita_mman.h for the rules.

#include "vita_mman.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

namespace
{
    // Memory blocks come in 4 KiB pages for the USER_RW type.
    constexpr size_t kVitaPageSize = 0x1000;
    // The dedicated server makes a handful of long-lived reservations (the
    // physical memory arena, the hunk, the debug hunk) plus short-lived
    // directory-listing hunks. The kernel caps a process's block count too.
    constexpr int kMaxMappings = 128;

    struct Mapping
    {
        SceUID block;
        uint8_t *base;
        size_t length;
    };

    Mapping s_mappings[kMaxMappings];
    int s_mappingCount;
    pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

    // The mapping that holds [address, address + length), or null.
    Mapping *FindMapping(const void *address, size_t length)
    {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
        for (int i = 0; i < s_mappingCount; ++i)
        {
            Mapping &mapping = s_mappings[i];
            const uintptr_t base = reinterpret_cast<uintptr_t>(mapping.base);
            if (begin >= base && begin - base <= mapping.length && length <= mapping.length - (begin - base))
                return &mapping;
        }
        return nullptr;
    }
}

extern "C" void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
    (void)protection;
    if (address || fd != -1 || offset != 0 || (flags & MAP_FIXED) || !(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE)
        || length == 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (length > SIZE_MAX - (kVitaPageSize - 1))
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    const size_t rounded = (length + kVitaPageSize - 1) & ~(kVitaPageSize - 1);

    pthread_mutex_lock(&s_lock);
    if (s_mappingCount == kMaxMappings)
    {
        pthread_mutex_unlock(&s_lock);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    const SceUID block = sceKernelAllocMemBlock("KisakCOD mmap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, nullptr);
    void *base = nullptr;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 || !base)
    {
        if (block >= 0)
            sceKernelFreeMemBlock(block);
        pthread_mutex_unlock(&s_lock);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    // Anonymous mappings read as zero, as on Linux and as a Windows commit.
    memset(base, 0, rounded);
    s_mappings[s_mappingCount++] = Mapping{ block, static_cast<uint8_t *>(base), rounded };
    pthread_mutex_unlock(&s_lock);
    return base;
}

extern "C" int munmap(void *address, size_t length)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_mappingCount; ++i)
    {
        Mapping &mapping = s_mappings[i];
        if (mapping.base != address)
            continue;
        const size_t rounded = (length + kVitaPageSize - 1) & ~(kVitaPageSize - 1);
        if (rounded != mapping.length)
            break; // part of a block cannot be returned
        const int freed = sceKernelFreeMemBlock(mapping.block);
        s_mappings[i] = s_mappings[--s_mappingCount];
        pthread_mutex_unlock(&s_lock);
        if (freed < 0)
        {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    pthread_mutex_unlock(&s_lock);
    errno = EINVAL;
    return -1;
}

extern "C" int mprotect(void *address, size_t length, int protection)
{
    (void)protection;
    pthread_mutex_lock(&s_lock);
    const bool inside = FindMapping(address, length) != nullptr;
    pthread_mutex_unlock(&s_lock);
    if (!inside)
    {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

extern "C" int madvise(void *address, size_t length, int advice)
{
    pthread_mutex_lock(&s_lock);
    const bool inside = FindMapping(address, length) != nullptr;
    pthread_mutex_unlock(&s_lock);
    if (!inside)
    {
        errno = ENOMEM;
        return -1;
    }
    if (advice == MADV_DONTNEED)
        memset(address, 0, length);
    return 0;
}

extern "C" unsigned int Vita_MmapCount()
{
    pthread_mutex_lock(&s_lock);
    const unsigned int count = static_cast<unsigned int>(s_mappingCount);
    pthread_mutex_unlock(&s_lock);
    return count;
}

extern "C" size_t Vita_MmapBytes()
{
    pthread_mutex_lock(&s_lock);
    size_t total = 0;
    for (int i = 0; i < s_mappingCount; ++i)
        total += s_mappings[i].length;
    pthread_mutex_unlock(&s_lock);
    return total;
}
