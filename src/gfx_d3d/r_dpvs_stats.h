#pragma once

#include <universal/q_shared.h>

#include <atomic>
#include <cstdint>

// Everything here is inline: r_cmds.cpp registers the command, r_dpvs.cpp and
// r_dpvs_static.cpp count, and not every tool that builds one builds the others.
//
// r_dpvs_stats: what the culling of a cell's models and surfaces does, counted
// from one call of the command to the next. For deciding what to make faster:
// the walk tests boxes against planes one at a time, and what a faster test
// is worth depends on how many planes a box meets and where.
enum DpvsCount
{
    DPVS_COUNT_VIEWS,
    DPVS_COUNT_CELLS,
    DPVS_COUNT_CELL_PLANES,
    DPVS_COUNT_NODES,
    DPVS_COUNT_NODE_PLANES,
    DPVS_COUNT_NODES_OUTSIDE,
    DPVS_COUNT_NODES_INSIDE,
    DPVS_COUNT_LEAVES_CLIPPED,
    DPVS_COUNT_LEAF_PLANES_1,
    DPVS_COUNT_LEAF_PLANES_2,
    DPVS_COUNT_LEAF_PLANES_3,
    DPVS_COUNT_LEAF_PLANES_4,
    DPVS_COUNT_LEAF_PLANES_MORE,
    DPVS_COUNT_SMODELS_WHOLE,
    DPVS_COUNT_SMODELS_SEEN,
    DPVS_COUNT_SMODELS_TESTED,
    DPVS_COUNT_SMODEL_PLANES,
    DPVS_COUNT_SMODELS_CULLED,
    DPVS_COUNT_SURFACES_WHOLE,
    DPVS_COUNT_SURFACES_SEEN,
    DPVS_COUNT_SURFACES_TESTED,
    DPVS_COUNT_SURFACE_PLANES,
    DPVS_COUNT_SURFACES_CULLED,
    DPVS_COUNT_CULL_GROUPS,
    DPVS_COUNT_COUNT,
};
inline std::atomic<bool> s_dpvsCounting{false};
inline std::atomic<uint32_t> s_dpvsCounts[DPVS_COUNT_COUNT];

inline void DpvsCountAdd(DpvsCount which, uint32_t count)
{
    s_dpvsCounts[which].fetch_add(count, std::memory_order_relaxed);
}
inline void R_DpvsStatsCountView()
{
    if (s_dpvsCounting.load(std::memory_order_relaxed))
        DpvsCountAdd(DPVS_COUNT_VIEWS, 1);
}

inline void R_DpvsStats_f()
{
    if (!s_dpvsCounting.load(std::memory_order_relaxed))
    {
        for (std::atomic<uint32_t> &count : s_dpvsCounts)
            count.store(0, std::memory_order_relaxed);
        s_dpvsCounting.store(true, std::memory_order_relaxed);
        Com_Printf(CON_CHANNEL_GFX, "r_dpvs_stats: counting; call it again for the figures\n");
        return;
    }
    s_dpvsCounting.store(false, std::memory_order_relaxed);
    uint32_t n[DPVS_COUNT_COUNT];
    for (int i = 0; i < DPVS_COUNT_COUNT; ++i)
        n[i] = s_dpvsCounts[i].load(std::memory_order_relaxed);
    const double views = n[DPVS_COUNT_VIEWS] ? n[DPVS_COUNT_VIEWS] : 1;
    Com_Printf(CON_CHANNEL_GFX,
               "r_dpvs_stats: a view, over %u views: %.1f cells with %.1f planes each; %.0f tree nodes with %.0f "
               "plane tests (two box corners each), %.0f outside, %.0f wholly inside, %.0f leaves clipped\n",
               n[DPVS_COUNT_VIEWS], n[DPVS_COUNT_CELLS] / views,
               n[DPVS_COUNT_CELLS] ? double(n[DPVS_COUNT_CELL_PLANES]) / n[DPVS_COUNT_CELLS] : 0.0,
               n[DPVS_COUNT_NODES] / views, n[DPVS_COUNT_NODE_PLANES] / views, n[DPVS_COUNT_NODES_OUTSIDE] / views,
               n[DPVS_COUNT_NODES_INSIDE] / views, n[DPVS_COUNT_LEAVES_CLIPPED] / views);
    Com_Printf(CON_CHANNEL_GFX,
               "r_dpvs_stats: clipped leaves by planes left: 1: %.0f, 2: %.0f, 3: %.0f, 4: %.0f, more: %.0f\n",
               n[DPVS_COUNT_LEAF_PLANES_1] / views, n[DPVS_COUNT_LEAF_PLANES_2] / views,
               n[DPVS_COUNT_LEAF_PLANES_3] / views, n[DPVS_COUNT_LEAF_PLANES_4] / views,
               n[DPVS_COUNT_LEAF_PLANES_MORE] / views);
    Com_Printf(CON_CHANNEL_GFX,
               "r_dpvs_stats: static models: %.0f marked with their leaf, %.0f seen before, %.0f tested with %.0f "
               "plane tests, %.0f of them culled\n",
               n[DPVS_COUNT_SMODELS_WHOLE] / views, n[DPVS_COUNT_SMODELS_SEEN] / views,
               n[DPVS_COUNT_SMODELS_TESTED] / views, n[DPVS_COUNT_SMODEL_PLANES] / views,
               n[DPVS_COUNT_SMODELS_CULLED] / views);
    Com_Printf(CON_CHANNEL_GFX,
               "r_dpvs_stats: surfaces: %.0f marked with their leaf, %.0f seen before, %.0f tested with %.0f plane "
               "tests, %.0f of them culled; %.0f cull groups tested\n",
               n[DPVS_COUNT_SURFACES_WHOLE] / views, n[DPVS_COUNT_SURFACES_SEEN] / views,
               n[DPVS_COUNT_SURFACES_TESTED] / views, n[DPVS_COUNT_SURFACE_PLANES] / views,
               n[DPVS_COUNT_SURFACES_CULLED] / views, n[DPVS_COUNT_CULL_GROUPS] / views);
}
