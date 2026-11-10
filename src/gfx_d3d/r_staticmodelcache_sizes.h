#pragma once

// The static model cache's sizes, from one number.
//
// The cache holds static models' vertices already moved to their place in
// the world, so that all of one model in view go out in one draw instead of
// one each. It has four sections of 2^R_SMC_SECTION_TREE_BITS trees. A tree
// is 32 leaves of 16 vertices, 32 bytes a vertex, and a model's level of
// detail takes 16 to 512 vertices of one tree. Each vertex also has room for
// four 16-bit indices in main memory. One draw reads one section, with
// indices that count from the section's start, so a section holds 65536
// vertices at most.
//
// Retail: 128 trees a section, 8 MB of vertices and 2 MB of indices. The PS
// Vita's GXM back end: 32 trees a section, 2 MB and 512 KB, because a
// console's GPU has 104 MB for a map and mp_crash leaves 6.5 of them free
// (docs/vita-client.md, "Memory"). A model that finds the cache full of the
// last four frames' models is drawn by itself that frame, as with
// r_smc_enable 0, so a smaller cache costs draws and never a model. What a
// view asks for: on mp_crash the retail cache held 2.2 MB at the team menu,
// which looks over the map, 5.3 MB after the first look around and 6.3 MB
// after a walk (r_smc_stats, Windows build, 2026-11-10); it frees nothing
// before it is full, so the last two are several views' worth.
//
// This header is plain arithmetic and includes nothing of the engine: the
// host test scripts/tests/test_static_model_cache_sizes.py builds it at
// every size.

#include <cstdint>

#ifndef R_SMC_SECTION_TREE_BITS
#ifdef KISAK_GFX_GXM
#define R_SMC_SECTION_TREE_BITS 5
#else
#define R_SMC_SECTION_TREE_BITS 7
#endif
#endif

#define R_SMC_SECTION_COUNT 4
#define R_SMC_TREE_COUNT (R_SMC_SECTION_COUNT << R_SMC_SECTION_TREE_BITS)
#define R_SMC_TREE_LEAFS 32
#define R_SMC_LEAF_VERTS 16
// A section's vertices: what one draw's 16-bit indices reach.
#define R_SMC_SECTION_VERTS (1u << (R_SMC_SECTION_TREE_BITS + 9))
#define R_SMC_VERT_COUNT (R_SMC_SECTION_COUNT * R_SMC_SECTION_VERTS)
#define R_SMC_VERTEX_BYTES (R_SMC_VERT_COUNT * 32u)
#define SMC_MAX_INDEX_IN_CACHE (R_SMC_VERT_COUNT * 4u)

static_assert(R_SMC_SECTION_TREE_BITS >= 1 && R_SMC_SECTION_TREE_BITS <= 7);
static_assert(R_SMC_SECTION_VERTS <= 0x10000);
#if R_SMC_SECTION_TREE_BITS == 7
// The retail game's literals.
static_assert(R_SMC_TREE_COUNT == 512 && R_SMC_SECTION_VERTS == 0x10000 && R_SMC_VERT_COUNT == 262144 &&
              R_SMC_VERTEX_BYTES == 0x800000 && SMC_MAX_INDEX_IN_CACHE == 0x100000);
#endif

// A cached surface's handle: one more than its leaf's number in the whole
// cache, so that 0 can mean "not cached". It fits the 16 bits it is kept in.
inline uint16_t R_StaticModelCacheIndex(uint32_t treeIndex, uint32_t leafIndex)
{
    return (uint16_t)(R_SMC_TREE_LEAFS * treeIndex + leafIndex + 1);
}
static_assert(R_SMC_TREE_LEAFS * R_SMC_TREE_COUNT <= 0xFFFF);

// Its first vertex in the cache's vertex buffer.
inline uint32_t R_StaticModelCacheBaseVert(uint32_t treeIndex, uint32_t leafIndex)
{
    return R_SMC_LEAF_VERTS * leafIndex + (treeIndex << 9);
}

// Where its section starts in the vertex buffer, in bytes: the stream offset
// of its draws (retail: ((cacheIndex - 1) & 0xFFFFF000) << 9).
inline uint32_t R_StaticModelCacheSectionOffset(uint32_t cacheIndex)
{
    const uint32_t sectionLeafs = (uint32_t)R_SMC_TREE_LEAFS << R_SMC_SECTION_TREE_BITS;
    return ((cacheIndex - 1) & ~(sectionLeafs - 1)) << 9;
}

// A vertex of the buffer as the indices of its section's draws name it
// (retail: the low 16 bits).
inline uint32_t R_StaticModelCacheSectionVert(uint32_t vertIndex)
{
    return vertIndex & (R_SMC_SECTION_VERTS - 1);
}

// How many surfaces of 2^allocBits vertices a section holds (retail:
// 1 << (16 - allocBits)).
inline uint32_t R_StaticModelCacheSectionEntries(uint32_t allocBits)
{
    return 1u << (R_SMC_SECTION_TREE_BITS + 9 - allocBits);
}
