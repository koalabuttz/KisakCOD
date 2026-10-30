#pragma once

// ---------------------------------------------------------------------------
// Win32 virtual-memory compatibility for the native Linux build.
//
// universal/com_memory.cpp drives the engine's address-space allocator through
// VirtualAlloc/VirtualFree directly, so the two calls and the flag constants
// they take have to exist with their Windows meaning rather than being
// reshaped into a POSIX interface:
//
//   Z_VirtualReserve   VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE)
//   Z_VirtualCommit    VirtualAlloc(ptr,  size, MEM_COMMIT,  PAGE_READWRITE)
//   Z_VirtualDecommit  VirtualFree (ptr,  size, MEM_DECOMMIT)
//   Z_VirtualFree      VirtualFree (ptr,  0,    MEM_RELEASE)
//
// Backing model
//   A reservation is a real anonymous private mapping of the exact
//   page-rounded length, created PROT_NONE so that no physical memory is
//   charged until it is committed. Commit changes the protection of pages that
//   already belong to a tracked reservation; it never creates a mapping, and it
//   never asks the kernel to place memory at a caller-chosen address, so no
//   MAP_FIXED is used anywhere and an unrelated mapping can never be
//   overwritten. Decommit discards the pages' contents (madvise MADV_DONTNEED)
//   and returns them to PROT_NONE, so a later commit of the same range yields
//   zeroes, exactly as on Windows. Release unmaps the recorded region.
//
//   Nothing here allocates from the heap on behalf of a caller: VirtualAlloc is
//   not a malloc wrapper. The only heap use is a bookkeeping record per live
//   reservation.
//
// Error reporting
//   Failure returns NULL/FALSE and sets the Win32 error code through
//   SetLastError (see <linux/sys_fileio.h>), which is also what the POSIX
//   backend's file calls use. errno is left reflecting the failing system call
//   so a caller debugging the backend can see the underlying cause. Success
//   leaves the last error untouched, as the Windows functions do.
//
// Include order
//   This header is written the same way as <linux/sys_fileio.h>: it pulls in
//   <linux/compat.h> for the base type set (LPVOID/SIZE_T/DWORD/BOOL) and for
//   SetLastError, so it may be included either directly or from the end of
//   compat.h itself. It must not be included from the middle of compat.h,
//   before that type block.
// ---------------------------------------------------------------------------

#include <linux/compat.h>

#include <cstdint>

// --- Allocation types (VirtualAlloc's dwAllocationType) ---------------------

#ifndef MEM_COMMIT
#define MEM_COMMIT 0x00001000u
#endif
#ifndef MEM_RESERVE
#define MEM_RESERVE 0x00002000u
#endif

// --- Free types (VirtualFree's dwFreeType) ---------------------------------

#ifndef MEM_DECOMMIT
#define MEM_DECOMMIT 0x00004000u
#endif
#ifndef MEM_RELEASE
#define MEM_RELEASE 0x00008000u
#endif

// --- Page protection constants ---------------------------------------------
//
// Only the values the engine passes are meaningful here. PAGE_NOACCESS is
// accepted wherever PAGE_READWRITE is, because a reserve's protection is
// irrelevant until the pages are committed; anything else (the executable and
// read-only variants) is rejected with ERROR_INVALID_PARAMETER rather than
// silently given different access than asked for.

#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x00000001u
#endif
#ifndef PAGE_READONLY
#define PAGE_READONLY 0x00000002u
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x00000004u
#endif

// --- Error codes used by this module ---------------------------------------

#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif
#ifndef ERROR_INVALID_ADDRESS
#define ERROR_INVALID_ADDRESS 487
#endif

// --- Functions -------------------------------------------------------------

// Reserves and/or commits address space.
//
//   address == NULL    size is rounded up to a page and a fresh mapping is
//                      created. MEM_RESERVE alone reserves it inaccessible;
//                      MEM_COMMIT (with or without MEM_RESERVE) additionally
//                      makes it readable and writable.
//   address != NULL    only MEM_COMMIT is accepted, and the range must lie
//                      entirely inside a reservation this module handed out.
//                      Pages are made readable and writable; existing contents
//                      are preserved. Returns `address` on success.
//
// Windows page semantics are kept: the operation covers the whole pages that
// contain the requested bytes (floor of the start, ceiling of the end), so an
// unaligned size or address still commits or frees every page it touches.
// Fails with ERROR_INVALID_ADDRESS when a non-null address cannot be served:
// reserving at a caller-chosen address would require MAP_FIXED, and overriding
// an existing mapping is worse than refusing, so it is refused.
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect);

// Decommits or releases memory.
//
//   MEM_DECOMMIT       size must be non-zero and the range must lie inside a
//                      live reservation. Its pages are zeroed and returned to
//                      the reserved-but-inaccessible state. Decommitting pages
//                      that were already decommitted is not an error; like
//                      Windows, a range may mix committed and decommitted
//                      pages.
//   MEM_RELEASE        size must be 0 and address must be exactly the base of
//                      a live reservation. The whole region is unmapped.
//                      An interior address, an address from an unrelated
//                      mapping, or a base that was already released fails with
//                      ERROR_INVALID_ADDRESS.
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType);

// Number of live reservations. Diagnostic only: the selftest asserts that
// every allocation it makes is released, and a shutdown check can report a
// leak rather than guessing from the process's RSS.
uint32_t Sys_VirtualMemReservationCount();
