#pragma once
#include "r_gfx.h"
#include "rb_backend.h"

struct GfxMeshGlobals // sizeof=0x180
{                                       // ...
    GfxQuadMeshData fullSceneViewMesh[4]; // ...
    GfxMeshData spotShadowClearMeshData[4]; // ...
    GfxMeshData sunShadowClearMeshData[2]; // ...
};

char R_ReserveMeshIndices(GfxMeshData *mesh, int indexCount, r_double_index_t **indicesOut);
char R_ReserveMeshVerts(GfxMeshData *mesh, int vertCount, uint16_t *baseVertex);
uint8_t *R_GetMeshVerts(GfxMeshData *mesh, uint16_t baseVertex);
void R_ResetMesh(GfxMeshData *mesh);
void R_SetQuadMeshData(
    GfxMeshData *mesh,
    float x,
    float y,
    float w,
    float h,
    float s0,
    float t0,
    float s1,
    float t1,
    uint32_t color);
void R_SetQuadMesh(
    GfxQuadMeshData *quadMesh,
    float x,
    float y,
    float w,
    float h,
    float s0,
    float t0,
    float s1,
    float t1,
    uint32_t color);
void R_DrawQuadMesh(GfxCmdBufContext context, const Material *material, GfxMeshData *quadMesh);
void R_BeginMeshVerts(GfxMeshData* mesh);


extern GfxMeshGlobals gfxMeshGlob;