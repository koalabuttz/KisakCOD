#pragma once

// The renderer's one interface to the graphics API.
//
// Every Direct3D 9 call the renderer (src/gfx_d3d) makes goes through a function
// declared here; the implementation is r_rhi_d3d9_*.cpp in this directory, and
// no other renderer unit calls a D3D9 method. scripts/tests/check_rhi_boundary.py
// enforces that in CI.
//
// This is step M1a of the renderer plan (the mechanical half of the seam): the
// calls moved out of the renderer verbatim, so the D3D9 build renders exactly as
// before and r_logFile prints the same lines. The signatures still use D3D9
// types; M1b replaces them with backend handles once the shader spike (S0) has
// settled what the Vulkan Legacy adapter needs. Names follow the engine's own
// R_HW_* convention, which most of these functions already had.

#include "../rb_backend.h"

struct MaterialPixelShader;
struct tagRECT; // RECT: Windows, and the native D3D9 boundary on Linux
struct MaterialVertexShader;

// ---- Draws and vertex input (r_rhi_d3d9_state.cpp) -----------------------------
void R_HW_DrawIndexedPrimitive(IDirect3DDevice9 *device, const GfxDrawPrimArgs *args, int triCount);
void R_HW_SetIndices(IDirect3DDevice9 *device, IDirect3DIndexBuffer9 *ib);
void R_HW_SetStreamSource(
    IDirect3DDevice9 *device,
    uint32_t streamIndex,
    IDirect3DVertexBuffer9 *vb,
    uint32_t vertexOffset,
    uint32_t vertexStride);
void R_HW_SetVertexDeclaration(IDirect3DDevice9 *device, IDirect3DVertexDeclaration9 *decl);

// ---- Scissor -------------------------------------------------------------------
void R_HW_EnableScissor(IDirect3DDevice9 *device, const tagRECT *rect);
void R_HW_DisableScissor(IDirect3DDevice9 *device);

// ---- Fixed-function state ------------------------------------------------------
void R_SetInitialContextState(IDirect3DDevice9 *device);
void R_HW_SetViewport(IDirect3DDevice9 *device, const GfxViewport *viewport, float nearValue, float farValue);
void R_HW_SetAlphaTestEnable(IDirect3DDevice9 *device, __int16 stateBits0);
void R_HW_SetColorMask(IDirect3DDevice9 *device, uint32_t stateBits0);
void R_HW_SetCullFace(IDirect3DDevice9 *device, __int16 stateBits0);
void R_HW_SetPolygonMode(IDirect3DDevice9 *device, signed int stateBits0);
void R_HW_DisableBlend(IDirect3DDevice9 *device);
void R_HW_SetBlend(
    IDirect3DDevice9 *device,
    bool blendWasEnabled,
    uint32_t changedBits,
    uint32_t stateBits0);
void R_SetAlphaTestFunction(GfxCmdBufState *state, __int16 stateBits0);
void R_HW_SetDepthWriteEnable(IDirect3DDevice9 *device, char stateBits1);
void R_HW_SetDepthTestEnable(IDirect3DDevice9 *device, char stateBits1);
void R_HW_SetDepthTestFunction(IDirect3DDevice9 *device, char stateBits1);
void R_HW_EnableStencil(IDirect3DDevice9 *device);
void R_HW_DisableStencil(IDirect3DDevice9 *device);
void R_HW_SetFrontStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail);
void R_HW_SetBackStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail);
void R_HW_SetFrontStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc);
void R_HW_SetBackStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc);
uint32_t R_HW_SetSamplerState(
    IDirect3DDevice9 *device,
    uint32_t samplerIndex,
    uint32_t samplerState,
    uint32_t oldSamplerState);
void R_HW_DisableSampler(IDirect3DDevice9 *device, uint32_t samplerIndex);
void R_HW_SetRenderTarget(GfxCmdBufState *state, GfxRenderTargetId newTargetId);
void R_ClearScreenInternal(
    IDirect3DDevice9 *device,
    uint8_t whichToClear,
    const float *color,
    float depth,
    uint8_t stencil,
    const GfxViewport *viewport);
void R_HW_SetPolygonOffset(IDirect3DDevice9 *device, float scale, float bias);
void R_SetAlphaAntiAliasingState(IDirect3DDevice9 *device, __int16 stateBits0);
void R_SetDefaultAlphaTestFunction(GfxCmdBufState *state);
void R_HW_ForceSamplerState(IDirect3DDevice9 *device, uint32_t samplerIndex, uint32_t samplerState);

// ---- Shaders, constants and textures -------------------------------------------
void R_HW_SetVertexShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount);
void R_HW_SetPixelShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount);
void R_HW_SetPixelShader(IDirect3DDevice9 *device, const MaterialPixelShader *mtlShader);
void R_HW_SetVertexShader(IDirect3DDevice9 *device, const MaterialVertexShader *mtlShader);
void R_HW_SetSamplerTexture(IDirect3DDevice9 *device, uint32_t samplerIndex, const GfxTexture *texture);
