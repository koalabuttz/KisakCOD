#pragma once

// A first-fit allocator over a range of offsets, for the GPU memory pools of
// the PS Vita GXM back end (gxm_memory.h).
//
// GPU memory on the Vita comes in kernel memory blocks: whole blocks of CDRAM
// (the GPU's own memory) or main memory, mapped for the GPU once. The back end
// reserves a few large blocks at start-up and hands out pieces of them with
// this heap, because a kernel block per texture would cost a 256 KB (CDRAM)
// or 4 KB (main memory) granule each and a kernel call.
//
// The heap only does arithmetic on offsets, so it builds and is tested on any
// host (scripts/tests/test_vita_gxm_heap.py). It is not thread-safe; the pools
// lock around it.

#include <cstdint>
#include <map>
#include <unordered_map>

namespace gfx_gxm
{
class Heap
{
public:
    static constexpr uint32_t kNoSpace = 0xffffffffu;

    // A heap over [0, size); empty by default.
    Heap() = default;
    explicit Heap(uint32_t size);
    void Reset(uint32_t size);

    // The offset of size bytes aligned to align (a power of two), or kNoSpace.
    // The lowest free range that fits wins. Bytes skipped for alignment stay
    // free. A size of 0 is taken as 1.
    uint32_t Allocate(uint32_t size, uint32_t align);
    // Frees an offset Allocate returned; false (and nothing changes) for any
    // other offset.
    bool Free(uint32_t offset);
    // The size asked for at offset, 0 for an offset Allocate did not return.
    uint32_t SizeOf(uint32_t offset) const;

    uint32_t Size() const { return size_; }
    uint32_t Used() const { return used_; }
    uint32_t Allocations() const { return static_cast<uint32_t>(allocations_.size()); }
    uint32_t LargestFree() const;
    uint32_t FreeRanges() const { return static_cast<uint32_t>(free_.size()); }
    // The most bytes ever in use at once.
    uint32_t Peak() const { return peak_; }

    // Walks the free list and the allocations and checks they tile the range
    // without gaps or overlaps. For the tests.
    bool CheckConsistent() const;

private:
    // Free ranges by start offset, never adjacent (they are merged).
    std::map<uint32_t, uint32_t> free_;
    // Spans are whole granules, so the free list never holds slivers.
    static constexpr uint32_t kGranule = 16;
    struct Allocation
    {
        uint32_t size; // what the caller asked for
        uint32_t span; // bytes taken at offset: size rounded up to the granule
    };
    std::unordered_map<uint32_t, Allocation> allocations_;
    uint32_t size_ = 0;
    uint32_t used_ = 0;
    uint32_t peak_ = 0;

    void AddFree(uint32_t offset, uint32_t size);
};
} // namespace gfx_gxm
