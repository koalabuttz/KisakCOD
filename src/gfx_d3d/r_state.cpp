#include <universal/q_shared.h>
#include "r_state.h"
#include "rb_logfile.h"
#include "r_dvars.h"
#include "rb_stats.h"
#include "r_draw_bsp.h"
#include "rb_state.h"
#include "rb_pixelcost.h"
#include "r_rendertarget.h"
#include "r_utils.h"
#include "r_reflection_probe.h"

//float const *const shadowmapClearColor 820ebb50     gfx_d3d : r_state.obj
//BOOL g_renderTargetIsOverridden 85b5dd38     gfx_d3d : r_state.obj
//uint32_t *s_decodeSamplerFilterState 85b5dcb8     gfx_d3d : r_state.obj
//struct GfxScaledPlacement s_manualObjectPlacement 85b5dd18     gfx_d3d : r_state.obj

uint32_t s_decodeSamplerFilterState[24];

const _D3DTEXTUREFILTERTYPE s_mipFilterTable[4][3] =
{
  { D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR },
  { D3DTEXF_NONE, D3DTEXF_LINEAR, D3DTEXF_LINEAR },
  { D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_POINT },
  { D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_NONE }
}; // idb


void R_ChangeIndices(GfxCmdBufPrimState *state, IDirect3DIndexBuffer9 *ib)
{
    IDirect3DDevice9 *device; // [esp+4h] [ebp-4h]

    iassert( ib != state->indexBuffer );
    state->indexBuffer = ib;
    device = state->device;
    iassert( device );
    R_HW_SetIndices(device, ib);
}

void R_ChangeStreamSource(
    GfxCmdBufPrimState *state,
    uint32_t streamIndex,
    IDirect3DVertexBuffer9 *vb,
    uint32_t vertexOffset,
    uint32_t vertexStride)
{
    IDirect3DDevice9 *device; // [esp+4h] [ebp-4h]

    iassert(state->streams[streamIndex].vb != vb || state->streams[streamIndex].offset != vertexOffset || state->streams[streamIndex].stride != vertexStride);

    state->streams[streamIndex].vb = vb;
    state->streams[streamIndex].offset = vertexOffset;
    state->streams[streamIndex].stride = vertexStride;
    device = state->device;

    iassert(device);
    R_HW_SetStreamSource(device, streamIndex, vb, vertexOffset, vertexStride);
}

void R_SetTexFilter()
{
    int v0; // [esp+0h] [ebp-60h]
    int v1; // [esp+4h] [ebp-5Ch]
    int v2; // [esp+8h] [ebp-58h]
    int v3; // [esp+Ch] [ebp-54h]
    int v4; // [esp+10h] [ebp-50h]
    int v5; // [esp+14h] [ebp-4Ch]
    int v6; // [esp+1Ch] [ebp-44h]
    int integer; // [esp+24h] [ebp-3Ch]
    uint32_t entryIndex; // [esp+30h] [ebp-30h]
    int maxAniso; // [esp+34h] [ebp-2Ch]
    uint32_t mipFilterMode; // [esp+38h] [ebp-28h]
    int linearMippedAnisotropy; // [esp+3Ch] [ebp-24h]
    int linearNonMippedFilter; // [esp+40h] [ebp-20h]
    int linearMippedFilter; // [esp+44h] [ebp-1Ch]
    int texFilter; // [esp+48h] [ebp-18h]
    int anisotropicFilter; // [esp+50h] [ebp-10h]
    int anisotropyFor4x; // [esp+54h] [ebp-Ch]
    _D3DTEXTUREFILTERTYPE decoded; // [esp+58h] [ebp-8h]
    uint32_t decodeda; // [esp+58h] [ebp-8h]
    int anisotropyFor2x; // [esp+5Ch] [ebp-4h]

    maxAniso = r_texFilterAnisoMax->current.integer;
    if (maxAniso > gfxMetrics.maxAnisotropy)
        maxAniso = gfxMetrics.maxAnisotropy;
    if (maxAniso > 1)
    {
        anisotropicFilter = (gfxMetrics.hasAnisotropicMinFilter ? 768 : 512)
            | (gfxMetrics.hasAnisotropicMagFilter ? 12288 : 0x2000);
    }
    else
    {
        maxAniso = 1;
        anisotropicFilter = 8704;
    }
    if (maxAniso >= r_texFilterAnisoMin->current.integer)
        integer = r_texFilterAnisoMin->current.integer;
    else
        integer = maxAniso;
    if (integer <= 1)
        v6 = 1;
    else
        v6 = integer;
    //iassert( max( minAniso, min( maxAniso, 1 ) ) == minAniso );
    linearMippedAnisotropy = integer;
    if (integer == 1)
    {
        linearMippedFilter = 8704;
    }
    else
    {
        iassert( linearMippedAnisotropy > 1 );
        linearMippedFilter = anisotropicFilter;
    }
    if (maxAniso >= 2)
        v5 = 2;
    else
        v5 = maxAniso;
    if (integer <= v5)
    {
        if (maxAniso >= 2)
            v3 = 2;
        else
            v3 = maxAniso;
        v4 = v3;
    }
    else
    {
        v4 = integer;
    }
    anisotropyFor2x = v4;
    if (maxAniso >= 4)
        v2 = 4;
    else
        v2 = maxAniso;
    if (integer <= v2)
    {
        if (maxAniso >= 4)
            v0 = 4;
        else
            v0 = maxAniso;
        v1 = v0;
    }
    else
    {
        v1 = integer;
    }
    anisotropyFor4x = v1;
    if (r_texFilterDisable->current.enabled)
    {
        linearMippedAnisotropy = 1;
        anisotropyFor2x = 1;
        anisotropyFor4x = 1;
        anisotropicFilter = 4352;
        linearMippedFilter = 4352;
        linearNonMippedFilter = 4352;
        mipFilterMode = 3;
    }
    else
    {
        linearNonMippedFilter = 8704;
        mipFilterMode = r_texFilterMipMode->current.unsignedInt;
        if (mipFilterMode >= 4)
            MyAssertHandler(
                ".\\r_state.cpp",
                169,
                0,
                "mipFilterMode doesn't index R_MIP_FILTER_COUNT\n\t%i not in [0, %i)",
                mipFilterMode,
                4);
    }
    for (entryIndex = 0; entryIndex < 0x18; ++entryIndex)
    {
        texFilter = entryIndex & 7;
        decoded = (_D3DTEXTUREFILTERTYPE)(s_mipFilterTable[mipFilterMode][(int)(entryIndex & 0x18) >> 3] << 16);
        switch (texFilter)
        {
        case 2:
            if (decoded)
                decodeda = decoded | linearMippedAnisotropy | linearMippedFilter;
            else
                decodeda = linearNonMippedFilter | 1;
            break;
        case 3:
            decodeda = decoded | anisotropyFor2x | anisotropicFilter;
            break;
        case 4:
            decodeda = decoded | anisotropyFor4x | anisotropicFilter;
            break;
        default:
            decodeda = decoded | 0x1101;
            break;
        }
        s_decodeSamplerFilterState[entryIndex] = decodeda;
    }
    dx.mipBias = r_texFilterMipBias->current.unsignedInt;
}

