#pragma once

#include "rb_backend.h"
#include "r_init.h"
#include "r_rendercmds.h"

void R_InitContext(const GfxBackEndData *data, GfxCmdBuf *cmdBuf);
void R_CmdBufSet3D(GfxCmdBufSourceState *source);