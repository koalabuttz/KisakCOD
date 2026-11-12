// R_AvailableTextureMemory for native Linux, through the native D3D9 boundary.
//
// src/gfx_d3d/r_texturemem.cpp owns it on Windows and asks DirectDraw for the
// video memory size first (<ddraw.h>, so the unit does not compile here).
// DirectDraw does not exist on Linux, and 0 is what R_VideoMemory returns when
// DirectDraw cannot tell. What follows is r_texturemem.cpp:150 and
// R_DetectCurrentTextureMemory below it for that case, with its reporting cache:
// IDirect3DDevice9::GetAvailableTextureMem, reported once and never lowered.
//
// Also Sys_DetectClientVideoCard, win_configure.cpp's Sys_DetectVideoCard for
// the game clients (src/linux/main.cpp calls it from Sys_FindInfo).
//
// Linked by KisakCOD-mp, KisakCOD-sp and the world draw probe.

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

// What win_configure.cpp does: ask a D3D9 interface for adapter 0's
// description and let it go again. configure_mp.csv's GPU rows are matched
// against that text (Com_SetRecommendedGpu), and sys_gpu keeps it. DXVK Native
// reports the Vulkan device's name there; it starts SDL's video itself, so this
// works before the renderer has made a window, as R_PreCreateWindow relies on
// too. DXVK_WSI_DRIVER has to be set first (Sys_InstallClientWindow). Without a
// Vulkan device there is no interface and the text stays "Unknown video card",
// Windows' answer without D3D9. The renderer is not involved: r_backend is not
// registered yet, and the card is the same whichever back end draws later.
void Sys_DetectClientVideoCard(int descLimit, char *description)
{
    I_strncpyz(description, "Unknown video card", descLimit);
    IDirect3D9 *const d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9)
        return;
    D3DADAPTER_IDENTIFIER9 id{};
    if (d3d9->GetAdapterIdentifier(0, 0, &id) >= 0)
        I_strncpyz(description, id.Description, descLimit);
    d3d9->Release();
}