void R_ChangeDepthHackNearClip(GfxCmdBufSourceState *source, uint32_t depthHackFlags)
{
    iassert( source );
    if (depthHackFlags != source->depthHackFlags)
    {
        R_DepthHackNearClipChanged(source);
        source->depthHackFlags = depthHackFlags;
    }
}

void R_DepthHackNearClipChanged(GfxCmdBufSourceState *source)
{
    ++source->matrixVersions[2];
    ++source->matrixVersions[4];
    ++source->matrixVersions[5];
    source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][3] = -source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][3];
    R_DirtyCodeConstant(source, CONST_SRC_CODE_DEPTH_FROM_CLIP);
}

GfxCmdBufSourceState *R_GetCodeMatrix(
    GfxCmdBufSourceState *source,
    uint32_t sourceIndex,
    uint32_t firstRow)
{
    uint32_t baseIndex; // [esp+ECh] [ebp-18h]
    uint32_t transposeIndex; // [esp+F0h] [ebp-14h]
    uint32_t matrixIndex; // [esp+F8h] [ebp-Ch]
    uint32_t inverseIndex; // [esp+FCh] [ebp-8h]
    uint32_t matrixVersion; // [esp+100h] [ebp-4h]

    iassert((source->matrixVersions[(((sourceIndex)-CONST_SRC_FIRST_CODE_MATRIX) >> 2)]));
    iassert(firstRow >= 0 && firstRow <= 3);

    matrixVersion = source->matrixVersions[(sourceIndex - CONST_SRC_FIRST_CODE_MATRIX) >> 2];
    matrixIndex = sourceIndex - CONST_SRC_FIRST_CODE_MATRIX;

    if (source->constVersions[sourceIndex] == matrixVersion)
        return (GfxCmdBufSourceState *)((char *)source + 64 * matrixIndex + 16 * firstRow);

    baseIndex = matrixIndex & 0xFFFFFFFC;
    if (source->constVersions[(matrixIndex & 0xFFFFFFFC) + CONST_SRC_FIRST_CODE_MATRIX] != matrixVersion)
    {
        R_DeriveCodeMatrix(source, &source->matrices, baseIndex);
        if (matrixIndex == baseIndex)
            return (GfxCmdBufSourceState *)((char *)source + 64 * matrixIndex + 16 * firstRow);

        iassert(source->constVersions[sourceIndex] != matrixVersion);
    }
    source->constVersions[sourceIndex] = matrixVersion;
    transposeIndex = matrixIndex ^ 2;
    if (source->constVersions[(matrixIndex ^ 2) + CONST_SRC_FIRST_CODE_MATRIX] == matrixVersion)
    {
        MatrixTranspose44(source->matrices.matrix[transposeIndex].m, source->matrices.matrix[matrixIndex].m);

        return (GfxCmdBufSourceState *)((char *)source + 64 * matrixIndex + 16 * firstRow);
    }
    else
    {
        inverseIndex = matrixIndex ^ 1;
        if (source->constVersions[(matrixIndex ^ 1) + CONST_SRC_FIRST_CODE_MATRIX] == matrixVersion)
        {
            MatrixInverse44(source->matrices.matrix[inverseIndex].m, source->matrices.matrix[matrixIndex].m);

            return (GfxCmdBufSourceState *)((char *)source + 64 * matrixIndex + 16 * firstRow);
        }
        else
        {
            iassert(matrixIndex == (baseIndex | CONST_SRC_MATRIX_INVERSE_BIT | CONST_SRC_MATRIX_TRANSPOSE_BIT));
            iassert(transposeIndex == (baseIndex | CONST_SRC_MATRIX_INVERSE_BIT));
            iassert(inverseIndex == (baseIndex | CONST_SRC_MATRIX_TRANSPOSE_BIT));

            MatrixTranspose44(source->matrices.matrix[baseIndex].m, source->matrices.matrix[inverseIndex].m);
            source->constVersions[inverseIndex + 58] = matrixVersion;
            MatrixInverse44(source->matrices.matrix[inverseIndex].m, source->matrices.matrix[matrixIndex].m);

            return (GfxCmdBufSourceState *)((char *)source + 64 * matrixIndex + 16 * firstRow);
        }
    }
}

void R_DeriveCodeMatrix(GfxCmdBufSourceState *source, GfxCodeMatrices *activeMatrices, uint32_t baseIndex)
{
    const char *v3; // eax

    switch (baseIndex) // LWSS: mystery enum of sorts
    {
    case 4:
        R_DeriveViewMatrix(source);
        break;
    case 8:
        R_DeriveProjectionMatrix(source);
        break;
    case 12:
        R_DeriveWorldViewMatrix(source);
        break;
    case 16:
        R_DeriveViewProjectionMatrix(source);
        break;
    case 20:
        R_DeriveWorldViewProjectionMatrix(source);
        break;
    case 24:
        R_DeriveShadowLookupMatrix(source);
        break;
    case 28:
        R_GenerateWorldOutdoorLookupMatrix(source, activeMatrices->matrix[baseIndex].m);
        break;
    default:
        if (!alwaysfails)
        {
            v3 = va("unhandled case %i", baseIndex);
            MyAssertHandler(".\\r_state.cpp", 543, 1, v3);
        }
        break;
    }
}

void R_DeriveViewMatrix(GfxCmdBufSourceState *source)
{
    memcpy(&source->matrices.matrix[4], &source->viewParms, sizeof(source->matrices.matrix[4]));
    MatrixTransformVector44(source->eyeOffset, source->matrices.matrix[4].m, source->matrices.matrix[4].m[3]);
    source->constVersions[62] = source->matrixVersions[1];
}

void  R_DeriveWorldViewMatrix(GfxCmdBufSourceState *source)
{
    GfxMatrix world;
    GfxViewParms *p_viewParms; // [esp+44h] [ebp-10h]

    //activeMatrices = retaddr;
    p_viewParms = &source->viewParms;
    iassert( R_IsMatrixConstantUpToDate( source, CONST_SRC_CODE_WORLD_MATRIX ) );
    memcpy(&world, (float *)source, sizeof(GfxMatrix));
    Vec3Add(world.m[3], source->eyeOffset, world.m[3]);
    MatrixMultiply44(world.m, p_viewParms->viewMatrix.m, source->matrices.matrix[12].m);
    source->constVersions[70] = source->matrixVersions[3];
}

