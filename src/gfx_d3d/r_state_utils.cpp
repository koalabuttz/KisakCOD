#include <universal/q_shared.h>
#include "r_utils.h"
#include <universal/com_memory.h>
#include <aim_assist/aim_assist.h>
#include "r_state.h"
#include "r_dvars.h"
#include "r_cmdbuf.h"
#include <universal/surfaceflags.h>
#include "rb_logfile.h"
#include "rb_state.h"

#ifdef KISAK_RADIANT
#include <cgame/cg_local.h>
#endif

void R_DeriveNearPlaneConstantsForView(GfxCmdBufSourceState *source)
{
    const GfxViewParms *viewParms = &source->viewParms;
    const GfxMatrix *mtx = &viewParms->inverseViewProjectionMatrix;

    iassert(fabs(mtx->m[0][3]) < 1.0e-5f * mtx->m[3][3]);
    iassert(fabs(mtx->m[1][3]) < 1.0e-5f * mtx->m[3][3]);
    iassert( mtx->m[3][3] != 0 );

    float scale = 1.0 / mtx->m[3][3];

    R_SetCodeConstant(source,
        CONST_SRC_CODE_NEARPLANE_ORG,
        mtx->m[3][0] * scale - source->viewParms.origin[0],
        mtx->m[3][1] * scale - source->viewParms.origin[1],
        mtx->m[3][2] * scale - source->viewParms.origin[2],
        0.0
        );

    float scale2 = scale + scale;

    R_SetCodeConstant(source, 
        CONST_SRC_CODE_NEARPLANE_DX,
        mtx->m[0][0] * scale2,
        mtx->m[0][1] * scale2,
        mtx->m[0][2] * scale2,
        0.0
    );
    R_SetCodeConstant(source, 
        CONST_SRC_CODE_NEARPLANE_DY,
        mtx->m[1][0] * -scale2,
        mtx->m[1][1] * -scale2,
        mtx->m[1][2] * -scale2,
        0.0
    );
}

void R_SetGameTime(GfxCmdBufSourceState *source, float gameTime)
{
    float v2; // [esp+8h] [ebp-30h]
    float v3; // [esp+28h] [ebp-10h]
    float cosOfFracPartOfGameTime; // [esp+2Ch] [ebp-Ch]
    float sinOfFracPartOfGameTime; // [esp+30h] [ebp-8h]
    float fracPartOfGameTime; // [esp+34h] [ebp-4h]

    v2 = floor(gameTime);
    fracPartOfGameTime = gameTime - v2;
    v3 = fracPartOfGameTime * 6.283185482025146;
    cosOfFracPartOfGameTime = cos(v3);
    sinOfFracPartOfGameTime = sin(v3);
    source->input.consts[CONST_SRC_CODE_GAMETIME][0] = sinOfFracPartOfGameTime;
    source->input.consts[CONST_SRC_CODE_GAMETIME][1] = cosOfFracPartOfGameTime;
    source->input.consts[CONST_SRC_CODE_GAMETIME][2] = fracPartOfGameTime;
    source->input.consts[CONST_SRC_CODE_GAMETIME][3] = gameTime;
    R_DirtyCodeConstant(source, CONST_SRC_CODE_GAMETIME);
}

void R_BeginView(GfxCmdBufSourceState *source, const GfxSceneDef *sceneDef, const GfxViewParms *viewParms)
{
    source->sceneDef = *sceneDef;
    source->viewParms3D = viewParms;
    source->skinnedPlacement.base.origin[0] = sceneDef->viewOffset[0];
    source->skinnedPlacement.base.origin[1] = sceneDef->viewOffset[1];
    source->skinnedPlacement.base.origin[2] = sceneDef->viewOffset[2];
    source->viewMode = VIEW_MODE_NONE;
    R_Set3D(source);
    source->materialTime = 0.0;
    R_SetGameTime(source, source->sceneDef.floatTime);
    R_DeriveNearPlaneConstantsForView(source);
}

