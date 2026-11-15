#include <universal/q_shared.h>
#include "r_reflection_probe.h"
#include "r_dvars.h"
#include <qcommon/com_bsp.h>
#include "r_bsp.h"
#include "r_screenshot.h"
#include "r_rendercmds.h"
#include <EffectsCore/fx_system.h>
#include "r_scene.h"
#include "r_dpvs.h"
#include "r_workercmds_common.h"
#include <universal/com_files.h>
#include <universal/q_parse.h>
#include "r_image.h"
#include "r_init.h"
#include "rhi/r_rhi_modern.h"
#include <win32/win_local.h>

int s_numColorCorrectionDataEntries;
ColorCorrectionData s_colorCorrectionDataEntries[1024];

char R_CopyReflectionsFromLumpData(
    DiskGfxReflectionProbe *probeRawData,
    const DiskGfxReflectionProbe *probeRawLumpData,
    int lumpProbeCount)
{
    const DiskGfxReflectionProbe *v5; // [esp+4h] [ebp-8h]
    int probeIndex; // [esp+8h] [ebp-4h]

    for (probeIndex = 0; probeIndex < lumpProbeCount; ++probeIndex)
    {
        v5 = &probeRawLumpData[probeIndex];
        if (v5->origin[0] == probeRawData->origin[0]
            && v5->origin[1] == probeRawData->origin[1]
            && v5->origin[2] == probeRawData->origin[2])
        {
            memcpy(probeRawData->pixels, probeRawLumpData[probeIndex].pixels, sizeof(probeRawData->pixels));
            return 1;
        }
    }
    return 0;
}

void R_CalcCubeMapViewValues(refdef_s *refdef, CubemapShot cubemapShot, int cubemapSize)
{
    refdef->x = 0;
    refdef->y = 0;
    refdef->width = cubemapSize + 2;
    refdef->height = cubemapSize + 2;
    refdef->tanHalfFovX = (cubemapSize + 2) / cubemapSize;
    refdef->tanHalfFovY = refdef->tanHalfFovX;
    refdef->zNear = 0.0;
    switch (cubemapShot)
    {
    case CUBEMAPSHOT_RIGHT:
        refdef->viewaxis[0][0] = 1.0;
        refdef->viewaxis[0][1] = 0.0;
        refdef->viewaxis[0][2] = 0.0;
        refdef->viewaxis[1][0] = 0.0;
        refdef->viewaxis[1][1] = 1.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = 0.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 1.0;
        break;
    case CUBEMAPSHOT_LEFT:
        refdef->viewaxis[0][0] = -1.0;
        refdef->viewaxis[0][1] = 0.0;
        refdef->viewaxis[0][2] = 0.0;
        refdef->viewaxis[1][0] = 0.0;
        refdef->viewaxis[1][1] = -1.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = 0.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 1.0;
        break;
    case CUBEMAPSHOT_FRONT:
        refdef->viewaxis[0][0] = 0.0;
        refdef->viewaxis[0][1] = -1.0;
        refdef->viewaxis[0][2] = 0.0;
        refdef->viewaxis[1][0] = 1.0;
        refdef->viewaxis[1][1] = 0.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = 0.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 1.0;
        break;
    case CUBEMAPSHOT_UP:
        refdef->viewaxis[0][0] = 0.0;
        refdef->viewaxis[0][1] = 0.0;
        refdef->viewaxis[0][2] = 1.0;
        refdef->viewaxis[1][0] = 0.0;
        refdef->viewaxis[1][1] = 1.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = -1.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 0.0;
        break;
    case CUBEMAPSHOT_DOWN:
        refdef->viewaxis[0][0] = 0.0;
        refdef->viewaxis[0][1] = 0.0;
        refdef->viewaxis[0][2] = -1.0;
        refdef->viewaxis[1][0] = 0.0;
        refdef->viewaxis[1][1] = 1.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = 1.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 0.0;
        break;
    default:
        if (cubemapShot != CUBEMAPSHOT_BACK)
            MyAssertHandler(
                ".\\r_reflection_probe.cpp",
                78,
                1,
                "%s\n\t(cubemapShot) = %i",
                "(cubemapShot == CUBEMAPSHOT_BACK)",
                cubemapShot);
        refdef->viewaxis[0][0] = 0.0;
        refdef->viewaxis[0][1] = 1.0;
        refdef->viewaxis[0][2] = 0.0;
        refdef->viewaxis[1][0] = -1.0;
        refdef->viewaxis[1][1] = 0.0;
        refdef->viewaxis[1][2] = 0.0;
        refdef->viewaxis[2][0] = 0.0;
        refdef->viewaxis[2][1] = 0.0;
        refdef->viewaxis[2][2] = 1.0;
        break;
    }
}

void R_BspGenerateReflections()
{
    R_GenerateReflections((char*)rgp.world->name, rgp.world->reflectionProbes + 1, rgp.world->reflectionProbeCount - 1);
}

