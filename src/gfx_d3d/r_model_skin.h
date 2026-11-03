#pragma once
#include "r_material.h"

void  R_SkinXModelCmd(_WORD *data);
void R_SkinXSurfaceSkinned(
    const struct XSurface *xsurf,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *skinVerticesOut);
void R_SkinXSurfaceWeight(
    const GfxPackedVertex *inVerts,
    const struct XSurfaceVertexInfo *vertexInfo,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *outVerts);
void R_SkinXSurfaceWeight0(
    const GfxPackedVertex *vertsIn,
    const uint16_t *vertexBlend,
    int vertCount,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *vertsOut);
void R_SkinXSurfaceWeight1(
    const GfxPackedVertex *vertsIn,
    const uint16_t *vertexBlend,
    int vertCount,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *vertsOut);
void R_SkinXSurfaceWeight2(
    const GfxPackedVertex *vertsIn,
    const uint16_t *vertexBlend,
    int vertCount,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *vertsOut);
void R_SkinXSurfaceWeight3(
    const GfxPackedVertex *vertsIn,
    const uint16_t *vertexBlend,
    int vertCount,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *vertsOut);
void R_SkinXSurfaceRigid(
    const XSurface *surf,
    int totalVertCount,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *vertices);


// r_model_skin_sse
void R_SkinXSurfaceSkinnedSse(
    const XSurface *xsurf,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertexNormal *skinVertNormalIn,
    GfxPackedVertexNormal *skinVertNormalOut,
    GfxPackedVertex *skinVerticesOut);