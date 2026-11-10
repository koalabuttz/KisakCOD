// Host test of the static model cache's size arithmetic
// (src/gfx_d3d/r_staticmodelcache_sizes.h), built once for every size the
// header allows and once each for the two defaults. See
// scripts/tests/test_static_model_cache_sizes.py.
//
// The cache's code keeps three views of one place in the cache: a handle
// (16 bits, in the world's draw instances), a vertex number in the whole
// vertex buffer (where the vertices are written and where the indices are
// kept), and a stream offset plus a 16-bit index (what a draw reads). The
// retail game derived each from the others with literals that only hold for
// its 512 trees. These checks walk every tree and leaf of a cache of the
// given size and make sure the three views name the same vertex.

#include <gfx_d3d/r_staticmodelcache_sizes.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
int g_failed;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            if (++g_failed <= 20)                                                                                      \
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                      \
        }                                                                                                              \
    } while (0)

const uint32_t kTreesASection = 1u << R_SMC_SECTION_TREE_BITS;
const uint32_t kVertexBytes = 32;

void Totals()
{
    CHECK(R_SMC_TREE_COUNT == R_SMC_SECTION_COUNT * kTreesASection);
    CHECK(R_SMC_VERT_COUNT == R_SMC_TREE_COUNT * R_SMC_TREE_LEAFS * R_SMC_LEAF_VERTS);
    CHECK(R_SMC_SECTION_VERTS == kTreesASection * R_SMC_TREE_LEAFS * R_SMC_LEAF_VERTS);
    CHECK(R_SMC_VERTEX_BYTES == R_SMC_VERT_COUNT * kVertexBytes);
    CHECK(SMC_MAX_INDEX_IN_CACHE == R_SMC_VERT_COUNT * 4u);
    // A draw's indices are 16 bits and count from its section's start.
    CHECK(R_SMC_SECTION_VERTS <= 0x10000u);
#ifdef EXPECT_BITS
    CHECK(R_SMC_SECTION_TREE_BITS == EXPECT_BITS);
#endif
}

// Every leaf of every tree, at every size a model's level of detail takes
// (2^4 to 2^9 vertices, at a leaf that size is a multiple of).
void Places()
{
    std::vector<uint8_t> handleSeen(0x10000, 0);
    for (uint32_t section = 0; section < R_SMC_SECTION_COUNT; ++section)
    {
        for (uint32_t treeInSection = 0; treeInSection < kTreesASection; ++treeInSection)
        {
            // As SMC_ClearCache hands a section its trees.
            const uint32_t tree = treeInSection + (section << R_SMC_SECTION_TREE_BITS);
            CHECK(tree < R_SMC_TREE_COUNT);
            for (uint32_t leaf = 0; leaf < R_SMC_TREE_LEAFS; ++leaf)
            {
                const uint32_t handle = R_StaticModelCacheIndex(tree, leaf);
                // 0 means "not cached"; SMC_GetLeaf reads leafs[0][handle - 1].
                CHECK(handle != 0);
                CHECK(handle - 1 == R_SMC_TREE_LEAFS * tree + leaf);
                CHECK(!handleSeen[handle]);
                handleSeen[handle] = 1;

                const uint32_t base = R_StaticModelCacheBaseVert(tree, leaf);
                // Leaves do not share vertices.
                CHECK(base == R_SMC_LEAF_VERTS * (R_SMC_TREE_LEAFS * tree + leaf));

                const uint32_t sectionOffset = R_StaticModelCacheSectionOffset(handle);
                const uint32_t sectionVert = R_StaticModelCacheSectionVert(base);
                CHECK(sectionOffset == section * R_SMC_SECTION_VERTS * kVertexBytes);
                // The draw reads the vertex the patch wrote.
                CHECK(sectionOffset + kVertexBytes * sectionVert == kVertexBytes * base);

                for (uint32_t allocBits = 4; allocBits <= 9; ++allocBits)
                {
                    const uint32_t verts = 1u << allocBits;
                    if (leaf % (verts / R_SMC_LEAF_VERTS))
                        continue;
                    CHECK(base + verts <= R_SMC_VERT_COUNT);
                    CHECK(kVertexBytes * (base + verts) <= R_SMC_VERTEX_BYTES);
                    // Its last vertex is in its own section and fits 16 bits.
                    CHECK(sectionVert + verts <= R_SMC_SECTION_VERTS);
                    CHECK(sectionVert + verts - 1 <= 0xFFFFu);
                    // Four indices a vertex, kept at four times the vertex.
                    CHECK(4 * (base + verts) <= SMC_MAX_INDEX_IN_CACHE);
                }

#if R_SMC_SECTION_TREE_BITS == 7
                // The retail game's own expressions.
                CHECK(sectionOffset == (((handle - 1) & 0xFFFFF000u) << 9));
                CHECK(sectionVert == (uint16_t)base);
                CHECK(handle == 32 * tree + leaf + 1);
                CHECK(base == 16 * leaf + (tree << 9));
#endif
            }
        }
    }
}

void Entries()
{
    for (uint32_t allocBits = 4; allocBits <= 9; ++allocBits)
    {
        // What the map load plans for a section is what the section holds.
        CHECK(R_StaticModelCacheSectionEntries(allocBits) * (1u << allocBits) == R_SMC_SECTION_VERTS);
#if R_SMC_SECTION_TREE_BITS == 7
        CHECK(R_StaticModelCacheSectionEntries(allocBits) == 1u << (16 - allocBits));
#endif
    }
}
} // namespace

int main()
{
    Totals();
    Places();
    Entries();
    if (g_failed)
    {
        std::printf("FAIL: %d checks failed with %d trees a section\n", g_failed, (int)kTreesASection);
        return 1;
    }
    std::printf("ok: %u trees a section, %u KB of vertices, %u KB of indices\n", kTreesASection,
                (unsigned)(R_SMC_VERTEX_BYTES >> 10), (unsigned)(SMC_MAX_INDEX_IN_CACHE * 2u >> 10));
    return 0;
}