void R_GenerateReflectionRawData(DiskGfxReflectionProbe *probeRawData)
{
    float zfar; // [esp+0h] [ebp-40B8h]
    FxCmd cmd; // [esp+4h] [ebp-40B4h] BYREF
    CubemapShot cubemapShot; // [esp+10h] [ebp-40A8h]
    refdef_s dst; // [esp+18h] [ebp-40A0h] BYREF

    memset(&dst, 0, sizeof(dst));
    dst.vieworg[0] = probeRawData->origin[0];
    dst.vieworg[1] = probeRawData->origin[1];
    dst.vieworg[2] = probeRawData->origin[2];
    dst.localClientNum = 0;
    dst.time = 0;
    dst.blurRadius = 0.0;
    dst.useScissorViewport = 0;

    R_InitPrimaryLights(dst.primaryLights);

    for (cubemapShot = CUBEMAPSHOT_RIGHT; cubemapShot < CUBEMAPSHOT_COUNT; ++cubemapShot)
    {
        R_BeginCubemapShot(256, 1);
        R_BeginFrame();
        R_BeginSharedCmdList();
        R_ClearClientCmdList2D();
        R_ClearScene(0);
        FX_BeginUpdate(0);
        R_CalcCubeMapViewValues(&dst, cubemapShot, 256);
        R_SetLodOrigin(&dst);
        zfar = R_GetFarPlaneDist();
        FX_SetNextUpdateCamera(0, &dst, zfar);
        FX_FillUpdateCmd(0, &cmd);
        R_UpdateSpotLightEffect(&cmd);
        R_UpdateNonDependentEffects(&cmd);
        R_UpdateRemainingEffects(&cmd);
        R_RenderScene(&dst);
        R_EndFrame();
        R_IssueRenderCommands(0xFFFFFFFF);
        R_EndCubemapShot(cubemapShot);
    }
    R_CreateReflectionRawDataFromCubemapShot(probeRawData, 64);
}

void R_GenerateReflectionRawDataAll(DiskGfxReflectionProbe *probeRawData, int probeCount, bool *generateProbe)
{
    int probeIndex; // [esp+0h] [ebp-4h]

    for (probeIndex = 0; probeIndex < probeCount; ++probeIndex)
    {
        if (generateProbe[probeIndex])
            R_GenerateReflectionRawData(&probeRawData[probeIndex]);
    }
}

const char *fields_2[6] = { "name", "black_level", "white_level", "gamma", "saturation", NULL }; // idb
char R_VerifyFieldNames(const char **buf, const char *filename)
{
    int fieldIndex; // [esp+0h] [ebp-8h]
    parseInfo_t *token; // [esp+4h] [ebp-4h]

    for (fieldIndex = 0; fields_2[fieldIndex]; ++fieldIndex)
    {
        token = Com_Parse(buf);
        if (I_stricmp(fields_2[fieldIndex], token->token))
        {
            Com_PrintError(CON_CHANNEL_ERROR, "R_VerifyFieldNames: file %s column header %d was %s instead of %s", filename, fieldIndex, token, fields_2[fieldIndex]);
            return 0;
        }
    }
    Com_SkipRestOfLine(buf);
    return 1;
}

void R_ParseColorCorrectionData(const char *buf, const char *filename)
{
    ColorCorrectionData *ccd; // [esp+10h] [ebp-8h]
    parseInfo_t *token; // [esp+14h] [ebp-4h]

    iassert( s_numColorCorrectionDataEntries == 0 );
    if (R_VerifyFieldNames(&buf, filename))
    {
        while (1)
        {
            token = Com_Parse(&buf);
            if (!buf)
                break;
            if (s_numColorCorrectionDataEntries == 1024)
            {
                Com_PrintError(CON_CHANNEL_ERROR, "R_ParseColorCorrectionData: file %s max color correction entries [%d] exceeded. Ignoring the rest of the file", filename, 1024, filename);
                return;
            }
            ccd = &s_colorCorrectionDataEntries[s_numColorCorrectionDataEntries++];
            iassert( token );
            if (strlen(token->token) >= 0x40)
                Com_PrintError(CON_CHANNEL_ERROR, "R_ParseColorCorrectionData: file %s truncating name because %s is too longer than %d", filename, token, 64);
            I_strncpyz(ccd->name, token->token, 64);
            ccd->black_level = Com_ParseFloatOnLine(&buf);
            ccd->white_level = Com_ParseFloatOnLine(&buf);
            ccd->gamma = Com_ParseFloatOnLine(&buf);
            ccd->saturation = Com_ParseFloatOnLine(&buf);
            ccd->range = ccd->white_level - ccd->black_level;
            Com_SkipRestOfLine(&buf);
        }
    }
}

