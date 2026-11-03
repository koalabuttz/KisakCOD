#pragma once
#include "rb_backend.h"
#include "r_buffers.h"

struct GfxShaderConstantState_s // sizeof=0x8
{                                       // ...
    MaterialArgumentCodeConst codeConst;
    uint32_t version;
};
union GfxShaderConstantState // sizeof=0x8
{                                       // ...
    GfxShaderConstantState_s fields;
    unsigned __int64 packed;
};

int R_ReserveIndexData(GfxCmdBufPrimState *state, int triCount);
int R_SetVertexData(GfxCmdBufState *state, const void *data, int vertexCount, int stride);
void R_ChangeObjectPlacement(GfxCmdBufSourceState *source, const GfxScaledPlacement *placement);
int R_SetIndexData(GfxCmdBufPrimState *state, uint8_t *indices, int triCount);
void R_SetupPassPerPrimArgs(GfxCmdBufContext context);
void R_SetVertexShaderConstantFromCode(GfxCmdBufContext context, const MaterialShaderArgument *routingData);
void R_HW_SetVertexShaderConstant(__int64 device, __int64 data);
float *R_GetCodeConstant(GfxCmdBufContext context, uint32_t constant);
char R_IsVertexShaderConstantUpToDate(GfxCmdBufContext context, const MaterialShaderArgument *routingData);
char R_IsShaderMatrixUpToDate(
    const GfxCmdBufSourceState *source,
    GfxShaderConstantState *constant,
    const MaterialShaderArgument *routingData);
char R_IsShaderConstantUpToDate(
    const GfxCmdBufSourceState *source,
    GfxShaderConstantState *constant,
    const MaterialShaderArgument *routingData);
void R_SetupPassPerObjectArgs(GfxCmdBufContext context);
void R_SetPixelShaderConstantFromLiteral(GfxCmdBufState *state, uint32_t dest, const float *literal);
void R_HW_SetPixelShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount);
int R_IsPixelShaderConstantUpToDate(GfxCmdBufContext context, const MaterialShaderArgument *routingData);
void R_SetPixelShaderConstantFromCode(GfxCmdBufContext context, const MaterialShaderArgument *routingData);
void R_SetupPassCriticalPixelShaderArgs(GfxCmdBufContext context);
void R_SetupPassVertexShaderArgs(GfxCmdBufContext context);
void R_OverrideImage(GfxImage **image, const MaterialTextureDef *texdef);
void R_SetPixelShader(GfxCmdBufState *state, const MaterialPixelShader *pixelShader);
void R_SetVertexShader(GfxCmdBufState *state, const MaterialVertexShader *vertexShader);
void R_UpdateVertexDecl(GfxCmdBufState *state);
void R_SetupPass(GfxCmdBufContext context, uint32_t passIndex);
void R_SetState(GfxCmdBufState *state, uint32_t *stateBits);
void R_SetVertexShaderConstantFromLiteral(GfxCmdBufState *state, uint32_t dest, const float *literal);
const MaterialTextureDef *R_SetPixelSamplerFromMaterial(
    GfxCmdBufContext context,
    const MaterialShaderArgument *arg,
    const MaterialTextureDef *texDef);

void R_SetPassShaderPrimArguments(
    GfxCmdBufContext context,
    uint32_t argCount,
    const MaterialShaderArgument *arg);
void R_SetPassShaderObjectArguments(
    GfxCmdBufContext context,
    uint32_t argCount,
    const MaterialShaderArgument *arg);
void R_SetPassPixelShaderStableArguments(
    GfxCmdBufContext context,
    uint32_t argCount,
    const MaterialShaderArgument *arg);
void R_SetPassVertexShaderStableArguments(
    GfxCmdBufContext context,
    uint32_t argCount,
    const MaterialShaderArgument *arg);
void R_SetPassShaderStableArguments(
    GfxCmdBufContext context,
    uint32_t argCount,
    const MaterialShaderArgument *arg);

inline void R_CheckVertexDataOverflow(int dataSize)
{
    if (!gfxBuf.dynamicVertexBuffer)
        MyAssertHandler("c:\\trees\\cod3\\src\\gfx_d3d\\r_shade.h", 38, 0, "%s", "gfxBuf.dynamicVertexBuffer");
    if (dataSize <= 0 || dataSize > gfxBuf.dynamicVertexBuffer->total)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\r_shade.h",
            39,
            0,
            "%s\n\t(dataSize) = %i",
            "(dataSize > 0 && dataSize <= gfxBuf.dynamicVertexBuffer->total)",
            dataSize);
    if (dataSize + gfxBuf.dynamicVertexBuffer->used > gfxBuf.dynamicVertexBuffer->total)
        gfxBuf.dynamicVertexBuffer->used = 0;
}


// r_draw_pixelshader
uint32_t R_SkipDrawSurfListMaterial(const GfxDrawSurf *drawSurfList, uint32_t drawSurfCount);