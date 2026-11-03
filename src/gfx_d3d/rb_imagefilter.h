#pragma once
#include "r_rendercmds.h"

struct GfxImageFilterPass // sizeof=0x98
{                                       // ...
    const Material *material;           // ...
    float srcWidth;                     // ...
    float srcHeight;                    // ...
    int dstWidth;                       // ...
    int dstHeight;                      // ...
    int tapHalfCount;                   // ...
    float tapOffsetsAndWeights[8][4];
};

struct GfxImageFilter // sizeof=0x130C
{                                       // ...
    int passCount;                      // ...
    GfxImageFilterPass passes[32];      // ...
    GfxImage *sourceImage;              // ...
    GfxRenderTargetId finalTarget;      // ...
};


void RB_GaussianFilterImage(
    float radius,
    GfxRenderTargetId srcRenderTargetId,
    GfxRenderTargetId dstRenderTargetId);
void RB_VirtualToSceneRadius(float radius, float *radiusX, float *radiusY);
int RB_GenerateGaussianFilterChain(
    float radiusX,
    float radiusY,
    uint32_t srcWidth,
    uint32_t srcHeight,
    int dstWidth,
    int dstHeight,
    int passLimit,
    GfxImageFilterPass *filterPass);
void RB_GenerateGaussianFilter1D(float radius, int *res, int axis, GfxImageFilterPass *filterPass);
int RB_PickSymmetricFilterMaterial(int halfTapCount, const Material **material);
int RB_GaussianFilterPoints1D(float pixels, int srcRes, int dstRes, int tapLimit, float *tapOffsets, float *tapWeights);
void RB_GenerateGaussianFilter2D(
    float radius,
    uint32_t srcWidth,
    uint32_t srcHeight,
    int dstWidth,
    int dstHeight,
    GfxImageFilterPass *filterPass);
void RB_FilterImage(GfxImageFilter *filter);
void RB_SetupFilterPass(const GfxImageFilterPass *filterPass);
void RB_FilterPingPong(const GfxImageFilter *filter, int passIndex);
void RB_GlowFilterImage(float radius);
GfxRenderTargetId RB_ApplyGlowFilter(
    float radius,
    GfxRenderTargetId srcRenderTarget,
    GfxRenderTargetId dstRenderTarget);