void R_LoadColorCorrectionData()
{
    uint8_t *filebuf; // [esp+4h] [ebp-Ch]
    int fileSize; // [esp+8h] [ebp-8h]
    int f; // [esp+Ch] [ebp-4h] BYREF

    fileSize = FS_FOpenFileByMode((char*)"reflections/reflections.csv", &f, FS_READ);
    if (fileSize >= 0)
    {
        Hunk_CheckTempMemoryHighClear();
        filebuf = (uint8_t *)Hunk_AllocateTempMemoryHigh(fileSize + 1, "R_LoadColorCorrectionData");
        FS_Read(filebuf, fileSize, f);
        FS_FCloseFile(f);
        filebuf[fileSize] = 0;
        Com_BeginParseSession("reflections/reflections.csv");
        Com_SetCSV(1);
        R_ParseColorCorrectionData((const char*)filebuf, "reflections/reflections.csv");
        Com_EndParseSession();
        Hunk_ClearTempMemoryHigh();
    }
    else
    {
        Com_PrintError(CON_CHANNEL_ERROR, "R_LoadColorCorrectionData: failed to open %s", "reflections/reflections.csv");
    }
}

ColorCorrectionData *R_CreateDefaultColorCorrectionEntry()
{
    ColorCorrectionData *result; // eax
    ColorCorrectionData *ccd; // [esp+0h] [ebp-4h]

    iassert( s_numColorCorrectionDataEntries == 0 );
    ccd = &s_colorCorrectionDataEntries[s_numColorCorrectionDataEntries++];
    I_strncpyz(ccd->name, "default", 64);
    ccd->black_level = 0.0;
    ccd->white_level = 1.0;
    ccd->gamma = 1.0;
    ccd->saturation = 1.0;
    result = ccd;
    ccd->range = ccd->white_level - ccd->black_level;
    return result;
}

const ColorCorrectionData *R_FindColorCorrectionData(const char *name)
{
    int i; // [esp+0h] [ebp-4h]

    iassert( name );
    if (!*name)
        return R_FindColorCorrectionData("default");
    for (i = 0; i < s_numColorCorrectionDataEntries; ++i)
    {
        if (!I_stricmp(s_colorCorrectionDataEntries[i].name, name))
            return &s_colorCorrectionDataEntries[i];
    }
    Com_PrintError(CON_CHANNEL_ERROR, "R_FindColorCorrectionData: failed to find color correction entry %s. Using %s instead.", name, s_colorCorrectionDataEntries);
    return s_colorCorrectionDataEntries;
}

const float colorIntensityScale[3] = { 0.114f, 0.587f, 0.299f };

void R_ColorCorrectBGRAPixel(const ColorCorrectionData *ccd, const uint8_t *from, uint8_t *to)
{
    float v3; // [esp+18h] [ebp-64h]
    float v4; // [esp+1Ch] [ebp-60h]
    float v5; // [esp+20h] [ebp-5Ch]
    float v6; // [esp+24h] [ebp-58h]
    float v7; // [esp+28h] [ebp-54h]
    float v8; // [esp+2Ch] [ebp-50h]
    float v9; // [esp+30h] [ebp-4Ch]
    float v10; // [esp+34h] [ebp-48h]
    float v11; // [esp+3Ch] [ebp-40h]
    float v12; // [esp+40h] [ebp-3Ch]
    float v13; // [esp+44h] [ebp-38h]
    float v14; // [esp+54h] [ebp-28h]
    int m; // [esp+58h] [ebp-24h]
    int k; // [esp+5Ch] [ebp-20h]
    int j; // [esp+60h] [ebp-1Ch]
    int i; // [esp+64h] [ebp-18h]
    float intensity; // [esp+68h] [ebp-14h]
    float maxIntensity; // [esp+6Ch] [ebp-10h]
    float color[3]; // [esp+70h] [ebp-Ch] BYREF

    iassert( ccd );
    for (i = 0; i < 3; ++i)
    {
        color[i] = from[i] / 255.0;
        color[i] = (color[i] - ccd->black_level) / ccd->range;
        v14 = color[i];
        v10 = v14 - 0.0;
        if (v10 < 0.0)
            v9 = 0.0;
        else
            v9 = v14;
        color[i] = v9;
        v8 = pow(color[i], ccd->gamma);
        color[i] = v8;
    }
    intensity = Vec3Dot(color, colorIntensityScale);
    for (j = 0; j < 3; ++j)
        color[j] = ccd->saturation * color[j] + (1.0 - ccd->saturation) * intensity;
    maxIntensity = 0.1f;
    for (k = 0; k < 3; ++k)
    {
        v13 = color[k];
        v7 = 4.0 - v13;
        if (v7 < 0.0)
            v6 = 4.0;
        else
            v6 = v13;
        color[k] = v6;
        if (maxIntensity < color[k])
            maxIntensity = color[k];
    }
    for (m = 0; m < 3; ++m)
    {
        color[m] = color[m] / maxIntensity;
        v11 = color[m];
        v5 = v11 - 1.0;
        if (v5 < 0.0)
            v12 = v11;
        else
            v12 = 1.0;
        v4 = 0.0 - v11;
        if (v4 < 0.0)
            v3 = v12;
        else
            v3 = 0.0;
        color[m] = v3;
        to[m] = (color[m] * 255.0);
    }
    to[3] = (maxIntensity / 4.0 * 255.0);
}

