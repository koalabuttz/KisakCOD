// The PS Vita client's Direct3DCreate9: the renderer's way to its graphics
// device (R_HW_Direct3DCreate9 in src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp).
//
// Stage 1 (docs/vita-client.md) has no GXM back end yet, so this answers as
// DXVK Native does when no device can be made: with no interface. The renderer
// then stops with its own "couldn't create a Direct3D device" error, once
// everything before it (file system, zones, sound, UI start-up) has run on the
// Vita. The GXM back end replaces this file.

#include "vita_client.h"

#include <linux/d3d9_types.h>

IDirect3D9 *WINAPI Direct3DCreate9(UINT)
{
    Vita_ClientLog("vita: no GXM graphics back end in this build yet (stage 1), so there is no D3D9 device");
    return nullptr;
}
