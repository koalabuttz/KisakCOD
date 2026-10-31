#pragma once
#include "r_rendercmds.h"

// Explicit storage boundary shared by game command generation and platform backends.
// Caller owns aligned storage, frame lifecycle and diagnostics; allocation never owns it.
GfxCmdHeader *R_AppendCommand(GfxCmdArray &list, unsigned capacity, GfxRenderCommand command, int bytes);
using GfxCommandAllocate = GfxCmdHeader *(*)(void *,GfxRenderCommand,int);
void R_EmitClearScreenCommand(void *owner,GfxCommandAllocate allocate,int whichToClear,
                             const float *color,float depth,uint8_t stencil);
void R_EmitEndCommand(void *owner,GfxCommandAllocate allocate);