void R_CopyBlockFromBgraToPixelColorWithColorCorrection(
    const ColorCorrectionData *colorCorrectionData,
    uint8_t *to,
    const uint8_t *from,
    uint32_t blockSize)
{
    uint32_t offset; // [esp+0h] [ebp-4h]

    iassert( blockSize );
    iassert( blockSize % 4 == 0 );
    offset = 0;
    do
    {
        R_ColorCorrectBGRAPixel(colorCorrectionData, &from[offset], &to[offset]);
        offset += 4;
    } while (offset != blockSize);
}

GfxImage *R_GenerateReflectionImageFromRawData(const uint8_t *rawPixels, int probeIndex)
{
    char *v2; // eax
    int v4; // [esp+0h] [ebp-188h]
    const uint8_t *pixels[6][15]; // [esp+8h] [ebp-180h] BYREF
    int imgIndex; // [esp+170h] [ebp-18h]
    int mipmapLevelSize; // [esp+174h] [ebp-14h]
    GfxImage *reflectionImage; // [esp+178h] [ebp-10h]
    _D3DFORMAT imageFormat; // [esp+17Ch] [ebp-Ch]
    int scaledSize; // [esp+180h] [ebp-8h]
    int mipLevel; // [esp+184h] [ebp-4h]

    imageFormat = D3DFMT_A8R8G8B8;
    mipLevel = 0;
    mipmapLevelSize = 0x4000;
    for (imgIndex = 0; imgIndex < 6; ++imgIndex)
    {
        pixels[imgIndex][0] = rawPixels;
        rawPixels += mipmapLevelSize;
    }
    for (imgIndex = 0; imgIndex < 6; ++imgIndex)
    {
        scaledSize = 64;
        mipLevel = 1;
        do
        {
            if (scaledSize >> 1 > 1)
                v4 = scaledSize >> 1;
            else
                v4 = 1;
            scaledSize = v4;
            mipmapLevelSize = v4 * 4 * v4;
            pixels[imgIndex][mipLevel] = rawPixels;
            rawPixels += mipmapLevelSize;
            ++mipLevel;
        } while (scaledSize != 1);
    }
    v2 = va("*reflection_probe%i", probeIndex);
    reflectionImage = Image_Alloc(v2, IMG_CATEGORY_AUTO_GENERATED, TS_FUNCTION, 0);
    iassert( reflectionImage );
    Image_GenerateCube(reflectionImage, pixels, 64, imageFormat, mipLevel);
    iassert(reflectionImage->texture.basemap); // lwss add
    return reflectionImage;
}

void R_GenerateReflectionImages(
    GfxReflectionProbe *probes,
    const DiskGfxReflectionProbe *probeRawData,
    int probeCount,
    int probeBaseIndex)
{
    const ColorCorrectionData *ColorCorrectionData; // eax
    GfxReflectionProbe *v5; // [esp+0h] [ebp-14h]
    const DiskGfxReflectionProbe *v6; // [esp+4h] [ebp-10h]
    uint8_t *pixels; // [esp+Ch] [ebp-8h]
    int probeIndex; // [esp+10h] [ebp-4h]

    if (!s_numColorCorrectionDataEntries)
        R_LoadColorCorrectionData();
    if (!s_numColorCorrectionDataEntries)
        R_CreateDefaultColorCorrectionEntry();
    pixels = (uint8_t *)Hunk_AllocateTempMemory(131064, "R_GenerateReflectionImages");
    for (probeIndex = 0; probeIndex < probeCount; ++probeIndex)
    {
        ColorCorrectionData = R_FindColorCorrectionData(probeRawData[probeIndex].colorCorrectionFilename);
        v5 = &probes[probeIndex];
        v6 = &probeRawData[probeIndex];
        v5->origin[0] = v6->origin[0];
        v5->origin[1] = v6->origin[1];
        v5->origin[2] = v6->origin[2];
        R_CopyBlockFromBgraToPixelColorWithColorCorrection(ColorCorrectionData, pixels, v6->pixels, 0x1FFF8u);
        v5->reflectionImage = R_GenerateReflectionImageFromRawData(pixels, probeBaseIndex + probeIndex);
    }
    Hunk_FreeTempMemory((char*)pixels);
}

bool R_ReflectionProbeGenerateExitWhenDone()
{
    return r_reflectionProbeGenerate->current.enabled && r_reflectionProbeGenerateExit->current.enabled;
}