void R_DeriveProjectionMatrix(GfxCmdBufSourceState *source)
{
    memcpy(&source->matrices.matrix[8], &source->viewParms.projectionMatrix, sizeof(source->matrices.matrix[8]));
    if (source->depthHackFlags == 2)
        source->matrices.matrix[8].m[3][2] = source->viewParms.depthHackNearClip;
    source->constVersions[66] = source->matrixVersions[2];
}

void R_DeriveViewProjectionMatrix(GfxCmdBufSourceState *source)
{
    GfxMatrix *viewProj; // [esp+14h] [ebp-10h]

    viewProj = &source->matrices.matrix[16];
    if (source->depthHackFlags == 2)
    {
        if (source->constVersions[66] != source->matrixVersions[2])
            R_DeriveProjectionMatrix(source);
        MatrixMultiply44(source->viewParms.viewMatrix.m, source->matrices.matrix[8].m, viewProj->m);
    }
    else
    {
        memcpy(viewProj, &source->viewParms.viewProjectionMatrix, sizeof(GfxMatrix));
    }
    MatrixTransformVector44(source->eyeOffset, source->matrices.matrix[16].m, source->matrices.matrix[16].m[3]);
    source->constVersions[74] = source->matrixVersions[4];
}

void  R_DeriveWorldViewProjectionMatrix(GfxCmdBufSourceState *source)
{
    float *mat20; // [esp-8h] [ebp-60h]
    GfxMatrix mat;
    GfxViewParms *p_viewParms; // [esp+48h] [ebp-10h]

    //activeMatrices = retaddr;
    p_viewParms = &source->viewParms;

    //iassert(R_IsMatrixConstantUpToDate(source, CONST_SRC_CODE_WORLD_MATRIX));

    memcpy(&mat, (float*)source, sizeof(GfxMatrix));
    mat20 = (float *)&source->matrices.matrix[20];

    if (source->depthHackFlags == 2)
    {
        if (source->constVersions[74] != source->matrixVersions[4])
            R_DeriveViewProjectionMatrix(source);
        MatrixMultiply44(mat.m, source->matrices.matrix[16].m, *(mat4x4*)mat20);
    }
    else
    {
        Vec3Add(mat.m[3], source->eyeOffset, mat.m[3]);
        MatrixMultiply44(mat.m, p_viewParms->viewProjectionMatrix.m, *(mat4x4*)mat20);
    }
    source->constVersions[78] = source->matrixVersions[5];
}

void R_DeriveShadowLookupMatrix(GfxCmdBufSourceState *source)
{
    memcpy(&source->matrices.matrix[24], &source->shadowLookupMatrix, sizeof(source->matrices.matrix[24]));
    MatrixTransformVector44(source->eyeOffset, source->matrices.matrix[24].m, source->matrices.matrix[24].m[3]);
    source->constVersions[82] = source->matrixVersions[6];
}

void  R_GenerateWorldOutdoorLookupMatrix(
    GfxCmdBufSourceState *source,
    float (*outMatrix)[4])
{
    GfxMatrix worldMatrix; // [esp-Ch] [ebp-8Ch] BYREF
    float worldOffset[4]; // [esp+34h] [ebp-4Ch] BYREF
    float zInTimesInvViewTimesOutdoorLookup[4]; // [esp+44h] [ebp-3Ch] BYREF
    float zInTimesInvView[4]; // [esp+54h] [ebp-2Ch] BYREF

    const float awayBias = r_outdoorAwayBias->current.value;
    const float downBias = r_outdoorDownBias->current.value;

    zInTimesInvView[0] = 0.0;
    zInTimesInvView[1] = 0.0;
    zInTimesInvView[2] = -awayBias;
    zInTimesInvView[3] = 0.0;

    MatrixTransformVector44(zInTimesInvView, *(const mat4x4*)R_GetCodeMatrix(source, 63u, 0), zInTimesInvViewTimesOutdoorLookup);

    zInTimesInvViewTimesOutdoorLookup[2] += downBias;

    iassert(rgp.world);

    MatrixTransformVector44(zInTimesInvViewTimesOutdoorLookup, rgp.world->outdoorLookupMatrix, worldOffset);

    //iassert(R_IsMatrixConstantUpToDate(source, CONST_SRC_CODE_WORLD_MATRIX));

    qmemcpy(&worldMatrix, source, sizeof(worldMatrix));

    Vec3Add(worldMatrix.m[3], source->eyeOffset, worldMatrix.m[3]);

    MatrixMultiply44(worldMatrix.m, rgp.world->outdoorLookupMatrix, *(mat4x4*)outMatrix);

    Vec4Add(&(*outMatrix)[12], worldOffset, &(*outMatrix)[12]);

    source->constVersions[86] = source->matrixVersions[7];
}

const GfxImage *R_GetTextureFromCode(
    GfxCmdBufSourceState *source,
    MaterialTextureSource codeTexture,
    uint8_t *samplerState)
{
    const char *v3; // eax

    bcassert(codeTexture, TEXTURE_SRC_CODE_COUNT);
    iassert( source );
    *samplerState = source->input.codeImageSamplerStates[codeTexture];
    if ((*samplerState & 7) == 0)
    {
        v3 = va("R_GetTextureFromCode %d, %d", codeTexture, *samplerState);
        MyAssertHandler(".\\r_state.cpp", 611, 0, "%s\n\t%s", "*samplerState & SAMPLER_FILTER_MASK", v3);
    }
    return source->input.codeImages[codeTexture];
}

void R_TextureFromCodeError(GfxCmdBufSourceState *source, uint32_t codeTexture)
{
    if (!rg.codeImageNames[codeTexture])
        MyAssertHandler(
            ".\\r_state.cpp",
            618,
            0,
            "%s\n\t(codeTexture) = %i",
            "(rg.codeImageNames[codeTexture])",
            codeTexture);
    Com_Error(ERR_DROP, "Tried to use '%s' when it isn't valid\n", rg.codeImageNames[codeTexture]);
}

const GfxImage *R_OverrideGrayscaleImage(const dvar_s *dvar)
{
    const char *v2; // eax

    if (dvar->current.integer == 2)
        return rgp.whiteImage;
    if (!dvar->current.integer)
        return rgp.blackImage;
    if (dvar->current.integer != 3)
    {
        v2 = va("%s = %i", dvar->name, dvar->current.integer);
        MyAssertHandler(".\\r_state.cpp", 631, 0, "%s\n\t%s", "dvar->current.integer == R_COLOR_OVERRIDE_GRAY", v2);
    }
    return rgp.grayImage;
}

