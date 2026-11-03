#pragma once
#include "rb_backend.h"


int R_SetupMaterial(
    GfxCmdBufContext context,
    GfxCmdBufContext *prepassContext,
    const GfxDrawSurfListInfo *info,
    GfxDrawSurf drawSurf);
int R_SetPrepassMaterial(GfxCmdBufContext context, GfxDrawSurf drawSurf, MaterialTechniqueType techType);
int R_SetMaterial(GfxCmdBufContext context, GfxDrawSurf drawSurf, MaterialTechniqueType techType);
void R_SetGameTime(GfxCmdBufSourceState *source, float gameTime);
int R_UpdateMaterialTime(GfxCmdBufSourceState *source, float materialTime);
