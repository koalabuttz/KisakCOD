#pragma once
#include "r_rendercmds.h"

enum RenderTargetUsage : __int32
{                                       // ...
    RENDERTARGET_USAGE_DEPTH = 0x0,
    RENDERTARGET_USAGE_RENDER = 0x1,
    RENDERTARGET_USAGE_RENDER_SHARE_SCENE = 0x2,
    RENDERTARGET_USAGE_TEXTURE = 0x3,
};

void AssertUninitializedRenderTarget(const GfxRenderTarget *renderTarget);
int R_GetDepthStencilFormat(_D3DFORMAT renderTargetFormat);
void R_InitRenderTargets();
void R_InitRenderTargets_PC();
void R_ShareRenderTarget(GfxRenderTargetId idFrom, GfxRenderTargetId idTo);
void R_InitFullscreenRenderTargetImage(
    int imageProgType,
    FullscreenType screenType,
    int picmip,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget);
void R_GetFullScreenRes(FullscreenType screenType, int *fullscreenWidth, int *fullscreenHeight);
IDirect3DSurface9 *R_AssignSingleSampleDepthStencilSurface();
void R_InitRenderTargetImage(
    int imageProgType,
    uint16_t width,
    uint16_t height,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget);
void R_AssignImageToRenderTargetColor(GfxRenderTargetSurface *surface, GfxImage *image);
void R_InitShadowmapRenderTarget(
    int imageProgType,
    uint16_t tileRes,
    uint16_t tileRowCount,
    GfxRenderTarget *renderTarget);
void R_InitAndTrackRenderTargetImage(
    int imageProgType,
    uint16_t width,
    uint16_t height,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget);
void R_InitShadowCookieBlurRenderTarget(GfxRenderTarget *renderTarget);
void R_InitShadowCookieRenderTarget(GfxRenderTarget *renderTarget);
void R_AssignShadowCookieDepthStencilSurface(GfxRenderTargetSurface *surface);
_D3DFORMAT R_InitFrameBufferRenderTarget();
void R_ShutdownRenderTargets();
const char *R_RenderTargetName(GfxRenderTargetId renderTargetId);
