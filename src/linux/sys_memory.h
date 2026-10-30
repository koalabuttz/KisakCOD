#pragma once
#include <cstdint>

int Sys_SystemMemoryMB();

// ---------------------------------------------------------------------------
// Native physical-memory arena.
//
// Replaces universal/physicalmemory.cpp. The database allocates its zones out of
// a single contiguous arena and depends on the addresses being stable and
// page-aligned, so the arena is created by reserving address space with
// PROT_NONE and committing it page by page as allocations advance. Nothing is
// physically backed until it is actually used, but every byte the allocator
// hands out is guaranteed writable.
// ---------------------------------------------------------------------------

// Releases the arena. Called during shutdown; safe when never initialised.
void Posix_ReleasePhysicalMemory();

// Bytes reserved for the arena (128 MB, matching the Windows build).
uint32_t Posix_PhysicalMemorySize();

// Reports the arena's base address, or NULL when not initialised.
uint8_t *Posix_PhysicalMemoryBase();