void R_GenerateReflections(char *mapname, GfxReflectionProbe *probes, uint32_t probeCount)
{
    char v3; // al
    DiskGfxReflectionProbe *v4; // [esp+8h] [ebp-124h]
    GfxReflectionProbe *v5; // [esp+Ch] [ebp-120h]
    uint32_t lumpProbeCount; // [esp+10h] [ebp-11Ch] BYREF
    uint32_t version; // [esp+14h] [ebp-118h]
    const DiskGfxReflectionProbe *probeRawLumpData; // [esp+18h] [ebp-114h]
    DiskGfxReflectionProbe *probeRawGeneratedData; // [esp+1Ch] [ebp-110h]
    uint32_t probeIndex; // [esp+20h] [ebp-10Ch]
    bool generateProbe[256]; // [esp+24h] [ebp-108h] BYREF
    uint32_t lumpSize; // [esp+128h] [ebp-4h]

    iassert( r_reflectionProbeGenerate );
    iassert( probeCount < MAX_MAP_REFLECTION_PROBES );
    if (r_reflectionProbeGenerate->current.enabled)
    {
        Com_LoadBsp(mapname);
        version = Com_GetBspVersion();
        if (version != 22)
            Com_Error(
                ERR_DROP,
                "You can only generate reflections for BSP version %i, but the BSP is version %i.  You need to recompile the map.",
                22,
                version);
        probeRawLumpData = (DiskGfxReflectionProbe*)Com_GetBspLump(LUMP_REFLECTION_PROBES, 0x20044u, &lumpProbeCount);
        if (probeCount != lumpProbeCount)
            MyAssertHandler(
                ".\\r_reflection_probe.cpp",
                189,
                0,
                "probeCount == lumpProbeCount\n\t%i, %i",
                probeCount,
                lumpProbeCount);
        if (probeCount)
        {
            lumpSize = 131140 * probeCount;
            probeRawGeneratedData = (DiskGfxReflectionProbe*)Z_Malloc(131140 * probeCount, "R_GenerateReflections", 0);
            for (probeIndex = 0; probeIndex < lumpProbeCount; ++probeIndex)
            {
                qmemcpy(
                    probeRawGeneratedData[probeIndex].colorCorrectionFilename,
                    probeRawLumpData[probeIndex].colorCorrectionFilename,
                    sizeof(probeRawGeneratedData[probeIndex].colorCorrectionFilename));
                v4 = &probeRawGeneratedData[probeIndex];
                v5 = &probes[probeIndex];
                v4->origin[0] = v5->origin[0];
                v4->origin[1] = v5->origin[1];
                v4->origin[2] = v5->origin[2];
                if (r_reflectionProbeRegenerateAll->current.enabled)
                {
                    generateProbe[probeIndex] = 1;
                }
                else
                {
                    v3 = R_CopyReflectionsFromLumpData(&probeRawGeneratedData[probeIndex], probeRawLumpData, probeCount);
                    generateProbe[probeIndex] = v3 == 0;
                }
            }
            R_GenerateReflectionRawDataAll(probeRawGeneratedData, lumpProbeCount, generateProbe);
            Com_SaveLump(LUMP_REFLECTION_PROBES, probeRawGeneratedData, lumpSize, COM_SAVE_LUMP_AND_CLOSE);
            R_GenerateReflectionImages(probes, probeRawGeneratedData, probeCount, 0);
            Z_Free(probeRawGeneratedData, 0);
        }
        else
        {
            Com_SaveLump(LUMP_REFLECTION_PROBES, 0, 0, COM_SAVE_LUMP_AND_CLOSE);
        }
        if (R_ReflectionProbeGenerateExitWhenDone())
        {
            Sys_NormalExit();
            exit(0);
        }
        Dvar_SetBool(r_reflectionProbeGenerate, 0);
    }
}
// ---- The Modern path's own probes (GfxModernHooks::probeCaptureWanted) ----------------
//
// docs/vulkan-modern.md "Reflection probes". Once a world, after its first
// frames of play, each of the world's probes (index 0 is the default probe,
// not one of the map's) is drawn again from its origin: six square views of
// 90 degrees, one along each face of the cube, through the Modern path with
// the back end reading each scene's light back (linear, before bloom,
// exposure and the tonemap). Each texel of the probe's first level is the
// light the views saw along its direction (two by two of the views' pixels),
// stored in the game's encoding; the back end then makes the blurrier levels
// again from it (GfxModernHooks::reflectionProbe).

static bool s_modernProbeCapturing;
// Whether the world loaded last still has its probes to draw.
static bool s_modernProbesPending;

bool R_ModernProbeCaptureActive() { return s_modernProbeCapturing; }

void R_ModernProbesLoaded() { s_modernProbesPending = true; }

// The direction through (u, v) of a face of a D3D9 cube (+X, -X, +Y, -Y, +Z,
// -Z), u and v from -1 to 1 across and down it; the game reads its probes
// with world directions. Not normalised.
static void R_ModernCubeDirection(int face, float u, float v, float *d)
{
    switch (face)
    {
    case 0: d[0] = 1.0f; d[1] = -v; d[2] = -u; break;
    case 1: d[0] = -1.0f; d[1] = -v; d[2] = u; break;
    case 2: d[0] = u; d[1] = 1.0f; d[2] = v; break;
    case 3: d[0] = u; d[1] = -1.0f; d[2] = -v; break;
    case 4: d[0] = u; d[1] = -v; d[2] = 1.0f; break;
    default: d[0] = -u; d[1] = -v; d[2] = -1.0f; break;
    }
}

