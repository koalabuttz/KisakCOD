#pragma once
#include "r_rendercmds.h"
#include "rb_backend.h"


bool R_UsingGlow(const GfxViewInfo *viewInfo);
bool R_UsingDepthOfField(const GfxViewInfo *viewInfo);
bool RB_UsingColorManipulation(const GfxViewInfo *viewInfo);
void RB_ApplyColorManipulationFullscreen(const GfxViewInfo *viewInfo);
void RB_ApplyColorManipulationSplitscreen(const GfxViewInfo *viewInfo);
void RB_GetResolvedScene();
void RB_GetDepthOfFieldInputImages(float radius);
void RB_ProcessPostEffects(const GfxViewInfo *viewInfo);
bool RB_UsingMergedPostEffects(const GfxViewInfo *viewInfo);
bool RB_UsingBlur(float blurRadius);
bool RB_UsingPostEffects(const GfxViewInfo *viewInfo);
void RB_CalcGlowEffect(const GfxViewInfo *viewInfo);
void RB_ApplyGlowEffect(const GfxViewInfo *viewInfo);
void RB_ApplyGlowEffectPass(const GfxViewInfo *viewInfo, GfxImage *glowImage);
void RB_ApplyMergedPostEffects(const GfxViewInfo *viewInfo);

void RB_GetSceneDepthOfFieldEquation(
    float nearOutOfFocus,
    float nearInFocus,
    float farInFocus,
    float farOutOfFocus,
    float *dofEquation,
    float zNear);
void RB_GetNearDepthOfFieldEquation(
    float outOfFocus,
    float inFocus,
    float nearClip,
    float depthScale,
    float *dofEquation);
void RB_GetViewModelDepthOfFieldEquation(float outOfFocus, float inFocus, float *dofEquation);
float RB_GetDepthOfFieldBlurFraction(const GfxViewInfo *viewInfo, float pixelRadiusAtSceneRes);
float RB_GetBlurRadius(float blurRadiusFromCode);
void RB_BlurScreen(const GfxViewInfo *viewInfo, float blurRadius);