void R_SetLightmap(GfxCmdBufContext context, uint32_t lmapIndex)
{
    const MaterialPass *pass; // [esp+0h] [ebp-8h]
    const GfxImage *overrideImage; // [esp+4h] [ebp-4h]

    iassert( rgp.world );
    pass = context.state->pass;
    if (lmapIndex == 31)
    {
        if ((pass->customSamplerFlags & 6) != 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                647,
                0,
                "%s",
                "!(pass->customSamplerFlags & ((1 << CUSTOM_SAMPLER_LIGHTMAP_PRIMARY) | (1 << CUSTOM_SAMPLER_LIGHTMAP_SECONDARY)))");
    }
    else
    {
        if (lmapIndex >= rgp.world->lightmapCount)
            MyAssertHandler(
                ".\\r_state.cpp",
                651,
                0,
                "lmapIndex doesn't index rgp.world->lightmapCount\n\t%i not in [0, %i)",
                lmapIndex,
                rgp.world->lightmapCount);
        if (r_lightMap->current.integer == 1)
        {
            if ((pass->customSamplerFlags & 2) != 0)
            {
                if (context.source->input.data->prim.hasSunDirChanged)
                    R_SetSampler(context, 2u, 0x62u, rgp.whiteImage);
                else
                    R_SetSampler(context, 2u, 0x62u, rgp.world->lightmaps[lmapIndex].primary);
            }
            if ((pass->customSamplerFlags & 4) != 0)
                R_SetSampler(context, 3u, 0x62u, rgp.world->lightmaps[lmapIndex].secondary);
        }
        else
        {
            overrideImage = R_OverrideGrayscaleImage(r_lightMap);
            if ((pass->customSamplerFlags & 2) != 0)
                R_SetSampler(context, 2u, 0x62u, overrideImage);
            if ((pass->customSamplerFlags & 4) != 0)
                R_SetSampler(context, 3u, 0x62u, overrideImage);
        }
    }
}

void R_SetReflectionProbe(GfxCmdBufContext context, uint32_t reflectionProbeIndex)
{
    iassert( rgp.world );
    iassert( reflectionProbeIndex != REFLECTION_PROBE_INVALID );
    if (reflectionProbeIndex >= rgp.world->reflectionProbeCount)
        MyAssertHandler(
            ".\\r_state.cpp",
            683,
            0,
            "reflectionProbeIndex doesn't index rgp.world->reflectionProbeCount\n\t%i not in [0, %i)",
            reflectionProbeIndex,
            rgp.world->reflectionProbeCount);
    iassert( rgp.world->reflectionProbes[reflectionProbeIndex].reflectionImage );
    if ((context.state->pass->customSamplerFlags & 1) != 0)
        R_SetSampler(context, 1u, 0x72u, rgp.world->reflectionProbes[reflectionProbeIndex].reflectionImage);
}

void R_ChangeDepthRange(GfxCmdBufState *state, GfxDepthRangeType depthRangeType)
{
    float v2; // [esp+8h] [ebp-28h]
    float v3; // [esp+Ch] [ebp-24h]
    IDirect3DDevice9 *device; // [esp+2Ch] [ebp-4h]

    if (state->depthRangeType == depthRangeType)
        MyAssertHandler(
            ".\\r_state.cpp",
            707,
            1,
            "state->depthRangeType != depthRangeType\n\t%i, %i",
            state->depthRangeType,
            depthRangeType);
    state->depthRangeType = depthRangeType;
    if (depthRangeType)
        v3 = 0.0;
    else
        v3 = 0.015625;
    state->depthRangeNear = v3;
    if (depthRangeType)
        v2 = 0.015625;
    else
        v2 = 1.0;
    state->depthRangeFar = v2;
#ifdef KISAK_GFX_GXM
    // The view model (the gun) in a depth pass: a sliver just past its own
    // range. Its lit pass (0 to 1/64) then always passes the depth test, while
    // the scene behind it (1/64 to 1) is still hidden. With its own range the
    // depth pass and the lit pass, two programs the Vita's compiler builds
    // apart, wrote depths a hair apart in the narrow range, and parts of the
    // gun failed the test and showed black (console run T). D3D9 wants the
    // far end past the near one (PC run Z17).
    if (depthRangeType != GFX_DEPTH_RANGE_SCENE &&
        (state->techType == TECHNIQUE_DEPTH_PREPASS || state->techType == TECHNIQUE_BUILD_FLOAT_Z))
    {
        state->depthRangeNear = state->depthRangeFar;
        state->depthRangeFar = state->depthRangeNear + 1.0f / 65536.0f;
    }
#endif
    device = state->prim.device;
    iassert( device );
    R_HW_SetViewport(device, &state->viewport, state->depthRangeNear, state->depthRangeFar);
}

int R_BeginMaterial(GfxCmdBufState *state, const Material *material, MaterialTechniqueType techType)
{
    const char *v4; // eax
    const char *v5; // eax
    const MaterialTechnique *technique; // [esp+8h] [ebp-4h]

    iassert( material );
    technique = Material_GetTechnique(material, techType);
    if (!technique)
        return 0;
    if (r_logFile->current.integer)
    {
        v4 = RB_LogTechniqueType(techType);
        v5 = va("R_BeginMaterial( %s, %s, %s )\n", material->info.name, technique->name, v4);
        RB_LogPrint(v5);
    }
    state->material = material;
    state->techType = techType;
    state->technique = technique;
    return 1;
}

void R_ClearAllStreamSources(GfxCmdBufPrimState *state)
{
    if (state->streams[0].vb || state->streams[0].offset || state->streams[0].stride)
        R_ChangeStreamSource(state, 0, 0, 0, 0);
    if (state->streams[1].vb || state->streams[1].offset || state->streams[1].stride)
        R_ChangeStreamSource(state, 1u, 0, 0, 0);
}

void R_DrawIndexedPrimitive(GfxCmdBufPrimState *state, const GfxDrawPrimArgs *args)
{
    int triCount; // [esp+4h] [ebp-8h]
    IDirect3DDevice9 *device; // [esp+8h] [ebp-4h]

    triCount = args->triCount;
    if (triCount >= r_drawPrimFloor->current.integer
        && (!r_drawPrimCap->current.integer || triCount <= r_drawPrimCap->current.integer))
    {
        if (r_skipDrawTris->current.enabled)
            triCount = 1;
        device = state->device;
        iassert( device );
        RB_TrackDrawPrimCall(triCount);
        R_HW_DrawIndexedPrimitive(device, args, triCount);
    }
}