int R_PickMaterial(
    int traceMask,
    const float *org,
    const float *dir,
    char *name,
    char *surfaceFlags,
    char *contents,
    uint32_t charLimit)
{
    int v8; // ecx
    char v9; // [esp+4Fh] [ebp-55h]
    const char *material; // [esp+58h] [ebp-4Ch]
    float end[3]; // [esp+5Ch] [ebp-48h] BYREF
    trace_t trace; // [esp+68h] [ebp-3Ch] BYREF
    int contentsLen; // [esp+94h] [ebp-10h]
    int index; // [esp+98h] [ebp-Ch]
    int surfaceFlagsLen; // [esp+9Ch] [ebp-8h]
    int i; // [esp+A0h] [ebp-4h]

    Vec3Mad(org, 262144.0, dir, end);
    CM_BoxTrace(&trace, org, end, vec3_origin, vec3_origin, 0, traceMask);
    if (trace.startsolid || trace.allsolid || trace.fraction == 1.0 || !trace.material)
        return 0;
    material = trace.material;
    do
    {
        v9 = *material;
        *name++ = *material++;
    } while (v9);
    *surfaceFlags = 0;
    surfaceFlags[charLimit - 1] = 0;
    surfaceFlagsLen = 0;
    *contents = 0;
    contents[charLimit - 1] = 0;
    contentsLen = 0;
    v8 = SURF_TYPEINDEX(trace.surfaceFlags);
    index = (uint8_t)v8;
    if ((_BYTE)v8 && index < 29)
        strncpy(surfaceFlags, infoParms[index - 1].name, charLimit);
    else
        strncpy(surfaceFlags, "^1default^7", charLimit);
    if (surfaceFlags[charLimit - 1])
        return 0;
    surfaceFlagsLen = strlen(surfaceFlags);
    if ((trace.contents & CONTENTS_SOLID) != 0)
        strncpy(contents, "solid", charLimit);
    else
        strncpy(contents, "^3nonsolid^7", charLimit);
    if (contents[charLimit - 1])
        return 0;
    contentsLen = strlen(contents);
    for (i = 28; infoParms[i].name; ++i)
    {
        if ((trace.surfaceFlags & infoParms[i].surfaceFlags) != 0)
        {
            surfaceFlags[surfaceFlagsLen++] = 32;
            strncpy(
                &surfaceFlags[surfaceFlagsLen],
                infoParms[i].name,
                charLimit - surfaceFlagsLen);
            if (surfaceFlags[charLimit - 1])
                return 0;
            surfaceFlagsLen += strlen(&surfaceFlags[surfaceFlagsLen]);
        }
        if ((trace.contents & infoParms[i].contents) != 0)
        {
            contents[contentsLen++] = 32;
            strncpy(&contents[contentsLen], infoParms[i].name, charLimit - contentsLen);
            if (contents[charLimit - 1])
                return 0;
            contentsLen += strlen(&contents[contentsLen]);
        }
    }
    return 1;
}

void R_SetShadowLookupMatrix(GfxCmdBufSourceState *source, const GfxMatrix *matrix)
{
    ++source->matrixVersions[6];
    memcpy(&source->shadowLookupMatrix, matrix, sizeof(source->shadowLookupMatrix));
}

void R_Set2D(GfxCmdBufSourceState *source)
{
    if (source->viewMode != VIEW_MODE_2D)
    {
        source->viewMode = VIEW_MODE_2D;
        source->viewportIsDirty = 1;
        source->eyeOffset[0] = 0.0;
        source->eyeOffset[1] = 0.0;
        source->eyeOffset[2] = 0.0;
        source->eyeOffset[3] = 1.0;

        GfxViewport viewport;
        R_GetViewport(source, &viewport);
        R_CmdBufSet2D(source, &viewport);
    }
}

void R_CmdBufSet2D(GfxCmdBufSourceState* source, GfxViewport* viewport)
{
    float v3[16]; // [esp-8h] [ebp-9Ch] BYREF
    GfxMatrix identity_52; // [esp+38h] [ebp-5Ch] BYREF
    GfxViewParms* transform_56; // [esp+7Ch] [ebp-18h]
    float transform_60; // [esp+80h] [ebp-14h]
    float v7; // [esp+84h] [ebp-10h]

    //viewParms = v2;
    //invHeight = retaddr;
    iassert(viewport->width > 0);
    iassert(viewport->height > 0);
    v7 = 1.0 / (double)viewport->width;
    transform_60 = 1.0 / (double)viewport->height;
    transform_56 = &source->viewParms;
    memset((uint8_t*)&identity_52, 0, sizeof(identity_52));
    identity_52.m[0][0] = v7 * 2.0;
    identity_52.m[1][1] = transform_60 * -2.0;
    identity_52.m[3][0] = -1.0 - v7;
    identity_52.m[3][1] = transform_60 + 1.0;
    identity_52.m[3][2] = 1.0;
    identity_52.m[3][3] = 1.0;
    R_MatrixIdentity44((float (*)[4])v3);
    R_MatrixIdentity44(transform_56->viewMatrix.m);
    memcpy(&transform_56->projectionMatrix, &identity_52, sizeof(transform_56->projectionMatrix));
    memcpy(&transform_56->viewProjectionMatrix, &identity_52, sizeof(transform_56->viewProjectionMatrix));
    memset(
        (uint8_t*)&transform_56->inverseViewProjectionMatrix,
        0,
        sizeof(transform_56->inverseViewProjectionMatrix));
    ++source->matrixVersions[1];
    ++source->matrixVersions[2];
    ++source->matrixVersions[4];
    memcpy(R_GetActiveWorldMatrix(source), v3, 0x40u);
}

void R_WorldMatrixChanged(GfxCmdBufSourceState *source)
{
    ++source->matrixVersions[0];
    ++source->matrixVersions[3];
    ++source->matrixVersions[5];
    ++source->matrixVersions[7];
    source->constVersions[58] = source->matrixVersions[0];
}

