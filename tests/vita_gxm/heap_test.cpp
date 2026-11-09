// Host test of the PS Vita GXM back end's offset heap (src/gfx_gxm/gxm_heap.h).
// Built and run by scripts/tests/test_vita_gxm_heap.py.

#include <gfx_gxm/gxm_heap.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using gfx_gxm::Heap;

namespace
{
int g_failures = 0;

void Expect(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

void Basics()
{
    Heap heap(1024);
    EXPECT(heap.CheckConsistent());
    const uint32_t a = heap.Allocate(100, 4);
    const uint32_t b = heap.Allocate(100, 4);
    EXPECT(a == 0);
    EXPECT(b == 112); // 100 rounded up to the 16-byte granule
    EXPECT(heap.SizeOf(a) == 100);
    EXPECT(heap.Used() == 224);
    EXPECT(heap.Allocations() == 2);
    EXPECT(heap.CheckConsistent());
    EXPECT(heap.Free(a));
    EXPECT(!heap.Free(a)); // twice
    EXPECT(!heap.Free(5)); // never returned
    EXPECT(heap.SizeOf(a) == 0);
    // The freed range at the bottom is reused first.
    EXPECT(heap.Allocate(50, 1) == 0);
    EXPECT(heap.CheckConsistent());
}

void Alignment()
{
    Heap heap(1 << 20);
    EXPECT(heap.Allocate(16, 16) == 0);
    const uint32_t aligned = heap.Allocate(4096, 4096);
    EXPECT(aligned == 4096);
    // The bytes skipped for alignment stay free and serve the next small block.
    EXPECT(heap.Allocate(32, 16) == 16);
    EXPECT(heap.FreeRanges() == 2);
    EXPECT(heap.Allocate(8, 3) == Heap::kNoSpace); // not a power of two
    EXPECT(heap.Allocate(8, 0) == Heap::kNoSpace);
    const uint32_t big = heap.Allocate(256 * 1024, 256 * 1024);
    EXPECT(big == 256 * 1024);
    EXPECT(heap.CheckConsistent());
}

void Merging()
{
    Heap heap(4096);
    uint32_t blocks[8];
    for (uint32_t &block : blocks)
        block = heap.Allocate(512, 16);
    EXPECT(heap.Allocate(1, 1) == Heap::kNoSpace); // full
    EXPECT(heap.LargestFree() == 0);
    // Free alternate blocks: four holes that cannot hold 1024 bytes.
    for (int i = 0; i < 8; i += 2)
        EXPECT(heap.Free(blocks[i]));
    EXPECT(heap.FreeRanges() == 4);
    EXPECT(heap.Allocate(1024, 16) == Heap::kNoSpace);
    // Freeing the rest merges everything back into one range.
    for (int i = 1; i < 8; i += 2)
        EXPECT(heap.Free(blocks[i]));
    EXPECT(heap.FreeRanges() == 1);
    EXPECT(heap.LargestFree() == 4096);
    EXPECT(heap.Used() == 0);
    EXPECT(heap.Peak() == 4096);
    EXPECT(heap.CheckConsistent());
}

void Edges()
{
    Heap empty;
    EXPECT(empty.Allocate(1, 1) == Heap::kNoSpace);
    EXPECT(empty.CheckConsistent());

    // A size that is not a multiple of the granule: the tail can still be used
    // by an allocation that fits in it exactly.
    Heap odd(100);
    EXPECT(odd.Allocate(96, 16) == 0);
    EXPECT(odd.Allocate(4, 1) == Heap::kNoSpace); // 4 rounds up to 16, only 4 left
    EXPECT(odd.CheckConsistent());

    // Near the top of the 32-bit range: no overflow in the arithmetic.
    Heap huge(0xfffffff0u);
    EXPECT(huge.Allocate(0xffffffe0u, 16) == 0);
    EXPECT(huge.Allocate(0x10u, 16) == 0xffffffe0u);
    EXPECT(huge.Allocate(0x10u, 16) == Heap::kNoSpace);
    EXPECT(huge.CheckConsistent());

    EXPECT(odd.Allocate(0, 1) == Heap::kNoSpace); // 0 is taken as 1: no room
    Heap zero(64);
    const uint32_t z = zero.Allocate(0, 1);
    EXPECT(z == 0 && zero.SizeOf(z) == 1);
}

void Stress()
{
    // Random allocations and frees against an independent model: no two live
    // blocks overlap, every block keeps its alignment, and the heap stays
    // consistent. Seeded, so a failure repeats.
    std::mt19937 random(1234);
    const uint32_t size = 8 << 20;
    Heap heap(size);
    struct Live
    {
        uint32_t offset, size;
    };
    std::vector<Live> live;
    uint32_t failures = 0;
    for (int step = 0; step < 20000; ++step)
    {
        if (live.empty() || random() % 3)
        {
            const uint32_t bytes = 1 + random() % (random() % 8 ? 4096 : 262144);
            const uint32_t align = 1u << (random() % 13);
            const uint32_t offset = heap.Allocate(bytes, align);
            if (offset == Heap::kNoSpace)
            {
                ++failures;
                continue;
            }
            EXPECT(offset % align == 0);
            EXPECT(uint64_t(offset) + bytes <= size);
            for (const Live &other : live)
                EXPECT(offset + bytes <= other.offset || other.offset + other.size <= offset);
            live.push_back({offset, bytes});
        }
        else
        {
            const size_t index = random() % live.size();
            EXPECT(heap.Free(live[index].offset));
            live[index] = live.back();
            live.pop_back();
        }
        if (step % 500 == 0)
            EXPECT(heap.CheckConsistent());
        if (g_failures > 20)
            return;
    }
    for (const Live &block : live)
        EXPECT(heap.Free(block.offset));
    EXPECT(heap.Used() == 0);
    EXPECT(heap.FreeRanges() == 1);
    EXPECT(heap.CheckConsistent());
    std::printf("stress: peak %u of %u bytes, %u allocations refused\n", heap.Peak(), size, failures);
}
} // namespace

int main()
{
    Basics();
    Alignment();
    Merging();
    Edges();
    Stress();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gxm heap: all checks passed\n");
    return 0;
}