void R_ChangeState_0(GfxCmdBufState *state, uint32_t stateBits0)
{
    bool blendOpRgbWasEnabled; // [esp+2Fh] [ebp-Dh]
    int changedBits; // [esp+30h] [ebp-Ch]
    IDirect3DDevice9 *device; // [esp+38h] [ebp-4h]

    changedBits = state->activeStateBits[0] ^ stateBits0;
    if (changedBits || ((state->refStateBits[0] ^ stateBits0) & 0x7000700) != 0)
    {
        if (r_logFile->current.integer)
            RB_LogPrintState_0(stateBits0, changedBits);
        iassert( dx.d3d9 && dx.device );
        device = state->prim.device;
        iassert( device );
        if ((changedBits & 0x18000000) != 0)
            R_HW_SetColorMask(device, stateBits0);
        if ((changedBits & 0x800) != 0)
            R_HW_SetAlphaTestEnable(state->prim.device, stateBits0);
        if ((stateBits0 & 0x800) != 0)
        {
            iassert( (stateBits0 & GFXS0_ATEST_MASK) == 0 );
            stateBits0 |= state->activeStateBits[0] & 0x3000;
            changedBits &= 0xFFFFCFFF;
            iassert( (stateBits0 ^ state->activeStateBits[0]) == changedBits );
        }
        if ((changedBits & 0x3000) != 0)
            R_SetAlphaTestFunction(state, stateBits0);
        if ((changedBits & 0xC000) != 0)
            R_HW_SetCullFace(device, stateBits0);
        if (changedBits < 0)
            R_HW_SetPolygonMode(device, stateBits0);
        blendOpRgbWasEnabled = (state->refStateBits[0] & 0x700) != 0;
        if ((stateBits0 & 0x700) != 0)
        {
            if ((stateBits0 & 0x7000000) == 0)
            {
                stateBits0 = stateBits0 & 0xF800FFFF | ((stateBits0 & 0x7FF) << 16);
                changedBits = changedBits & 0xF800FFFF | (state->activeStateBits[0] ^ stateBits0) & 0x7FF0000;
                iassert( (stateBits0 ^ state->activeStateBits[0]) == changedBits );
            }
            R_HW_SetBlend(device, blendOpRgbWasEnabled, changedBits, stateBits0);
        }
        else
        {
            if ((stateBits0 & 0x7000000) != 0)
                MyAssertHandler(
                    ".\\r_state.cpp",
                    883,
                    0,
                    "%s",
                    "(stateBits0 & GFXS0_BLENDOP_ALPHA_MASK) == (GFXS_BLENDOP_DISABLED << GFXS0_BLENDOP_ALPHA_SHIFT)");
            stateBits0 = stateBits0 & 0xF800F800 | state->activeStateBits[0] & 0x7FF07FF;
            changedBits &= 0xF800F800;
            iassert( (stateBits0 ^ state->activeStateBits[0]) == changedBits );
            if (blendOpRgbWasEnabled)
                R_HW_DisableBlend(device);
        }
        if (gfxMetrics.hasTransparencyMsaa && r_aaAlpha->current.integer && (changedBits & 0xF00) != 0)
            R_SetAlphaAntiAliasingState(device, stateBits0);
        state->activeStateBits[0] = stateBits0;
    }
}

void R_ChangeState_1(GfxCmdBufState *state, uint32_t stateBits1)
{
    int changedBits; // [esp+30h] [ebp-Ch]
    IDirect3DDevice9 *device; // [esp+38h] [ebp-4h]

    changedBits = state->activeStateBits[1] ^ stateBits1;
    if (changedBits)
    {
        if (!(stateBits1 & 0x40 | ((stateBits1 & 0x80) == 0)))
            MyAssertHandler(
                ".\\r_state.cpp",
                937,
                0,
                "%s",
                "!(stateBits1 & GFXS1_STENCIL_BACK_ENABLE) | (stateBits1 & GFXS1_STENCIL_FRONT_ENABLE)");
        if (r_logFile->current.integer)
            RB_LogPrintState_1(stateBits1, changedBits);
        iassert( dx.d3d9 && dx.device );
        device = state->prim.device;
        iassert( device );
        if ((changedBits & 1) != 0)
            R_HW_SetDepthWriteEnable(device, stateBits1);
        if ((changedBits & 2) != 0)
            R_HW_SetDepthTestEnable(device, stateBits1);
        if ((stateBits1 & 2) != 0)
        {
            iassert( (stateBits1 & GFXS1_DEPTHTEST_MASK) == 0 );
            stateBits1 |= state->activeStateBits[1] & 0xC;
            changedBits &= 0xFFFFFFF3;
            iassert( (stateBits1 ^ state->activeStateBits[1]) == changedBits );
        }
        if ((changedBits & 0xC) != 0)
            R_HW_SetDepthTestFunction(device, stateBits1);
        if ((changedBits & 0x30) != 0)
            R_ForceSetPolygonOffset(device, stateBits1);
        if ((stateBits1 & 0x40) != 0)
        {
            if ((changedBits & 0x40) != 0)
                R_HW_EnableStencil(device);
        }
        else
        {
            if ((changedBits & 0x40) != 0)
                R_HW_DisableStencil(device);
            stateBits1 = stateBits1 & 0x7F | state->activeStateBits[1] & 0xFFFFFF80;
            changedBits &= 0x7Fu;
        }
        if ((stateBits1 & 0x80) == 0)
        {
            stateBits1 = stateBits1 & 0xFFFFF | ((stateBits1 & 0xFFF00) << 12);
            changedBits = state->activeStateBits[1] ^ stateBits1;
        }
        if ((changedBits & 0x1FF00) != 0)
            R_HW_SetFrontStencilOp(device, (stateBits1 >> 8) & 7, (stateBits1 >> 11) & 7, (stateBits1 >> 14) & 7);
        if ((changedBits & 0x1FF00000) != 0)
            R_HW_SetBackStencilOp(device, (stateBits1 >> 20) & 7, (stateBits1 >> 23) & 7, (stateBits1 >> 26) & 7);
        if ((changedBits & 0xE0000) != 0)
            R_HW_SetFrontStencilFunc(device, (stateBits1 >> 17) & 7);
        if ((changedBits & 0xE0000000) != 0)
            R_HW_SetBackStencilFunc(device, stateBits1 >> 29);
        state->activeStateBits[1] = stateBits1;
    }
}

void R_SetSampler(
    GfxCmdBufContext context,
    uint32_t samplerIndex,
    uint8_t samplerState,
    const GfxImage *image)
{
    uint32_t decodedSamplerState; // [esp+Ch] [ebp-4h]

    iassert(image);
    if (context.state->samplerTexture[samplerIndex] != &image->texture)
    {
        context.state->samplerTexture[samplerIndex] = &image->texture;
        if (r_logFile->current.integer)
        {
            RB_LogPrint(va("---------- texture %i: %s\n", samplerIndex, image->name));
        }
        R_HW_SetSamplerTexture(context.state->prim.device, samplerIndex, &image->texture);
    }
    iassert((samplerState & (SAMPLER_FILTER_MASK | SAMPLER_MIPMAP_MASK)) != 0);
    if (context.state->refSamplerState[samplerIndex] != samplerState)
    {
        context.state->refSamplerState[samplerIndex] = samplerState;
        decodedSamplerState = R_DecodeSamplerState(samplerState);
        if (context.state->samplerState[samplerIndex] != decodedSamplerState)
            context.state->samplerState[samplerIndex] = R_HW_SetSamplerState(
                context.state->prim.device,
                samplerIndex,
                decodedSamplerState,
                context.state->samplerState[samplerIndex]);
    }
}

