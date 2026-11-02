// server_assets.cpp -- engine functions the shared database sources call that a
// native target without their owners still needs, and that need no device.
//
// The native Linux target (scripts/platform/linux/CMakeLists.txt) builds the
// shared simulation, filesystem and database sources, but not the renderer,
// client, UI, sound or FX sources. Asset loading lives in src/database: a server
// still has to load code_post_gfx_mp, common_mp, the localized variants and the
// map zones into the XAsset registry, because that is where weapon, xmodel,
// clipmap, comworld and gamemap data come from. Loading a zone calls into the
// renderer, FX and sound code, and the definitions those calls need are split
// across four files so that a target linking some of the owners can leave the
// matching file out:
//
//   server_device_counterparts.cpp    the dedicated renderer start
//                                     (CL_InitDedicated, R_MakeDedicated, the
//                                     fast-file manifest) and the
//                                     r_loadForRenderer-off counterparts for the
//                                     owners that create device objects.
//   server_renderer_counterparts.cpp  the r_loadForRenderer dvar and the
//                                     counterparts whose owners are r_dvars.cpp,
//                                     r_buffers.cpp, r_rendercmds.cpp,
//                                     r_xsurface.cpp and r_bsp_load_obj.cpp.
//   server_draw_counterparts.cpp      R_RegisterModel, the shader-cache clears,
//                                     the static model cache check and
//                                     Load_PicmipWater, whose owners a target
//                                     links only when it draws.
//   this file                         what every native target that loads zones
//                                     needs: FX_Register and the sound archiving
//                                     no-ops.
//
// The world device probe (scripts/platform/linux/world_device_probe.cmake) links
// this file and server_draw_counterparts.cpp and leaves the other two out. The
// world draw probe (scripts/platform/linux/world_draw_probe.cmake) links only
// this file.
//
// Not implemented here on purpose: renderer functions the dedicated data path
// never reaches. The target links with -Wl,--gc-sections so that code which only
// the client reaches is discarded, and a placeholder for such a function would
// replace the linker's silence with invented behavior. Where a renderer function
// *is* called, the definition below either performs the real CPU-side work or
// stops with ERR_FATAL.

#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <universal/com_memory.h>
#include <universal/profile.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <qcommon/threads.h>

#include <database/database.h>

#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_xsurface.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/rb_uploadshaders.h>
#include <gfx_d3d/r_rendercmds.h>

#include <EffectsCore/fx_system.h>
#include <sound/snd_public.h>

// ---------------------------------------------------------------------------
// Name-keyed effect registration.
// ---------------------------------------------------------------------------

// src/EffectsCore/fx_load_obj.cpp:1221. In the multiplayer sources the config-string
// field that would call this is already gated on com_dedicated
// (universal/q_shared.cpp:1035), so a server only needs the fast-file lookup.
const FxEffectDef *__cdecl FX_Register(const char *name)
{
    if (!IsFastFileLoad())
        Com_Error(ERR_FATAL, "FX_Register('%s'): loading effects from source is not supported in the dedicated build\n", name);

    if (!I_strncmp(name, "fx/", 3))
        MyAssertHandler(
            ".\\EffectsCore\\fx_load_obj.cpp",
            1330,
            0,
            "%s\n\t(name) = %s",
            "(I_strncmp( name, \"fx/\", 3 ))",
            name);

    return DB_FindXAssetHeader(ASSET_TYPE_FX, name).fx;
}

// ---------------------------------------------------------------------------
// Sound channel archiving.
//
// src/sound/snd.cpp:1148 and 1180 exist so that a zone unload does not leave the
// active sound channels holding alias pointers into freed zone memory: each
// channel's alias is replaced by its alias list plus an offset, then restored.
// A dedicated server has no sound system -- g_snd, the channels and the SND_*
// archiving helpers all live in the excluded src/sound sources -- so no channel
// can hold an alias and there is no alias state to preserve across a zone unload.
// ---------------------------------------------------------------------------

void __cdecl DB_SaveSounds()
{
}

void __cdecl DB_LoadSounds()
{
}
