#include "gxm_heap.h"

#include <iterator>
#include <utility>

namespace gfx_gxm
{
namespace
{
uint64_t AlignUp(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}
} // namespace

Heap::Heap(uint32_t size)
{
    Reset(size);
}

void Heap::Reset(uint32_t size)
{
    free_.clear();
    allocations_.clear();
    size_ = size;
    used_ = 0;
    peak_ = 0;
    if (size)
        free_.emplace(0u, size);
}

uint32_t Heap::Allocate(uint32_t size, uint32_t align)
{
    if (!align || (align & (align - 1)))
        return kNoSpace;
    if (align < kGranule)
        align = kGranule;
    const uint64_t span = AlignUp(size ? size : 1, kGranule);
    for (auto it = free_.begin(); it != free_.end(); ++it)
    {
        const uint64_t start = it->first;
        const uint64_t end = start + it->second;
        const uint64_t offset = AlignUp(start, align);
        if (offset + span > end)
            continue;
        free_.erase(it);
        if (offset > start)
            free_.emplace(static_cast<uint32_t>(start), static_cast<uint32_t>(offset - start));
        if (offset + span < end)
            free_.emplace(static_cast<uint32_t>(offset + span), static_cast<uint32_t>(end - offset - span));
        allocations_.emplace(static_cast<uint32_t>(offset),
                             Allocation{size ? size : 1, static_cast<uint32_t>(span)});
        used_ += static_cast<uint32_t>(span);
        if (used_ > peak_)
            peak_ = used_;
        return static_cast<uint32_t>(offset);
    }
    return kNoSpace;
}

void Heap::AddFree(uint32_t offset, uint32_t size)
{
    uint64_t start = offset;
    uint64_t end = start + size;
    // Merge with the range after, then the one before.
    auto next = free_.lower_bound(offset);
    if (next != free_.end() && next->first == end)
    {
        end += next->second;
        next = free_.erase(next);
    }
    if (next != free_.begin())
    {
        auto prev = std::prev(next);
        if (uint64_t(prev->first) + prev->second == start)
        {
            start = prev->first;
            free_.erase(prev);
        }
    }
    free_.emplace(static_cast<uint32_t>(start), static_cast<uint32_t>(end - start));
}

bool Heap::Free(uint32_t offset)
{
    const auto it = allocations_.find(offset);
    if (it == allocations_.end())
        return false;
    const uint32_t span = it->second.span;
    allocations_.erase(it);
    used_ -= span;
    AddFree(offset, span);
    return true;
}

uint32_t Heap::SizeOf(uint32_t offset) const
{
    const auto it = allocations_.find(offset);
    return it == allocations_.end() ? 0 : it->second.size;
}

uint32_t Heap::LargestFree() const
{
    uint32_t largest = 0;
    for (const auto &range : free_)
        if (range.second > largest)
            largest = range.second;
    return largest;
}

bool Heap::CheckConsistent() const
{
    // Every byte of [0, size) is in exactly one free range or allocation, and
    // no two free ranges touch.
    std::map<uint32_t, std::pair<uint32_t, bool>> pieces; // offset -> (bytes, free)
    for (const auto &range : free_)
    {
        if (!range.second)
            return false;
        pieces.emplace(range.first, std::make_pair(range.second, true));
    }
    uint64_t used = 0;
    for (const auto &allocation : allocations_)
    {
        if (!pieces.emplace(allocation.first, std::make_pair(allocation.second.span, false)).second)
            return false;
        used += allocation.second.span;
    }
    if (used != used_)
        return false;
    uint64_t cursor = 0;
    bool previousFree = false;
    for (const auto &piece : pieces)
    {
        if (piece.first != cursor)
            return false;
        if (piece.second.second && previousFree)
            return false;
        previousFree = piece.second.second;
        cursor += piece.second.first;
    }
    return cursor == size_;
}
} // namespace gfx_gxm