uint32_t R_DecodeSamplerState(uint8_t samplerState)
{
    uint32_t tableIndex; // [esp+0h] [ebp-8h]

    tableIndex = samplerState & 0x1F;
    bcassert(tableIndex, ARRAY_COUNT(s_decodeSamplerFilterState));

    return s_decodeSamplerFilterState[tableIndex]
        | 0x1500000
            | ((samplerState & 0x20) << 16)
            | ((samplerState & 0x40) << 17)
            | ((samplerState & 0x80) << 18);
}

void R_SetSamplerState(GfxCmdBufState *state, uint32_t samplerIndex, uint8_t samplerState)
{
    uint32_t decodedSamplerState; // [esp+8h] [ebp-4h]

    state->samplerTexture[samplerIndex] = 0;
    if ((samplerState & 0x1F) == 0)
        MyAssertHandler(
            ".\\r_state.cpp",
            1071,
            0,
            "%s\n\t(samplerState) = %i",
            "((samplerState & (SAMPLER_FILTER_MASK | SAMPLER_MIPMAP_MASK)) != 0)",
            samplerState);
    if (state->refSamplerState[samplerIndex] != samplerState)
    {
        state->refSamplerState[samplerIndex] = samplerState;
        decodedSamplerState = R_DecodeSamplerState(samplerState);
        if (state->samplerState[samplerIndex] != decodedSamplerState)
            state->samplerState[samplerIndex] = R_HW_SetSamplerState(
                state->prim.device,
                samplerIndex,
                decodedSamplerState,
                state->samplerState[samplerIndex]);
    }
}

void R_ForceSetBlendState(IDirect3DDevice9 *device, uint32_t stateBits0)
{
    if ((stateBits0 & 0x700) != 0)
        R_HW_SetBlend(device, 0, 0xFFFFFFFF, stateBits0);
    else
        R_HW_DisableBlend(device);
}

void R_ForceSetStencilState(IDirect3DDevice9 *device, uint32_t stateBits1)
{
    if ((stateBits1 & 0x40) != 0)
    {
        R_HW_EnableStencil(device);
        if ((stateBits1 & 0x80) == 0)
            stateBits1 = stateBits1 & 0xFFFFF | ((stateBits1 & 0xFFF00) << 12);
        R_HW_SetFrontStencilOp(device, (stateBits1 >> 8) & 7, (stateBits1 >> 11) & 7, (stateBits1 >> 14) & 7);
        R_HW_SetBackStencilOp(device, (stateBits1 >> 20) & 7, (stateBits1 >> 23) & 7, (stateBits1 >> 26) & 7);
        R_HW_SetFrontStencilFunc(device, (stateBits1 >> 17) & 7);
        R_HW_SetBackStencilFunc(device, stateBits1 >> 29);
    }
    else
    {
        R_HW_DisableStencil(device);
    }
}

void R_GetViewport(GfxCmdBufSourceState *source, GfxViewport *outViewport)
{
    int v2; // [esp+0h] [ebp-10h]
    int v3; // [esp+4h] [ebp-Ch]

    iassert( source );
    if (source->viewportBehavior == GFX_USE_VIEWPORT_FULL)
    {
        if (source->renderTargetWidth <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1167,
                0,
                "%s\n\t(source->renderTargetWidth) = %i",
                "(source->renderTargetWidth > 0)",
                source->renderTargetWidth);
        if (source->renderTargetHeight <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1168,
                0,
                "%s\n\t(source->renderTargetHeight) = %i",
                "(source->renderTargetHeight > 0)",
                source->renderTargetHeight);
        outViewport->x = 0;
        outViewport->y = 0;
        outViewport->width = source->renderTargetWidth;
        outViewport->height = source->renderTargetHeight;
    }
    else
    {
        if (source->sceneViewport.width <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1177,
                0,
                "%s\n\t(source->sceneViewport.width) = %i",
                "(source->sceneViewport.width > 0)",
                source->sceneViewport.width);
        if (source->sceneViewport.height <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1178,
                0,
                "%s\n\t(source->sceneViewport.height) = %i",
                "(source->sceneViewport.height > 0)",
                source->sceneViewport.height);
        *outViewport = source->sceneViewport;
        if (outViewport->width <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1181,
                0,
                "%s\n\t(outViewport->width) = %i",
                "(outViewport->width > 0)",
                outViewport->width);
        if (outViewport->height <= 0)
            MyAssertHandler(
                ".\\r_state.cpp",
                1182,
                0,
                "%s\n\t(outViewport->height) = %i",
                "(outViewport->height > 0)",
                outViewport->height);
    }
    if (source->viewMode != VIEW_MODE_2D && r_scaleViewport->current.value != 1.0)
    {
        outViewport->x += (int)((double)outViewport->width * (1.0 - r_scaleViewport->current.value) * 0.5);
        outViewport->y += (int)((double)outViewport->height * (1.0 - r_scaleViewport->current.value) * 0.5);
        if ((int)(r_scaleViewport->current.value * (double)outViewport->width) > 1)
            v3 = (int)(r_scaleViewport->current.value * (double)outViewport->width);
        else
            v3 = 1;
        outViewport->width = v3;
        if ((int)(r_scaleViewport->current.value * (double)outViewport->height) > 1)
            v2 = (int)(r_scaleViewport->current.value * (double)outViewport->height);
        else
            v2 = 1;
        outViewport->height = v2;
    }
}

void R_SetViewport(GfxCmdBufState *state, const GfxViewport *viewport)
{
    const char *v2; // eax

    if (r_logFile->current.integer)
    {
        v2 = va("Viewport at (%i, %i) with size %i x %i\n", viewport->x, viewport->y, viewport->width, viewport->height);
        RB_LogPrint(v2);
    }
    iassert( (viewport->x >= 0) );
    iassert( (viewport->y >= 0) );
    iassert( (viewport->width > 0) );
    if (viewport->height <= 0)
        MyAssertHandler(
            ".\\r_state.cpp",
            1202,
            0,
            "%s\n\t(viewport->height) = %i",
            "(viewport->height > 0)",
            viewport->height);
    if (viewport->x != state->viewport.x
        || viewport->y != state->viewport.y
        || viewport->width != state->viewport.width
        || viewport->height != state->viewport.height)
    {
        state->viewport = *viewport;
        R_HW_SetViewport(state->prim.device, viewport, state->depthRangeNear, state->depthRangeFar);
    }
}





