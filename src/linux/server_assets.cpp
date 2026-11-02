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
// across three files so that a target linking some of the owners can leave the
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
//   this file                         what every native target that loads zones
//                                     needs: R_RegisterModel, the shader-cache
//                                     clears, the static model cache check,
//                                     Load_PicmipWater, FX_Register and the sound
//                                     archiving no-ops.
//
// The world device probe (scripts/platform/linux/world_device_probe.cmake) links
// this file and leaves the other two out. It creates a device but never draws,
// registers its world only after its last zone load and unloads no zone, so what
// the shader-cache clears and the static model cache check below rely on holds
// there too; a target that draws must link rb_shade.cpp and
// r_staticmodelcache.cpp instead.
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
#include <gfx_d3d/r_model.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_xsurface.h>
#include <gfx_d3d/r_water.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/rb_uploadshaders.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/rb_shade.h>
#include <gfx_d3d/r_staticmodelcache.h>

#include <EffectsCore/fx_system.h>
#include <sound/snd_public.h>

// ---------------------------------------------------------------------------
// Shared model setup.
//
// R_RegisterModel lives in src/gfx_d3d but contains no renderer state: it
// precaches an XModel through the database and the hunk. The server needs it
// because config-string field parsing (src/universal/q_shared.cpp, CSPFT_XMODEL)
// calls it without testing com_dedicated -- unlike the FX and material fields
// next to it, which are gated on com_dedicated. Because its whole effect is
// CPU-side, it is extracted verbatim rather than stubbed.
// ---------------------------------------------------------------------------

// src/gfx_d3d/r_model.cpp:151.
XModel *__cdecl R_RegisterModel(const char *name)
{
    return XModelPrecache(
        (char *)name,
        (void *(__cdecl *)(int))Hunk_AllocXModelPrecache,
        (void *(__cdecl *)(int))Hunk_AllocXModelPrecacheColl);
}

// ---------------------------------------------------------------------------
// Renderer calls the database makes that need no device.
// ---------------------------------------------------------------------------

// src/gfx_d3d/rb_shade.cpp:53/96/138 clear the shader and vertex declaration
// cached in gfxCmdBufState. Only drawing binds them, and nothing here draws: the
// dedicated build has no device and the world device probe issues no draw. The
// original's own assertion (prim.vertexDecl == NULL) holds trivially.
void __cdecl RB_ClearPixelShader()
{
}

void __cdecl RB_ClearVertexShader()
{
}

void __cdecl RB_ClearVertexDecl()
{
}

// src/gfx_d3d/r_staticmodelcache.cpp:605 walks the registered world's static
// model draw instances. The dedicated build registers no world (rgp.world stays
// null), which is the single condition the original body tests. The world device
// probe gets here from its synchronous map load (DB_LoadXAssets archives assets
// first) before it registers the map's world, and it unloads no zone afterwards.
void __cdecl R_ClearAllStaticModelCacheRefs()
{
    if (rgp.world)
        Com_Error(ERR_FATAL, "R_ClearAllStaticModelCacheRefs: a GfxWorld was registered in a dedicated build\n");
}

// src/gfx_d3d/r_water.cpp:292, extracted verbatim. This is CPU work on the water
// simulation grid stored in the zone, not device work, and the shared material
// loader calls it for every water texture, so the grid is reduced to the same
// dimensions the client would produce.
void __cdecl Load_PicmipWater(water_t **waterRef)
{
    complex_s *H0;
    float real;
    float imag;
    complex_s *v4;
    int v5;
    int v6;
    int m;
    int srcIndex;
    int downsample;
    int n;
    int dstIndex;

    if ((*waterRef)->M >> r_picmip_water->current.integer < 4)
        v6 = 4;
    else
        v6 = (*waterRef)->M >> r_picmip_water->current.integer;
    if ((*waterRef)->N >> r_picmip_water->current.integer < 4)
        v5 = 4;
    else
        v5 = (*waterRef)->N >> r_picmip_water->current.integer;
    if (v6 != (*waterRef)->M || v5 != (*waterRef)->N)
    {
        iassert((*waterRef)->M == (*waterRef)->N);
        downsample = (*waterRef)->M / v6;
        (*waterRef)->M = v6;
        (*waterRef)->N = v5;
        srcIndex = 0;
        dstIndex = 0;
        for (m = 0; m < v6; ++m)
        {
            for (n = 0; n < v5; ++n)
            {
                H0 = (*waterRef)->H0;
                real = H0[srcIndex].real;
                imag = H0[srcIndex].imag;
                v4 = (*waterRef)->H0;
                v4[dstIndex].real = real;
                v4[dstIndex++].imag = imag;
                srcIndex += downsample;
            }
            srcIndex += v6 * downsample * (downsample - 1);
        }
    }
}

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
