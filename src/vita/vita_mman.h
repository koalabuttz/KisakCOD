#pragma once

// The subset of <sys/mman.h> the POSIX platform layer (src/linux) uses, for
// the PS Vita, whose newlib has no such header.
//
// The Vita gives applications no virtual memory API: memory is allocated in
// whole blocks (sceKernelAllocMemBlock) that are readable and writable from
// the start. So:
//   * mmap allocates a zeroed block at once, whatever the protection, and
//     only for anonymous private mappings without MAP_FIXED.
//   * mprotect checks that the range lies inside a mapping and changes
//     nothing. PROT_NONE pages stay readable, so the engine's "fault before
//     commit" guarantee does not hold here (the runtime self-tests that check
//     it are not built for the Vita).
//   * madvise(MADV_DONTNEED) zeroes the range, which is what a later access
//     sees after a Linux decommit.
//   * munmap frees a whole mapping; unmapping part of one fails with EINVAL.
// A reserve therefore costs its full size in RAM at once. See docs/vita.md for
// the reservations the dedicated server makes.

#if !defined(__vita__)
#error "vita/vita_mman.h is for the PS Vita build only"
#endif

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000

#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL 0
#define MADV_DONTNEED 4

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int madvise(void *address, size_t length, int advice);

// Number of live mappings and their total size, for diagnostics.
unsigned int Vita_MmapCount();
size_t Vita_MmapBytes();

#ifdef __cplusplus
}
#endif
