#pragma once
#include <xanim/xanim.h>
#include "rb_backend.h"

static _DWORD s_codeConstUpdateFreq[90] =
{
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0
}; // weak


uint32_t R_HashAssetName(const char *name);
uint32_t R_HashString(const char *string);
char *R_AllocGlobalVariable(uint32_t bytes, const char *name);
char R_CullPointAndRadius(const float *pt, float radius, const DpvsPlane *clipPlanes, int clipPlaneCount);
void R_ConvertColorToBytes(const float *colorFloat, uint32_t *colorBytes);
inline void R_ConvertColorToBytes(const float *colorFloat, unsigned char *colorBytes)
{
    R_ConvertColorToBytes(colorFloat, (uint32_t *)colorBytes);
}
inline void R_ConvertColorToBytes(const float *colorFloat, GfxColor *colorBytes)
{
    R_ConvertColorToBytes(colorFloat, (uint32_t *)colorBytes);
}
int R_PickMaterial(
    int traceMask,
    const float *org,
    const float *dir,
    char *name,
    char *surfaceFlags,
    char *contents,
    uint32_t charLimit);
double FresnelTerm(float n0, float n1, float cosIncidentAngle);
char R_GetClearColor(float *unpackedRgba);
void Byte4UnpackBgra(const uint8_t *from, float *to);


// r_state_utils
void R_BeginView(GfxCmdBufSourceState *source, const GfxSceneDef *sceneDef, const GfxViewParms *viewParms);
uint32_t R_HashAssetName(const char *name);
uint32_t R_HashString(const char *string);
char *R_AllocGlobalVariable(uint32_t bytes, const char *name);
char R_CullPointAndRadius(const float *pt, float radius, const DpvsPlane *clipPlanes, int clipPlaneCount);
int R_PickMaterial(
    int traceMask,
    const float *org,
    const float *dir,
    char *name,
    char *surfaceFlags,
    char *contents,
    uint32_t charLimit);
double FresnelTerm(float n0, float n1, float cosIncidentAngle);
char R_GetClearColor(float *unpackedRgba);
void Byte4UnpackBgra(const uint8_t *from, float *to);

void R_SetShadowLookupMatrix(GfxCmdBufSourceState *source, const GfxMatrix *matrix);
void R_Set2D(GfxCmdBufSourceState *source);
void R_Set3D(GfxCmdBufSourceState *source);
void R_CmdBufSet2D(GfxCmdBufSourceState* source, GfxViewport* viewport);
GfxCmdBufSourceState *R_GetActiveWorldMatrix(GfxCmdBufSourceState *source);
void R_InitCmdBufState(GfxCmdBufState *state);
void R_InitCmdBufSourceState(GfxCmdBufSourceState *source, const GfxCmdBufInput *input, int cameraView);

void R_MatrixIdentity44(float (*out)[4]);
double R_GetBaseLodDist(const float *origin);
double R_GetAdjustedLodDist(float dist, XModelLodRampType lodRampType);

#if !(defined(__linux__) || defined(__APPLE__) || defined(__vita__)) || !defined(KISAK_DEDICATED)
// Keep backend method calls out of shared headers. The real D3D backend owns
// this definition; Linux has no successful fallback for releasing GPU objects.
uint32_t R_ReleaseD3DSurface(IDirect3DSurface9 *surface);

template <typename T>
inline void R_ReleaseAndSetNULL(
    IDirect3DSurface9 *var,
    const char *fn,
    const char *filename,
    int line)
{
    uint32_t useCount; // [esp+0h] [ebp-4h]

    iassert(var);
    useCount = R_ReleaseD3DSurface(var);
    iassert(!useCount);
}
#endif
