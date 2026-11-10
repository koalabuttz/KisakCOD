#include <universal/q_shared.h>
#include "r_cmds.h"
#include <qcommon/cmd.h>

#include "r_screenshot.h"
#include "r_sky.h"
#include "r_dvars.h"
#include "r_init.h"
#include "r_rendercmds.h"
#include "r_image.h"
#include "r_model.h"
#include <universal/com_memory.h>
#include "r_staticmodel.h"
#include <xanim/xmodel.h>
#include "r_xsurface.h"
#include "rb_stats.h"
#include <DynEntity/DynEntity_client.h>
#include <database/database.h>
#include "r_staticmodelcache.h"

#include <algorithm>

void R_Cmd_ScreenshotJpeg()
{
	R_ScreenshotCommand(R_SCREENSHOT_JPG);
}
void R_Cmd_Screenshot()
{
	R_ScreenshotCommand(R_SCREENSHOT_TGA);
}

void R_Cmd_ApplyPicmip()
{
    Material_UpdatePicmipAll();
}

// KISAK_VULKAN_BACKEND reaches this file from scripts/vulkan_probe: on Linux
// and macOS vulkan_backend.cmake gives it to the objects this file is in (not
// the ones that make D3D9 calls), on Windows the whole program has it.
#if defined(KISAK_VULKAN_BACKEND)
// The Vulkan back end (src/linux/client/vulkan/vulkan_backend.h on Linux and
// macOS, src/win32/vulkan/win_vulkan_backend.cpp on Windows).
unsigned int Kisak_VulkanLogStats(bool reset);

// r_vkStats: prints the Vulkan device's frame statistics (frames a second,
// draws and submissions per frame, where the GPU's time went) since the
// command last ran, or since the device was made, and starts them again, so
// one launch can measure one stretch after another. "r_vkStats keep" prints
// them without starting again. With r_backend d3d9 there is nothing to print.
void R_Cmd_VulkanStats()
{
	const bool keep = Cmd_Argc() > 1 && !I_stricmp(Cmd_Argv(1), "keep");
	if (!Kisak_VulkanLogStats(!keep))
		Com_Printf(CON_CHANNEL_GFX, "r_vkStats: there is no Vulkan device (r_backend is not vulkan)\n");
}
cmd_function_s R_Cmd_VulkanStats_VAR;
#endif

cmd_function_s R_Cmd_Screenshot_VAR;
cmd_function_s R_Cmd_ScreenshotJpeg_VAR;
cmd_function_s R_ImageList_f_VAR;
cmd_function_s R_Cmd_ApplyPicmip_VAR;
cmd_function_s R_Cmd_ReloadMaterialTextures_VAR;
cmd_function_s R_Cmd_LoadSun_VAR;
cmd_function_s R_Cmd_SaveSun_VAR;
cmd_function_s R_MaterialList_f_VAR;
cmd_function_s R_ModelList_f_VAR;
cmd_function_s RB_Stats_f_VAR;
cmd_function_s R_StaticModelCacheStats_f_VAR;
cmd_function_s R_StaticModelCacheFlush_f_VAR;

void R_RegisterCmds()
{
	Cmd_AddCommandInternal("screenshot", R_Cmd_Screenshot, &R_Cmd_Screenshot_VAR);
	Cmd_AddCommandInternal("screenshotJpeg", R_Cmd_ScreenshotJpeg, &R_Cmd_ScreenshotJpeg_VAR);
	Cmd_AddCommandInternal("imagelist", R_ImageList_f, &R_ImageList_f_VAR);
	Cmd_AddCommandInternal("r_applyPicmip", R_Cmd_ApplyPicmip, &R_Cmd_ApplyPicmip_VAR);
	Cmd_AddCommandInternal("reloadmaterialtextures", R_Cmd_ReloadMaterialTextures, &R_Cmd_ReloadMaterialTextures_VAR);
	Cmd_AddCommandInternal("r_loadsun", R_Cmd_LoadSun, &R_Cmd_LoadSun_VAR);
	Cmd_AddCommandInternal("r_savesun", R_Cmd_SaveSun, &R_Cmd_SaveSun_VAR);
	Cmd_AddCommandInternal("gfx_world", R_MaterialList_f, &R_MaterialList_f_VAR);
	Cmd_AddCommandInternal("gfx_model", R_ModelList_f, &R_ModelList_f_VAR);
	Cmd_AddCommandInternal("gfx_stats", RB_Stats_f, &RB_Stats_f_VAR);
	Cmd_AddCommandInternal("r_smc_stats", R_StaticModelCacheStats_f, &R_StaticModelCacheStats_f_VAR);
	Cmd_AddCommandInternal("r_smc_flush", R_StaticModelCacheFlush_f, &R_StaticModelCacheFlush_f_VAR);
#if defined(KISAK_VULKAN_BACKEND)
	Cmd_AddCommandInternal("r_vkStats", R_Cmd_VulkanStats, &R_Cmd_VulkanStats_VAR);
#endif
}

void R_UnregisterCmds()
{
	Cmd_RemoveCommand("r_applyPicmip");
	Cmd_RemoveCommand("screenshot");
	Cmd_RemoveCommand("screenshotJpeg");
	Cmd_RemoveCommand("imagelist");
	Cmd_RemoveCommand("reloadmaterialtextures");
	Cmd_RemoveCommand("r_loadsun");
	Cmd_RemoveCommand("r_savesun");
	Cmd_RemoveCommand("gfx_world");
	Cmd_RemoveCommand("gfx_model");
	Cmd_RemoveCommand("gfx_stats");
	Cmd_RemoveCommand("r_smc_stats");
	Cmd_RemoveCommand("r_smc_flush");
#if defined(KISAK_VULKAN_BACKEND)
	Cmd_RemoveCommand("r_vkStats");
#endif
}