static float R_ModernEncodeSrgb(float light)
{
    const float v = light > 0.0f ? light : 0.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static float R_ModernDecodeSrgb(float value)
{
    const float v = value > 0.0f ? value : 0.0f;
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

// The game's encoding of a probe texel (R_ColorCorrectBGRAPixel): the display
// value's rgb over its largest channel (at least 0.1, at most 4), alpha that
// channel over 4. BGRA.
static void R_ModernStoreProbeTexel(const float *light, uint8_t *texel)
{
    float value[3];
    float largest = 0.1f;
    for (int c = 0; c < 3; ++c)
    {
        value[c] = R_ModernEncodeSrgb(light[c]);
        value[c] = value[c] > 4.0f ? 4.0f : value[c];
        largest = value[c] > largest ? value[c] : largest;
    }
    int alpha = (int)ceilf(largest / 4.0f * 255.0f);
    alpha = alpha < 1 ? 1 : alpha > 255 ? 255 : alpha;
    const float scale = (float)alpha / 255.0f * 4.0f;
    for (int c = 0; c < 3; ++c)
    {
        int channel = (int)(value[c] / scale * 255.0f + 0.5f);
        channel = channel < 0 ? 0 : channel > 255 ? 255 : channel;
        texel[2 - c] = (uint8_t)channel;
    }
    texel[3] = (uint8_t)alpha;
}

// The mean light (linear, the channels' mean) of a face's first level as the
// game stores it.
static float R_ModernProbeTexelLight(const uint8_t *texel)
{
    const float scale = (float)texel[3] / 255.0f * 4.0f;
    float sum = 0.0f;
    for (int c = 0; c < 3; ++c)
        sum += R_ModernDecodeSrgb((float)texel[2 - c] / 255.0f * scale);
    return sum / 3.0f;
}

// The light the game's encoding can hold in a channel (a display value of 4).
static float R_ModernProbeLightLimit()
{
    static const float limit = R_ModernDecodeSrgb(4.0f);
    return limit;
}

// One face: a frame of the probe's origin looking along the face, which the
// back end reads back. size is the view's inner square (a pixel of border
// around it, as the game's own cube shots). overlay, when given, draws the
// frame's 2D on top (the loading screen), after the scene that is read.
static bool R_ModernCaptureFace(const GfxModernHooks *hooks, const float *origin, int face, int size, float *rgb,
                                void (*overlay)())
{
    float forward[3], down[3], up[3], left[3];
    R_ModernCubeDirection(face, 0.0f, 0.0f, forward);
    // The direction's change down the face: v from -1 to 1.
    float below[3];
    R_ModernCubeDirection(face, 0.0f, 1.0f, below);
    for (int i = 0; i < 3; ++i)
    {
        down[i] = below[i] - forward[i];
        up[i] = -down[i];
    }
    // Right-handed: forward x left = up.
    Vec3Cross(up, forward, left);

    refdef_s *refdef = (refdef_s *)Z_Malloc(sizeof(refdef_s), "R_ModernCaptureFace", 0);
    memset(refdef, 0, sizeof(*refdef));
    refdef->x = 0;
    refdef->y = 0;
    refdef->width = size + 2;
    refdef->height = size + 2;
    // The inner square spans exactly 90 degrees.
    refdef->tanHalfFovX = (float)(size + 2) / (float)size;
    refdef->tanHalfFovY = refdef->tanHalfFovX;
    refdef->zNear = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        refdef->vieworg[i] = origin[i];
        refdef->viewaxis[0][i] = forward[i];
        refdef->viewaxis[1][i] = left[i];
        refdef->viewaxis[2][i] = up[i];
    }
    refdef->localClientNum = 0;
    refdef->time = 0;
    refdef->blurRadius = 0.0f;
    refdef->useScissorViewport = 0;
    R_InitPrimaryLights(refdef->primaryLights);

    FxCmd cmd;
    R_BeginFrame();
    R_BeginSharedCmdList();
    R_ClearClientCmdList2D();
    R_ClearScene(0);
    FX_BeginUpdate(0);
    R_SetLodOrigin(refdef);
    const float zfar = R_GetFarPlaneDist();
    FX_SetNextUpdateCamera(0, refdef, zfar);
    FX_FillUpdateCmd(0, &cmd);
    R_UpdateSpotLightEffect(&cmd);
    R_UpdateNonDependentEffects(&cmd);
    R_UpdateRemainingEffects(&cmd);
    R_RenderScene(refdef);
    if (overlay)
        overlay();
    R_EndFrame();
    R_IssueRenderCommands(0xFFFFFFFF);
    R_SyncRenderThread();
    Z_Free(refdef, 0);
    return hooks->probeCaptureRead(hooks->user, dx.device, 1, 1, size, size, rgb);
}

// A probe drawn again: the light along each texel's direction of its first
// level, by face, and the mean light of the probe as stored and as drawn.
struct ModernCapturedProbe
{
    uint32_t index;
    int edge;
    float *light;
    float before;
    float after;
};

// The new probes are scaled, all by one number for the map, to the stock
// probes' mean light (David's choice 2026-11-15, PC probes run 2: drawn as
// they are they came out about half as bright, and mp_bog's water lost its
// glints); within these bounds.
static const float kModernProbeScaleLeast = 0.25f;
static const float kModernProbeScaleMost = 4.0f;

static bool R_ModernDrawProbe(const GfxModernHooks *hooks, const GfxReflectionProbe &probe, int edge, float *rgb[6],
                              float *light, void (*overlay)())
{
    const int size = 2 * edge;
    for (int face = 0; face < 6; ++face)
    {
        if (!R_ModernCaptureFace(hooks, probe.origin, face, size, rgb[face], overlay))
            return false;
    }
    for (int face = 0; face < 6; ++face)
    {
        // Each texel: the light along its direction in the view of the same
        // face (its forward is the face's centre), averaged over the two by
        // two pixels around where the direction lands.
        float forward[3], below[3], up[3], left[3];
        R_ModernCubeDirection(face, 0.0f, 0.0f, forward);
        R_ModernCubeDirection(face, 0.0f, 1.0f, below);
        for (int i = 0; i < 3; ++i)
            up[i] = forward[i] - below[i];
        Vec3Cross(up, forward, left);
        for (int y = 0; y < edge; ++y)
        {
            for (int x = 0; x < edge; ++x)
            {
                float d[3];
                R_ModernCubeDirection(face, 2.0f * ((float)x + 0.5f) / (float)edge - 1.0f,
                                      2.0f * ((float)y + 0.5f) / (float)edge - 1.0f, d);
                const float along = Vec3Dot(d, forward);
                // Across (right is -left) and down the view, -1 to 1.
                const float sx = -Vec3Dot(d, left) / along;
                const float sy = -Vec3Dot(d, up) / along;
                // The view's pixel centres are at (i + 0.5) / size * 2 - 1.
                const float px = (sx + 1.0f) * 0.5f * (float)size - 0.5f;
                const float py = (sy + 1.0f) * 0.5f * (float)size - 0.5f;
                int x0 = (int)floorf(px), y0 = (int)floorf(py);
                x0 = x0 < 0 ? 0 : x0 > size - 2 ? size - 2 : x0;
                y0 = y0 < 0 ? 0 : y0 > size - 2 ? size - 2 : y0;
                float *out = &light[3 * ((face * edge + y) * edge + x)];
                out[0] = out[1] = out[2] = 0.0f;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const float *pixel = &rgb[face][3 * ((y0 + dy) * size + x0 + dx)];
                        for (int c = 0; c < 3; ++c)
                            out[c] += 0.25f * pixel[c];
                    }
            }
        }
    }
    return true;
}

