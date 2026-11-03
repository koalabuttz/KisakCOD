#pragma once
#include "rb_backend.h"


void R_DrawXModelRigidModelSurf(GfxCmdBufContext context, XSurface *xsurf);

uint32_t R_DrawXModelRigidSurf(
    const GfxDrawSurf *drawSurfList,
    uint32_t drawSurfCount,
    GfxCmdBufContext context);

uint32_t R_DrawXModelRigidSurfCamera(
    const GfxDrawSurf *drawSurfList,
    uint32_t drawSurfCount,
    GfxCmdBufContext context);

uint32_t R_DrawXModelRigidSurfLit(
    const GfxDrawSurf *drawSurfList,
    uint32_t drawSurfCount,
    GfxCmdBufContext context);