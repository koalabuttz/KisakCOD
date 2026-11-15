#pragma once
#include "r_material.h"
#include <universal/arch_intrin.h> // KISAK_ARCH_X86

// The SSE skinning path (r_model_skin_sse.cpp) moves 8-byte vertex halves in MMX
// registers, which share the x87 stack and need EMMS before float code runs.
// MSVC has no MMX intrinsics for x64, so x64 Windows builds use SSE2 registers
// for those halves instead (KISAK_SKIN_SSE2), and there is no MMX state to clear.
// Defining KISAK_SKIN_SSE2 elsewhere selects the SSE2 form too.
// Arm has neither and r_model_skin_sse.cpp builds empty there (sys_SSE is
// off). With GCC or Clang it skins with the compiler's vector types instead
// (r_model_skin_vec.h; NEON on Arm) while r_sse_skinning is on, and with the
// scalar code otherwise. Windows ARM64 (MSVC) always takes the scalar code.
#if defined(_M_X64) && !defined(KISAK_SKIN_SSE2)
#define KISAK_SKIN_SSE2 1
#endif
#if defined(KISAK_SKIN_SSE2) || !KISAK_ARCH_X86
inline void R_SkinEmptyMmxState() {}
#else
#include <mmintrin.h>
inline void R_SkinEmptyMmxState() { _mm_empty(); }
#endif

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


// r_model_skin_vec: the same bytes as R_SkinXSurfaceSkinned, four floats at a
// time. Only where r_model_skin_vec.h has KISAK_SKIN_VEC.
void R_SkinXSurfaceSkinnedVec(
    const XSurface *xsurf,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertex *skinVerticesOut);

// r_model_skin_sse
void R_SkinXSurfaceSkinnedSse(
    const XSurface *xsurf,
    const DObjSkelMat *boneMatrix,
    GfxPackedVertexNormal *skinVertNormalIn,
    GfxPackedVertexNormal *skinVertNormalOut,
    GfxPackedVertex *skinVerticesOut);