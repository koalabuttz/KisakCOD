#pragma once

// GPU memory for the PS Vita GXM back end.
//
// The GPU reads and writes only memory the process has mapped for it, and its
// shader code (USSE) lives in memory mapped separately for each shader unit.
// At start-up the back end reserves one kernel memory block per kind below,
// maps it, and then hands out pieces with a Heap (gxm_heap.h):
//
//   Cdram         the GPU's own 128 MB (about 112 MB for a game): textures,
//                 render targets, the display buffers, depth-stencil.
//   Main          uncached main memory the GPU reads and writes: vertex and
//                 index buffers, constants, the context's ring buffers. It
//                 comes out of the same 256 MB as the engine's heap and zones.
//   VertexUsse    vertex shader code, mapped for the vertex units.
//   FragmentUsse  fragment shader code, mapped for the fragment units.
//
// Allocate and Free may be called from any thread.

#include <cstdint>
#include <string>

namespace gfx_gxm
{
enum class MemoryKind : uint8_t
{
    Cdram,
    Main,
    VertexUsse,
    FragmentUsse,
    Count,
};
const char *MemoryKindName(MemoryKind kind);

struct MemorySizes
{
    // Bytes to reserve per kind; rounded up to the kernel's granule (256 KB
    // for CDRAM, 4 KB otherwise). 0 reserves nothing for that kind.
    uint32_t cdram = 0;
    uint32_t main = 0;
    uint32_t vertexUsse = 0;
    uint32_t fragmentUsse = 0;
};

// Reserves and maps the blocks. sceGxmInitialize must have run. False with
// the reason when a block cannot be had; whatever was reserved is released.
bool InitMemory(const MemorySizes &sizes, std::string &error);
void ShutdownMemory();

// Null when the pool is full or not reserved. align is a power of two.
void *Allocate(MemoryKind kind, uint32_t size, uint32_t align);
// Null is ignored; a pointer from another pool is logged by the caller's
// check (false) and not freed.
bool Free(MemoryKind kind, void *memory);
// A USSE pointer's offset, as the shader patcher wants it.
uint32_t UsseOffset(MemoryKind kind, const void *memory);

struct PoolStats
{
    uint32_t size;
    uint32_t used;
    uint32_t peak;
    uint32_t largestFree;
    uint32_t allocations;
};
PoolStats Stats(MemoryKind kind);
// One line: every pool's use, in MB or KB.
std::string DescribeMemory();
} // namespace gfx_gxm