void R_UpdateViewport(GfxCmdBufSourceState *source, GfxViewport *viewport)
{
    float lookupScale[2]; // v7, v8 [esp+34h] [ebp-28h]
    float invWidth; // [esp+3Ch] [ebp-20h]
    float invHeight; // [esp+40h] [ebp-1Ch]
    float lookupOffset[2]; // [esp+54h] [ebp-8h]

    iassert( source );
    iassert( source->viewMode != VIEW_MODE_NONE );

    source->viewportIsDirty = 0;

    iassert(source->renderTargetWidth > 0);
    iassert(source->renderTargetHeight > 0);

    invWidth = 1.0 / (double)source->renderTargetWidth;
    invHeight = 1.0 / (double)source->renderTargetHeight;

    lookupScale[0] = 0.5 * invWidth * (double)viewport->width;
    lookupScale[1] = 0.5 * invHeight * (double)viewport->height;

    lookupOffset[0] = lookupScale[0] + (invWidth * (double)viewport->x);
    lookupOffset[1] = lookupScale[1] + (invHeight * (double)viewport->y);

    lookupOffset[0] += invWidth * 0.5;
    lookupOffset[1] += invHeight * 0.5;

    R_SetCodeConstant(source,
        CONST_SRC_CODE_RENDER_TARGET_SIZE,
        (float)source->renderTargetWidth,
        (float)source->renderTargetHeight,
        invWidth,
        invHeight
    );
    R_SetCodeConstant(source,
        CONST_SRC_CODE_CLIP_SPACE_LOOKUP_SCALE,
        lookupScale[0],
        -lookupScale[1],
        0.0,
        1.0
    );
    R_SetCodeConstant(source,
        CONST_SRC_CODE_CLIP_SPACE_LOOKUP_OFFSET,
        lookupOffset[0],
        lookupOffset[1],
        0.0,
        0.0
    );
}

void R_DisableSampler(GfxCmdBufState *state, uint32_t samplerIndex)
{
    state->samplerTexture[samplerIndex] = 0;
    R_HW_DisableSampler(state->prim.device, samplerIndex);
}

void R_UnbindImage(GfxCmdBufState *state, const GfxImage *image)
{
    uint32_t samplerIndex; // [esp+0h] [ebp-4h]

    for (samplerIndex = 0; samplerIndex < vidConfig.maxTextureMaps; ++samplerIndex)
    {
        if (state->samplerTexture[samplerIndex] == &image->texture)
            R_DisableSampler(state, samplerIndex);
    }
}

bool g_renderTargetIsOverridden;
void R_SetRenderTargetSize(GfxCmdBufSourceState *source, GfxRenderTargetId newTargetId)
{
    GfxRenderTargetId actualTargetId; // [esp+0h] [ebp-4h]

    iassert(source);
    iassert(gfxRenderTargets);

    g_renderTargetIsOverridden = 0;
    if (pixelCostMode > GFX_PIXEL_COST_MODE_MEASURE_MSEC)
    {
        actualTargetId = RB_PixelCost_OverrideRenderTarget(newTargetId);
        g_renderTargetIsOverridden = newTargetId != actualTargetId;
        newTargetId = actualTargetId;
    }
    bcassert(newTargetId, R_RENDERTARGET_COUNT);
    source->viewportBehavior = R_ViewportBehaviorForRenderTarget(newTargetId);
    source->renderTargetWidth = gfxRenderTargets[newTargetId].width;
    source->renderTargetHeight = gfxRenderTargets[newTargetId].height;
    iassert(source->renderTargetWidth > 0);
    iassert(source->renderTargetHeight > 0);
}

const GfxViewportBehavior s_viewportBehaviorForRenderTarget[15] =
{
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FULL,
  GFX_USE_VIEWPORT_FOR_VIEW,
  GFX_USE_VIEWPORT_FOR_VIEW
}; // idb

GfxViewportBehavior R_ViewportBehaviorForRenderTarget(GfxRenderTargetId renderTargetId)
{
    iassert(s_viewportBehaviorForRenderTarget);
    bcassert(renderTargetId, R_RENDERTARGET_COUNT);

#ifdef KISAK_RADIANT
    // FAITHFUL to the CoD4Radiant editor: its R_SetRenderTarget (IDB 0x5397a0) and
    // R_SetupRenderTarget (IDB 0x539670) compute viewportBehavior INLINE as
    // `id != SHADOWMAP_SUN && id != SHADOWMAP_SPOT` — i.e. every target except the two shadowmaps
    // is FULL (FRAME_BUFFER included). The s_viewportBehaviorForRenderTarget table below is the
    // CoD3 *game* variant (FRAME_BUFFER/SCENE = FOR_VIEW); used in the editor it made the plain-2D
    // texture browser read its viewport from the stale sceneViewport (rendered into a small
    // top-left rectangle). Use the editor's inline rule so FRAME_BUFFER resolves to FULL.
    return (renderTargetId != R_RENDERTARGET_SHADOWMAP_SUN && renderTargetId != R_RENDERTARGET_SHADOWMAP_SPOT)
               ? GFX_USE_VIEWPORT_FULL
               : GFX_USE_VIEWPORT_FOR_VIEW;
#else
    return s_viewportBehaviorForRenderTarget[renderTargetId];
#endif
}

void R_SetRenderTarget(GfxCmdBufContext context, GfxRenderTargetId newTargetId)
{
    if (pixelCostMode > GFX_PIXEL_COST_MODE_MEASURE_MSEC)
        newTargetId = RB_PixelCost_OverrideRenderTarget(newTargetId);

    if (newTargetId != context.state->renderTargetId)
    {
        if (r_logFile->current.integer)
        {
            RB_LogPrint(va("\n========== R_SetRenderTarget( %s ) ==========\n\n", R_RenderTargetName(newTargetId)));
        }
        R_UpdateStatsTarget(newTargetId);

        if (gfxRenderTargets[newTargetId].image)
            R_UnbindImage(context.state, gfxRenderTargets[newTargetId].image);

        iassert(context.source->viewportBehavior == R_ViewportBehaviorForRenderTarget(newTargetId));
        iassert(context.source->renderTargetHeight == (int)gfxRenderTargets[newTargetId].height);
        iassert(context.source->renderTargetWidth > 0);
        iassert(context.source->renderTargetHeight > 0);

        R_HW_SetRenderTarget(context.state, newTargetId);
        context.state->renderTargetId = newTargetId;
        context.source->viewMode = VIEW_MODE_NONE;
        context.source->viewportIsDirty = 1;

        // LWSS: if this assert() goes off, it means that R_RENDERTARGET_SHADOWMAP_SUN is the new renderTargetId (Set in R_AddSpotShadowsForLight() - path with sm_qualitySpotShadow)
        iassert(context.source->renderTargetWidth == (int)gfxRenderTargets[newTargetId].width);
    }
}

