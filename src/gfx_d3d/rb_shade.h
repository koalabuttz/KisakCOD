#pragma once
#include "rb_backend.h"

enum $A1823DBB2E1ECBBFE5B3FD011D242004 : __int32
{
    R_NORMAL_OVERRIDE_FLAT = 0x0,
    R_NORMAL_OVERRIDE_NONE = 0x1,
    R_NORMAL_OVERRIDE_COUNT = 0x2,
};

void R_SetVertexDecl(GfxCmdBufPrimState *primState, const MaterialVertexDeclaration *vertexDecl);
void RB_ClearPixelShader();
void R_HW_SetPixelShader(IDirect3DDevice9 *device, const MaterialPixelShader *mtlShader);
void RB_ClearVertexShader();
void R_HW_SetVertexShader(IDirect3DDevice9 *device, const MaterialVertexShader *mtlShader);
void RB_ClearVertexDecl();
void RB_SetTessTechnique(const Material *material, MaterialTechniqueType techType);
void RB_BeginSurface(const Material *material, MaterialTechniqueType techType);
void RB_EndTessSurface();
GfxPrimStats *RB_EndSurfacePrologue();
void RB_EndSurfaceEpilogue();
void RB_DrawTessSurface();
void R_DrawTessTechnique(GfxCmdBufContext context, const GfxDrawPrimArgs *args);
void RB_TessOverflow();