// The mean light of a probe as stored (its first level), and of its new light
// as the encoding would hold it.
static bool R_ModernProbeMeans(IDirect3DCubeTexture9 *cube, int edge, const float *light, float *before,
                               float *after)
{
    float stored = 0.0f, drawn = 0.0f;
    for (int face = 0; face < 6; ++face)
    {
        D3DLOCKED_RECT locked;
        if (FAILED(cube->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, nullptr, 0)))
            return false;
        for (int y = 0; y < edge; ++y)
        {
            const uint8_t *row = (const uint8_t *)locked.pBits + y * locked.Pitch;
            for (int x = 0; x < edge; ++x)
                stored += R_ModernProbeTexelLight(&row[4 * x]);
        }
        cube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
    }
    const int count = 6 * edge * edge;
    for (int i = 0; i < 3 * count; ++i)
        drawn += light[i] < R_ModernProbeLightLimit() ? light[i] : R_ModernProbeLightLimit();
    *before = stored / (float)count;
    *after = drawn / (float)(3 * count);
    return true;
}

static bool R_ModernWriteProbe(IDirect3DCubeTexture9 *cube, int edge, const float *light, float scale)
{
    for (int face = 0; face < 6; ++face)
    {
        D3DLOCKED_RECT locked;
        if (FAILED(cube->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, nullptr, 0)))
            return false;
        for (int y = 0; y < edge; ++y)
        {
            uint8_t *row = (uint8_t *)locked.pBits + y * locked.Pitch;
            for (int x = 0; x < edge; ++x)
            {
                const float *in = &light[3 * ((face * edge + y) * edge + x)];
                const float scaled[3] = {in[0] * scale, in[1] * scale, in[2] * scale};
                R_ModernStoreProbeTexel(scaled, &row[4 * x]);
            }
        }
        cube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
    }
    return true;
}

