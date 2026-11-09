// The PS Vita client's Direct3DCreate9: the renderer's way to its graphics
// device (R_HW_Direct3DCreate9 in src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp).
//
// It hands out the D3D9 front (src/gfx_vk/d3d9), the same IDirect3D9 that
// r_backend vulkan uses on the PC, on the GXM Legacy back end
// (src/gfx_gxm/legacy). This file is the front's host: the one display mode,
// the one monitor and the log. See docs/vita-client.md.

#include "vita_client.h"

#include <linux/d3d9_types.h>

#include <gfx_vk/d3d9/vk_d3d9.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>

#include <win32/win_local.h>

namespace
{
namespace front = gfx_vk::d3d9;

uint32_t HostModeCount(void *)
{
    return 1;
}

bool HostMode(void *, uint32_t index, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    if (index)
        return false;
    width = kVitaScreenWidth;
    height = kVitaScreenHeight;
    refreshRate = 60;
    return true;
}

bool HostCurrentMode(void *user, uint32_t &width, uint32_t &height, uint32_t &refreshRate)
{
    return HostMode(user, 0, width, height, refreshRate);
}

HMONITOR HostMonitor(void *)
{
    return MonitorFromPoint(tagPOINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

void HostLog(void *, const char *line)
{
    Com_Printf(CON_CHANNEL_GFX, "%s\n", line);
}
} // namespace

IDirect3D9 *WINAPI Direct3DCreate9(UINT)
{
    front::Host host;
    host.modeCount = HostModeCount;
    host.mode = HostMode;
    host.currentMode = HostCurrentMode;
    host.monitor = HostMonitor;
    host.log = HostLog;
    // Compiled GXM programs, one file each (gxm_program_cache.h).
    host.shaderCachePath = "ux0:data/KisakCOD/gxp";
    Com_Printf(CON_CHANNEL_GFX, "Graphics back end: GXM (stage 2c: shaders compiled, no draws yet)\n");
    return front::CreateDirect3D9(host);
}

void Vita_RendererWindowDestroying(void *hwnd)
{
    front::WindowDestroying(reinterpret_cast<front::D3D9Window>(hwnd));
}
