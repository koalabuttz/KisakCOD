// Device-free production viewport state, shared by renderer backends.
#include <universal/q_shared.h>
#include "r_state.h"

void __cdecl R_SetViewportStruct(GfxCmdBufSourceState *source, const GfxViewport *viewport)
{
    iassert(viewport->width > 0);
    iassert(viewport->height > 0);
#ifndef KISAK_RADIANT
    iassert(source->viewportBehavior == GFX_USE_VIEWPORT_FOR_VIEW);
#endif

    source->sceneViewport = *viewport;
    source->viewMode = VIEW_MODE_NONE;
    source->viewportIsDirty = 1;
}

void __cdecl R_SetViewportValues(GfxCmdBufSourceState *source, int x, int y, int width, int height)
{
    GfxViewport viewport; // [esp+0h] [ebp-10h] BYREF

    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    R_SetViewportStruct(source, &viewport);
}