void R_UpdateStatsTarget(int newTargetId)
{
    if (newTargetId == R_RENDERTARGET_SHADOWCOOKIE || newTargetId == R_RENDERTARGET_DYNAMICSHADOWS)
        g_viewStats = &g_frameStatsCur.viewStats[1];
    else
        g_viewStats = &g_frameStatsCur.viewStats[0];
}

void R_ClearScreen(
    IDirect3DDevice9 *device,
    uint8_t whichToClear,
    const float *color,
    float depth,
    uint8_t stencil,
    const GfxViewport *viewport)
{
    iassert( whichToClear );
    if ((whichToClear & 0xF8) != 0)
        MyAssertHandler(
            ".\\r_state.cpp",
            1485,
            0,
            "%s\n\t(whichToClear) = %i",
            "((whichToClear & ~(0x00000001l | 0x00000002l | 0x00000004l)) == 0)",
            whichToClear);
    iassert( color );
    iassert( (depth >= 0.0f && depth <= 1.0f) );
    if (pixelCostMode <= GFX_PIXEL_COST_MODE_MEASURE_MSEC || (whichToClear &= ~1u) != 0)
        R_ClearScreenInternal(device, whichToClear, color, depth, stencil, viewport);
}

void R_ForceSetPolygonOffset(IDirect3DDevice9 *device, char stateBits1)
{
    __int64 v2; // [esp+10h] [ebp-24h]
    uint32_t offset; // [esp+28h] [ebp-Ch]
    float bias; // [esp+2Ch] [ebp-8h]
    float scale; // [esp+30h] [ebp-4h]

    offset = stateBits1 & 0x30;
    if (offset == 48)
    {
        bias = sm_polygonOffsetBias->current.value * 0.0000152587890625;
        scale = sm_polygonOffsetScale->current.value;
    }
    else
    {
        v2 = offset >> 4;
        bias = (double)v2 * r_polygonOffsetBias->current.value * 0.0000152587890625;
        scale = (double)v2 * r_polygonOffsetScale->current.value;
    }
    R_HW_SetPolygonOffset(device, scale, bias);
}

void R_SetMeshStream(GfxCmdBufState *state, GfxMeshData *mesh)
{
    R_SetStreamSource(&state->prim, mesh->vb.buffer, 0, mesh->vertSize);
}

void R_SetCompleteState(IDirect3DDevice9 *device, uint32_t *stateBits)
{
    iassert(device);
    R_HW_SetColorMask(device, stateBits[0]);
    R_HW_SetAlphaTestEnable(device, stateBits[0]);
    iassert(stateBits[0] & GFXS0_ATEST_DISABLE);
    R_HW_SetCullFace(device, stateBits[0]);
    R_HW_SetPolygonMode(device, stateBits[0]);
    R_ForceSetBlendState(device, stateBits[0]);
    R_HW_SetDepthWriteEnable(device, stateBits[1]);
    R_HW_SetDepthTestEnable(device, stateBits[1]);
    R_HW_SetDepthTestFunction(device, stateBits[1]);
    R_ForceSetPolygonOffset(device, stateBits[1]);
    R_ForceSetStencilState(device, stateBits[1]);
}

void R_InitLocalCmdBufState(GfxCmdBufState *state)
{
    memcpy(state, &gfxCmdBufState, sizeof(GfxCmdBufState));
    memset(state->vertexShaderConstState, 0, sizeof(state->vertexShaderConstState));
    memset(state->pixelShaderConstState, 0,  sizeof(state->pixelShaderConstState));
}

void R_DrawCall(
    DrawCallCallback callback,
    const void* userData,
    GfxCmdBufSourceState* source,
    const GfxViewInfo* viewInfo,
    const GfxDrawSurfListInfo* info,
    const GfxViewParms* viewParms,
    GfxCmdBuf* cmdBufEA,
    GfxCmdBuf* prepassCmdBufEA)
{
    GfxCmdBufState cmdBuf;
    GfxCmdBufState prepassCmdBuf;
    GfxCmdBufContext context;
    GfxCmdBufContext prepassContext;

    context.source = source;
    context.state = &cmdBuf;

    R_BeginView(source, &viewInfo->sceneDef, viewParms);

    R_InitLocalCmdBufState(&cmdBuf);

    if (prepassCmdBufEA)
    {
        prepassContext.source = source;
        prepassContext.state = &prepassCmdBuf;

        R_InitLocalCmdBufState(&prepassCmdBuf);

        callback(userData, context, prepassContext);
        memcpy(&gfxCmdBufState, &prepassCmdBuf, sizeof(gfxCmdBufState));
    }
    else
    {
        prepassContext.source = NULL;
        prepassContext.state = NULL;
        callback(userData, context, prepassContext);
    }
    memcpy(&gfxCmdBufState, &cmdBuf, sizeof(gfxCmdBufState));
}

void R_SetCodeConstant(GfxCmdBufSourceState *source, CodeConstant constant, float x, float y, float z, float w)
{
    bcassert(constant, CONST_SRC_CODE_COUNT_FLOAT4);

    source->input.consts[constant][0] = x;
    source->input.consts[constant][1] = y;
    source->input.consts[constant][2] = z;
    source->input.consts[constant][3] = w;

    R_DirtyCodeConstant(source, constant);
}

void R_SetCodeConstantFromVec4(GfxCmdBufSourceState *source, CodeConstant constant, float *value)
{
    float *v3; // [esp+0h] [ebp-4h]

    iassert(constant < CONST_SRC_CODE_COUNT_FLOAT4);

    v3 = source->input.consts[constant];
    v3[0] = value[0];
    v3[1] = value[1];
    v3[2] = value[2];
    v3[3] = value[3];
    R_DirtyCodeConstant(source, constant);
}

void R_DirtyCodeConstant(GfxCmdBufSourceState *source, CodeConstant constant)
{
    iassert(constant < ARRAY_COUNT(source->constVersions));

    ++source->constVersions[constant];
}

void R_UpdateCodeConstant(
    GfxCmdBufSourceState *source,
    CodeConstant constant,
    float x,
    float y,
    float z,
    float w)
{
    float *v6; // [esp+0h] [ebp-8h]

    if (x != source->input.consts[constant][0]
        || y != source->input.consts[constant][1]
        || z != source->input.consts[constant][2]
        || w != source->input.consts[constant][3])
    {
        if (constant >= 0x3A)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h",
                495,
                0,
                "constant doesn't index CONST_SRC_CODE_COUNT_FLOAT4\n\t%i not in [0, %i)",
                constant,
                58);
        v6 = source->input.consts[constant];
        *v6 = x;
        v6[1] = y;
        v6[2] = z;
        v6[3] = w;
        R_DirtyCodeConstant(source, constant);
    }
}


