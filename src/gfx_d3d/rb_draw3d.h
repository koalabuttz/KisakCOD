#pragma once
#if defined(__linux__) || defined(__APPLE__)
#include <linux/d3d9_types.h> // native Linux: D3D9 type surface only, no D3D9 API
#else
#include <d3d9.h>
#endif
#include "rb_backend.h"

enum $F8BAC180992631E19A574A0F352E600A : __int32
{
    GFX_SM_OVERLAY_OFF = 0x0,
    GFX_SM_OVERLAY_SUN = 0x1,
    GFX_SM_OVERLAY_SPOT = 0x2,
    GFX_SM_OVERLAY_COUNT = 0x3,
};

struct GfxPointLitSurfsInfo // sizeof=0x1C
{                                       // ...
    const GfxViewInfo *viewInfo;        // ...
    GfxMeshData *clearQuadMesh;         // ...
    const GfxDrawSurfListInfo *drawSurfInfo; // ...
    uint32_t x;                     // ...
    uint32_t y;                     // ...
    uint32_t w;                     // ...
    uint32_t h;                     // ...
};

void R_HW_InsertFence(IDirect3DQuery9 **fence);
void R_ShowTris(GfxCmdBufContext context, const GfxDrawSurfListInfo *info);
void RB_Draw3DInternal(const GfxViewInfo *viewInfo);
void RB_FullbrightDrawCommands(const GfxViewInfo *viewInfo);
void RB_EndSceneRendering(GfxCmdBufContext context, const GfxCmdBufInput *input, const GfxViewInfo *viewInfo);
void R_SetAndClearSceneTarget(const GfxViewport *viewport);
void R_ClearForFrameBuffer(IDirect3DDevice9 *device, const GfxViewport *viewport);
void R_DrawFullbright(const GfxViewInfo *viewInfo, GfxCmdBufInput *input, GfxCmdBuf *cmdBuf);
void R_DrawDebugShaderLitCallback(const void *data, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void R_DrawFullbrightDecalCallback(const void *data, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void R_DrawDebugShaderEmissiveCallback(const void *data, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void RB_DebugShaderDrawCommands(const GfxViewInfo *viewInfo);
void R_DrawDebugShader(const GfxViewInfo *viewInfo, GfxCmdBuf *cmdBuf);
void R_DrawDebugShaderDecalCallback(const void *data, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void RB_StandardDrawCommands(const GfxViewInfo *viewInfo);
void  R_DrawLights(const GfxViewInfo *viewInfo, GfxCmdBuf *cmdBuf);
void R_DrawPointLitSurfs(GfxCmdBufSourceState *source, const GfxViewInfo *viewInfo, GfxCmdBuf *cmdBuf);
void R_DrawPointLitSurfsCallback(const void *userData, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void  R_DrawEmissive(const GfxViewInfo *viewInfo, GfxCmdBuf *cmdBuf);
void R_DrawEmissiveCallback(const void *userData, GfxCmdBufContext context, GfxCmdBufContext prepassContext);
void RB_Draw3DCommon();
void R_SetResolvedScene(GfxCmdBufContext context);
void RB_DebugShaderDrawCommandsCommon();
void RB_StandardDrawCommandsCommon();
void RB_ApplyLatePostEffects(const GfxViewInfo *viewInfo);
void RB_DrawDebugPostEffects();
void RB_ShowFbColorDebug_Screen();
void RB_ShowFbColorDebug_Feedback();
void RB_ShowFloatZDebug();
void RB_ShowShadowsDebug();
