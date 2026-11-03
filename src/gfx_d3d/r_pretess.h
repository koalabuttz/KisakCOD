#pragma once
#include "r_rendercmds.h"
#include "r_drawsurf.h"

#define MAX_DRAWSURFS 0x8000

enum DrawSurfType : __int32;

struct GfxBspPreTessDrawSurf // sizeof=0x4
{                                       // ...
    uint16_t baseSurfIndex;     // ...
    uint16_t totalTriCount;     // ...
};

void R_InitDrawSurfListInfo(GfxDrawSurfListInfo *info);
void R_EmitDrawSurfList(GfxDrawSurf *drawSurfs, uint32_t drawSurfCount);
void R_MergeAndEmitDrawSurfLists(DrawSurfType firstStage, int stageCount);
uint32_t R_EmitDrawSurfListForKey(
    const GfxDrawSurf *drawSurfs,
    uint32_t drawSurfCount,
    uint32_t primarySortKey);


uint16_t *R_AllocPreTessIndices(int count);

void R_EndPreTess();
void R_BeginPreTess();

int R_ReadBspPreTessDrawSurfs(
    struct GfxReadCmdBuf *cmdBuf,
    const struct GfxBspPreTessDrawSurf **list,
    uint32_t *count,
    uint32_t *baseIndex);