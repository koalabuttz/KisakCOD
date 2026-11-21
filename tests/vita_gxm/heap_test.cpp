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

void TopDown()
{
    // Pieces asked for from the top go at the top end of the highest range
    // that fits, aligned down; the bottom stays for the others.
    Heap heap(4096);
    EXPECT(heap.Allocate(1000, 256, true) == 3072); // 4096 - 1008 = 3088, down to 3072
    EXPECT(heap.Allocate(100, 16) == 0);
    EXPECT(heap.Allocate(512, 16, true) == 2560);
    // The first piece spans 1008 bytes from 3072, leaving [4080, 4096).
    EXPECT(heap.FreeRanges() == 2); // [112, 2560) and [4080, 4096)
    EXPECT(heap.LargestFree() == 2448);
    // The highest range that fits: [4080, 4096) is too small.
    EXPECT(heap.Allocate(1000, 16, true) == 1552); // 2560 - 1008
    EXPECT(heap.CheckConsistent());
    const std::vector<uint32_t> ranges = heap.LargestFreeRanges(4);
    EXPECT(ranges.size() == 2 && ranges[0] == 1440 && ranges[1] == 16); // [112, 1552), [4080, 4096)
    EXPECT(heap.Allocate(1441, 16, true) == Heap::kNoSpace);
    EXPECT(heap.Allocate(1440, 16, true) == 112);
    EXPECT(heap.LargestFree() == 16);
    EXPECT(heap.CheckConsistent());
    // Alignment that does not fit inside the range moves on to a lower one.
    Heap aligned(8192);
    EXPECT(aligned.Allocate(4096, 16) == 0);
    EXPECT(aligned.Allocate(16, 16) == 4096);
    // Free: [4112, 8192). The highest 4096-aligned offset with 64 bytes
    // before 8192 is 4096, below the range's start: no room.
    EXPECT(aligned.Allocate(64, 4096, true) == Heap::kNoSpace);
    EXPECT(aligned.Allocate(64, 1024, true) == 7168);
    EXPECT(aligned.CheckConsistent());
    // Several ranges, largest first, cut to the count.
    Heap many(1024);
    uint32_t pieces[8];
    for (uint32_t &piece : pieces)
        piece = many.Allocate(128, 16);
    EXPECT(many.Free(pieces[1]) && many.Free(pieces[3]) && many.Free(pieces[4]) && many.Free(pieces[6]));
    const std::vector<uint32_t> two = many.LargestFreeRanges(2);
    EXPECT(two.size() == 2 && two[0] == 256 && two[1] == 128);
    EXPECT(many.LargestFreeRanges(10).size() == 3);
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
            const uint32_t offset = heap.Allocate(bytes, align, random() % 4 == 0);
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
    TopDown();
    Stress();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gxm heap: all checks passed\n");
    return 0;
}
