#pragma once
#include "r_rendercmds.h"

// Explicit storage boundary shared by game command generation and platform backends.
// Caller owns aligned storage, frame lifecycle and diagnostics; allocation never owns it.
GfxCmdHeader *R_AppendCommand(GfxCmdArray &list, unsigned capacity, GfxRenderCommand command, int bytes);
using GfxCommandAllocate = GfxCmdHeader *(*)(void *,GfxRenderCommand,int);
void R_EmitClearScreenCommand(void *owner,GfxCommandAllocate allocate,int whichToClear,
                             const float *color,float depth,uint8_t stencil);
void R_EmitEndCommand(void *owner,GfxCommandAllocate allocate);

void R_EmitStretchRawCommand(void *owner,GfxCommandAllocate allocate,int x,int y,int width,int height,
                             int columns,int rows,const uint8_t *rgba);

void R_EmitStretchPicCommand(void *owner,GfxCommandAllocate allocate,float x,float y,float width,float height,
                             float s0,float t0,float s1,float t1,const float *color,Material *material);

GfxCmdDrawText2D *R_EmitBaseTextCommand(void *owner,GfxCommandAllocate allocate,
    const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
    float rotation,const float *color,int style,int cursorPos,char cursor);
