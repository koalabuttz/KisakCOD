#pragma once
#include "r_material.h"

#define HCOUNT 4096

struct WaterGlob // sizeof=0x9000
{                                       // ...
    complex_s H[4096];
    uint8_t pixels[4096];       // ...
};

struct WaterGlobStatic // sizeof=0x1C00
{                                       // ...
    float sinTable[1024];               // ...
    complex_s fftTrigTable[256];        // ...
    int fftBitswap[256];                // ...
};

void TRACK_r_water();
void R_UploadWaterTextureInternal(water_t **data);
void WaterFrequenciesAtTime(complex_s *H, const water_t *water, float t);
void WaterAmplitudesFromFrequencies(complex_s *H, const water_t *water);
void TransposeArray(complex_s *H, uint32_t M);
void WaterPixelsFromAmplitudes(GfxColor *pixels, complex_s *H, const water_t *water);
void GenerateMipMaps(_D3DFORMAT format, uint8_t *pixels, water_t *water);
void R_UploadWaterTexture(water_t *water, float floatTime);
void R_InitWater();
void Load_PicmipWater(water_t **waterRef);

// r_water_load_obj
void R_InitLoadWater();
water_t *R_LoadWaterSetup(const water_t *water);
void R_ShutdownLoadWater();