GfxCmdBufSourceState *R_GetActiveWorldMatrix(GfxCmdBufSourceState *source)
{
    R_WorldMatrixChanged(source);
    return source;
}

double R_GetAdjustedLodDist(float dist, XModelLodRampType lodRampType)
{
    iassert(rg.lodParms.valid);
    return (dist * rg.lodParms.ramp[lodRampType].scale + rg.lodParms.ramp[lodRampType].bias);
}

double R_GetBaseLodDist(const float* origin)
{
    float diff[3]; // [esp+4h] [ebp-Ch] BYREF

    Vec3Sub(rg.lodParms.origin, origin, diff);
    return Vec3Length(diff);
}

const float g_identityMatrix44[4][4] =
{
  { 1.0, 0.0, 0.0, 0.0 },
  { 0.0, 1.0, 0.0, 0.0 },
  { 0.0, 0.0, 1.0, 0.0 },
  { 0.0, 0.0, 0.0, 1.0 }
}; // idb
void R_MatrixIdentity44(float (*out)[4])
{
    iassert( out );
    qmemcpy(out, g_identityMatrix44, 0x40u);
}

void R_Set3D(GfxCmdBufSourceState *source)
{
    iassert( source->viewParms3D );
    if (source->viewMode != VIEW_MODE_3D)
    {
        source->viewMode = VIEW_MODE_3D;
        qmemcpy(&source->viewParms, source->viewParms3D, sizeof(source->viewParms));
        if (source->viewParms.origin[3] == 0.0)
        {
            source->eyeOffset[0] = 0.0;
            source->eyeOffset[1] = 0.0;
            source->eyeOffset[2] = 0.0;
        }
        else
        {
            source->eyeOffset[0] = source->viewParms.origin[0];
            source->eyeOffset[1] = source->viewParms.origin[1];
            source->eyeOffset[2] = source->viewParms.origin[2];
        }
        source->eyeOffset[3] = 1.0;
        R_CmdBufSet3D(source);
    }
}

void R_InitCmdBufSourceState(GfxCmdBufSourceState *source, const GfxCmdBufInput *input, int cameraView)
{
    float *v3; // [esp+8h] [ebp-8h]

    iassert(source);
    iassert(input);

    memset((uint8_t *)source, 0, sizeof(GfxCmdBufSourceState));
    qmemcpy(&source->input, input, sizeof(source->input));

    for (uint32_t constant = 0; constant < 0x3A; ++constant)
    {
        if (input == &gfxCmdBufInput || s_codeConstUpdateFreq[constant] != 2 || constant < 0x20)
        {
            v3 = source->input.consts[constant];
            v3[0] = FLT_MAX;
            v3[1] = FLT_MAX;
            v3[2] = FLT_MAX;
            v3[3] = 0.0f;
        }
        else
        {
            source->constVersions[constant] = 1;
        }
    }

    iassert(source->shadowableLightForShadowLookupMatrix == PRIMARY_LIGHT_NONE);

    source->skinnedPlacement.base.quat[3] = 1.0f;
    source->skinnedPlacement.scale = 1.0f;
    source->cameraView = cameraView;
}

void R_SetDefaultStateBits(uint32_t *stateBits)
{
    *stateBits = 0;
    stateBits[1] = 0;
    *stateBits |= 0x800u;
    *stateBits |= 0x18000000u;
    *stateBits |= 0x4000u;
    *stateBits = *stateBits;
    *stateBits = *stateBits;
    stateBits[1] |= 2u;
    stateBits[1] = stateBits[1];
}

void R_InitCmdBufState(GfxCmdBufState *state)
{
    uint32_t samplerIndex; // [esp+10h] [ebp-Ch]
    IDirect3DDevice9 *device; // [esp+14h] [ebp-8h]
    uint32_t forceSamplerState; // [esp+18h] [ebp-4h]

    iassert( state );
    device = state->prim.device;
    memset(state->refSamplerState, 0, sizeof(GfxCmdBufState));
    state->prim.device = device;
    state->depthRangeType = GFX_DEPTH_RANGE_FULL;
    state->depthRangeNear = 0.0;
    state->depthRangeFar = 1.0;
    state->renderTargetId = R_RENDERTARGET_NONE;
    R_SetDefaultStateBits(state->activeStateBits);
    R_SetDefaultStateBits(state->refStateBits);
    R_SetCompleteState(device, state->activeStateBits);
    R_SetDefaultAlphaTestFunction(state);
    forceSamplerState = R_DecodeSamplerState(1);
    for (samplerIndex = 0; samplerIndex < 0x10; ++samplerIndex)
    {
        R_HW_ForceSamplerState(state->prim.device, samplerIndex, forceSamplerState);
        state->refSamplerState[samplerIndex] = 1;
        state->samplerState[samplerIndex] = forceSamplerState;
    }
    state->prim.vertexDecl = (IDirect3DVertexDeclaration9 *)-1;
    state->pixelShader = (const MaterialPixelShader *)-1;
}
