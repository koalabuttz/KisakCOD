#pragma once

int RB_AddDebugLine(
    const float *start,
    const float *end,
    const float *color,
    bool depthTest,
    int vertCount,
    int vertLimit,
    struct GfxPointVertex *verts);

int RB_EndDebugLines(bool depthTest, int vertCount, const struct GfxPointVertex *verts);

void RB_DrawDebug(const struct GfxViewParms *viewParms);