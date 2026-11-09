// The PS Vita client's Direct3DCreate9: the renderer's way to its graphics
// device (R_HW_Direct3DCreate9 in src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp).
//
// Stage 1 (docs/vita-client.md) has no GXM back end yet. Answering "no
// device" as DXVK does would not stop the renderer: R_PreCreateWindow's
// failure is ignored and the next step asserts on the empty display mode
// list (first Vita run). So the client stops here, through Sys_Error, which
// logs the memory pools to console-mp.log and exits without a crash dump.
// The GXM back end replaces this file.

#include "vita_client.h"

#include <linux/d3d9_types.h>

#include <win32/win_local.h> // Sys_Error

IDirect3D9 *WINAPI Direct3DCreate9(UINT)
{
    Sys_Error("vita: no GXM graphics back end in this build yet (stage 1): the client stops at the renderer");
    return nullptr; // Sys_Error does not return
}
