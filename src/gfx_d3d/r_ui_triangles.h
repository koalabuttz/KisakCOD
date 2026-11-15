#pragma once

// Cuts an indexed triangle mesh into runs that fit one RC_DRAW_UI_TRIANGLES
// command (R_AddCmdDrawUiTriangles, r_rendercmds.cpp). Header-only so the
// asset-free RmlUi test (tests/rmlui) checks the same code.
//
// Triangles keep their order. Each run holds at most MaxVerts vertices and
// MaxIndices indices, its own copy of every vertex its triangles use, and its
// indices renumbered into that copy. emit(verts, vertexCount, indices,
// indexCount) is called once per run. Every index must be below 65536.

#include <cstdint>

template <int MaxVerts, int MaxIndices, class Vertex, class Emit>
void R_SplitUiTriangles(const Vertex *verts, const uint16_t *indices, int indexCount, Emit &&emit)
{
    static_assert(MaxVerts >= 3 && MaxIndices >= 3, "a run must hold a triangle");

    // remap[v] is v's place in the current run, or -1. A run clears the
    // entries it used when it ends, so the table stays all -1 between calls.
    // The buffers are kept off the stack (the Vita's threads have small ones).
    static thread_local int remap[65536];
    static thread_local bool remapReady;
    static thread_local Vertex runVerts[MaxVerts];
    static thread_local uint16_t runIndices[MaxIndices];
    static thread_local uint16_t runSources[MaxVerts];
    if (!remapReady)
    {
        for (int &entry : remap)
            entry = -1;
        remapReady = true;
    }

    int runVertCount = 0;
    int runIndexCount = 0;
    auto flush = [&]() {
        if (runIndexCount)
            emit(runVerts, runVertCount, runIndices, runIndexCount);
        for (int i = 0; i < runVertCount; ++i)
            remap[runSources[i]] = -1;
        runVertCount = 0;
        runIndexCount = 0;
    };
    for (int tri = 0; tri + 2 < indexCount; tri += 3)
    {
        int fresh = 0;
        for (int k = 0; k < 3; ++k)
            fresh += remap[indices[tri + k]] < 0 ? 1 : 0;
        if (runVertCount + fresh > MaxVerts || runIndexCount + 3 > MaxIndices)
            flush();
        for (int k = 0; k < 3; ++k)
        {
            const uint16_t source = indices[tri + k];
            if (remap[source] < 0)
            {
                remap[source] = runVertCount;
                runSources[runVertCount] = source;
                runVerts[runVertCount++] = verts[source];
            }
            runIndices[runIndexCount++] = (uint16_t)remap[source];
        }
    }
    flush();
}
