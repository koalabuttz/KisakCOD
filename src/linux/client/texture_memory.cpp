// R_AvailableTextureMemory for native Linux, through the native D3D9 boundary.
//
// src/gfx_d3d/r_texturemem.cpp owns it on Windows and asks DirectDraw for the
// video memory size first (<ddraw.h>, so the unit does not compile here).
// DirectDraw does not exist on Linux, and 0 is what R_VideoMemory returns when
// DirectDraw cannot tell. What follows is r_texturemem.cpp:150 and
// R_DetectCurrentTextureMemory below it for that case, with its reporting cache:
// IDirect3DDevice9::GetAvailableTextureMem, reported once and never lowered.
//
// Linked by KisakCOD-mp and the world draw probe.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_texturemem.h>

static uint32_t s_maxReportedTexMem;

uint32_t R_AvailableTextureMemory()
{
    iassert(dx.device);
    const uint32_t texMemInMegs = dx.device->GetAvailableTextureMem() >> 20;
    Com_Printf(CON_CHANNEL_GFX,
               "DirectX reports %i MB of available texture memory, but wouldn't tell available video memory.\n",
               texMemInMegs);
    if (s_maxReportedTexMem >= texMemInMegs)
        Com_Printf(CON_CHANNEL_GFX, "Using previously reported texture memory size of %i MB.\n", s_maxReportedTexMem);
    else
        s_maxReportedTexMem = texMemInMegs;
    return s_maxReportedTexMem;
}
