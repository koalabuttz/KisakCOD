#pragma once
#include "rb_backend.h"


void TRACK_rb_state();
void RB_SetInitialState();
void RB_InitSceneViewport();
void RB_InitImages();


extern const GfxCmdBufContext gfxCmdBufContext;
extern GfxCmdBufSourceState gfxCmdBufSourceState;
extern GfxCmdBufState gfxCmdBufState;
extern GfxCmdBufInput gfxCmdBufInput;