void R_ModernCaptureProbes(void (*overlay)())
{
    if (!s_modernProbesPending || !rgp.world || !rg.registered || r_norefresh->current.enabled)
        return;
    s_modernProbesPending = false;
    const GfxModernHooks *hooks = R_ModernHooks();
    if (!hooks || !hooks->probeCaptureWanted || !hooks->probeCapture || !hooks->probeCaptureRead
        || !hooks->reflectionProbe || !hooks->probeCaptureWanted(hooks->user, dx.device))
        return;
    const int started = Sys_Milliseconds();
    int skipped = 0;
    float *rgb[6] = {};
    int rgbEdge = 0;
    const uint32_t probeCount = rgp.world->reflectionProbeCount;
    ModernCapturedProbe *probes =
        (ModernCapturedProbe *)Z_Malloc(sizeof(ModernCapturedProbe) * (probeCount ? probeCount : 1),
                                        "R_ModernCaptureProbes", 0);
    uint32_t captured = 0;
    float before = 0.0f, after = 0.0f;

    // First every probe is drawn, and its light kept.
    s_modernProbeCapturing = true;
    hooks->probeCapture(hooks->user, dx.device, true);
    for (uint32_t probeIndex = 1; probeIndex < probeCount; ++probeIndex)
    {
        const GfxReflectionProbe &probe = rgp.world->reflectionProbes[probeIndex];
        IDirect3DCubeTexture9 *cube = probe.reflectionImage ? probe.reflectionImage->texture.cubemap : nullptr;
        D3DSURFACE_DESC desc;
        if (!cube || FAILED(cube->GetLevelDesc(0, &desc)) || desc.Format != D3DFMT_A8R8G8B8 || desc.Width < 2
            || desc.Width > 256 || desc.Width != desc.Height)
        {
            ++skipped;
            continue;
        }
        const int edge = (int)desc.Width;
        // Two by two of the views' pixels for each texel.
        const int size = 2 * edge;
        if (edge != rgbEdge)
        {
            for (int face = 0; face < 6; ++face)
            {
                if (rgb[face])
                    Z_Free(rgb[face], 0);
                rgb[face] = (float *)Z_Malloc(sizeof(float) * 3 * size * size, "R_ModernCaptureProbes", 0);
            }
            rgbEdge = edge;
        }
        ModernCapturedProbe &kept = probes[captured];
        kept.index = probeIndex;
        kept.edge = edge;
        kept.light = (float *)Z_Malloc(sizeof(float) * 3 * 6 * edge * edge, "R_ModernCaptureProbes", 0);
        if (!R_ModernDrawProbe(hooks, probe, edge, rgb, kept.light, overlay)
            || !R_ModernProbeMeans(cube, edge, kept.light, &kept.before, &kept.after))
        {
            Z_Free(kept.light, 0);
            ++skipped;
            continue;
        }
        before += kept.before;
        after += kept.after;
        ++captured;
    }
    hooks->probeCapture(hooks->user, dx.device, false);
    s_modernProbeCapturing = false;
    for (int face = 0; face < 6; ++face)
    {
        if (rgb[face])
            Z_Free(rgb[face], 0);
    }

    // Then all of them are scaled to the stock probes' mean light, written
    // over their first level, and their blurrier levels made again.
    float scale = after > 0.0f ? before / after : 1.0f;
    scale = scale < kModernProbeScaleLeast ? kModernProbeScaleLeast
            : scale > kModernProbeScaleMost ? kModernProbeScaleMost
                                             : scale;
    int written = 0;
    for (uint32_t i = 0; i < captured; ++i)
    {
        const ModernCapturedProbe &kept = probes[i];
        const GfxReflectionProbe &probe = rgp.world->reflectionProbes[kept.index];
        IDirect3DCubeTexture9 *cube = probe.reflectionImage->texture.cubemap;
        if (R_ModernWriteProbe(cube, kept.edge, kept.light, scale))
        {
            hooks->reflectionProbe(hooks->user, dx.device, cube);
            ++written;
            if (written <= 64)
                Com_Printf(CON_CHANNEL_GFX,
                           "modern: probe %u at (%.0f %.0f %.0f): mean light %.3f as stored, %.3f drawn, %.3f "
                           "scaled\n",
                           kept.index, probe.origin[0], probe.origin[1], probe.origin[2], kept.before, kept.after,
                           kept.after * scale);
        }
        else
            ++skipped;
        Z_Free(kept.light, 0);
    }
    Z_Free(probes, 0);
    const int took = Sys_Milliseconds() - started;
    Com_Printf(CON_CHANNEL_GFX,
               "modern: %d reflection probes drawn again with the Modern path (%d left as they were) in %d ms; "
               "mean light %.3f as stored, %.3f drawn, scaled by %.2f to the stored light\n",
               written, skipped, took, captured ? before / (float)captured : 0.0f,
               captured ? after / (float)captured : 0.0f, scale);
}
