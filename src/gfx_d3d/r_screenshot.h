#pragma once
#include <bgame/bg_local.h>
#include "r_reflection_probe.h"

#define CUBE_MAP_HIGH_MIP_SIZE 64

enum GfxScreenshotType : __int32
{                                       // ...
    R_SCREENSHOT_JPG = 0x0,
    R_SCREENSHOT_TGA = 0x1,
};

enum CubeCoord : __int32
{                                       // ...
    CUBE_X0 = 0x0,
    CUBE_X1 = 0x1,
    CUBE_Y0 = 0x2,
    CUBE_Y1 = 0x3,
    CUBE_NUM_COORDS = 0x4,
};

enum FlipEdge : __int32
{                                       // ...
    DONT_FLIP_EDGE = 0x0,
    FLIP_EDGE = 0x1,
};

void TRACK_r_screenshot();
void R_LevelShot();
void R_SaveGameShot(const char *saveName);
void R_BeginCubemapShot(int pixelWidthHeight, int pixelBorder);
void R_CubemapShotSetInitialState();
void R_EndCubemapShot(CubemapShot shotIndex);
void R_CopyCubemapShot(CubemapShot shotIndex);
void R_CubemapShotCopySurfaceToBuffer(uint8_t *buffer, int bufferSizeInBytes);
void R_CubemapShotRestoreState();
void R_SaveCubemapShot(char *filename, CubemapShot shotIndex, float n0, float n1);
void R_CubemapShotWriteTargaFile(char *filename, CubemapShot shotIndex, float n0, float n1);
void R_CubemapShotWriteTargaHeader(int res, uint8_t *fileBuffer);
void R_CubemapShotCopyBufferToTarga(const uint8_t *srcBuffer, uint8_t *targa);
void R_CubemapShotApplyFresnelToTarga(CubemapShot shotIndex, float n0, float n1, uint8_t *targa);
uint8_t R_CubemapShotCalcReflectionFactor(
    int shotIndex,
    int colIndex,
    int rowIndex,
    float n0,
    float n1);
void R_LightingFromCubemapShots(const float *baseColor);
void R_GetDirForCubemapPixel(int faceIndex, float x, float y, float *dir);
void R_CubemapShotExtractLinearLight(
    uint8_t **pixels,
    int width,
    int height,
    float (**linearColors)[3]);

void R_ScreenshotCommand(GfxScreenshotType type);

void R_CreateReflectionRawDataFromCubemapShot(struct DiskGfxReflectionProbe *probeRawData, int downSampleRes);