// Link boundary for KisakCOD-world-frontend-probe.
//
// The probe is the offline asset exporter (the KisakCOD-dedi sources) with the
// production renderer front end linked in (scripts/platform/linux/
// world_frontend_probe.cmake). The dedicated server already owns the host
// services, the database, the collision map, dvars, commands and memory, and
// the renderer units replace the dedicated counterparts in
// src/linux/server_renderer_counterparts.cpp. What is left, the symbols no
// linked unit defines, is defined here in exactly two kinds:
//
//  1. Storage whose owner does not compile on Linux yet: r_init.cpp holds the
//     device globals and the render-thread state, and win_wndproc.cpp the window
//     dvars. Each object has the owner's exact type and is zero-initialized,
//     which is what the owner's definition is before its init code runs: no
//     device, no render thread, rendering not disabled. With r_glob zeroed, the
//     real R_BeginRemoteScreenUpdate, R_SyncRenderThread and
//     R_ReleaseThreadOwnership take the "no render thread" branch that the
//     dedicated counterparts reproduce. The window dvars are registered by
//     R_RegisterDvars.
//
//  2. Functions the probe must never reach. Each aborts with its name, so a
//     path the probe has no data for is a failure, not a skipped feature:
//     device errors and surface release (no device exists, and the probe checks
//     r_loadForRenderer is off before loading), client and cgame state (no
//     client runs), entity posing, skinning and bounds (the probe adds no scene
//     entity), effects, model lighting, shadows, debug drawing and material
//     technique overrides.

#include <universal/q_shared.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_model_lighting.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_spotshadow.h>
#include <gfx_d3d/r_staticmodelcache.h>
#include <gfx_d3d/r_utils.h>

#include <cstdio>
#include <cstdlib>

// ---- 1. storage owned by units that do not compile on Linux yet ---------------

DxGlobals dx;                 // r_init.cpp
GfxGlobals r_glob;            // r_init.cpp
int g_disableRendering;       // r_init.cpp
const dvar_t *r_fullscreen;   // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_xpos;       // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_ypos;       // win_wndproc.cpp on Windows; R_RegisterDvars registers it

// ---- 2. paths the probe must never reach -----------------------------------

namespace
{
[[noreturn]] void Unreachable(const char *what)
{
    std::fprintf(stderr, "world_frontend_probe: reached %s, which this probe has no data for\n", what);
    std::fflush(nullptr);
    std::abort();
}
} // namespace

#define KISAK_UNREACHABLE(ret, name, params) \
    ret name params { Unreachable(#name); }

// No device exists: nothing is created, so nothing fails or is released.
KISAK_UNREACHABLE(const char *, R_ErrorDescription, (HRESULT))
KISAK_UNREACHABLE(void, R_FatalInitError, (const char *))
KISAK_UNREACHABLE(uint32_t, R_ReleaseD3DSurface, (IDirect3DSurface9 *))
// No client or cgame runs in this process.
KISAK_UNREACHABLE(bool, CL_IsLocalClientInGame, (int32_t))
KISAK_UNREACHABLE(int, CG_CullIn, (cpose_t *))
KISAK_UNREACHABLE(void, CG_UsedDObjCalcPose, (cpose_t *))
// The probe adds no scene entity, effect, light or shadow.
KISAK_UNREACHABLE(void, FX_GenerateVerts, (FxGenerateVertsCmd *))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForWorld, (int))
KISAK_UNREACHABLE(void, R_ProcessCmd_UpdateFxSpotLight, (FxCmd *))
KISAK_UNREACHABLE(void, R_ProcessCmd_UpdateFxNonDependent, (FxCmd *))
KISAK_UNREACHABLE(void, R_ProcessCmd_UpdateFxRemaining, (FxCmd *))
KISAK_UNREACHABLE(void, R_GenerateShadowCookiesCmd, (ShadowCookieCmd *))
KISAK_UNREACHABLE(void, R_AddSpotShadowEntCmd, (const GfxSpotShadowEntCmd *))
KISAK_UNREACHABLE(void, R_ShowLightVisCachePoints, (const float *, const DpvsPlane *, int))
KISAK_UNREACHABLE(void, R_SkinXModelCmd, (uint16_t *))
KISAK_UNREACHABLE(void, R_SkinGfxEntityCmd, (GfxSceneEntity **))
KISAK_UNREACHABLE(void, R_SkinCachedStaticModelCmd, (SkinCachedStaticModelCmd *))
KISAK_UNREACHABLE(void, R_UpdateGfxEntityBoundsCmd, (GfxSceneEntity **))
KISAK_UNREACHABLE(void, R_SkinSceneDObj, (GfxSceneEntity *, GfxSceneEntity *, const DObj_s *, DObjAnimMat *, int))
KISAK_UNREACHABLE(void, R_UpdateSceneEntBounds, (GfxSceneEntity *, GfxSceneEntity **, const DObj_s **, int))
KISAK_UNREACHABLE(uint32_t, R_AllocModelLighting_Box,
                  (const GfxViewInfo *, float *, const float *, const float *, uint16_t *, GfxLightingInfo *))
KISAK_UNREACHABLE(uint32_t, R_AllocModelLighting_Sphere,
                  (const GfxViewInfo *, float *, const float *, float, uint16_t *, GfxLightingInfo *))
KISAK_UNREACHABLE(uint32_t, R_AllocModelLighting_PrimaryLight, (float *, uint32_t, uint16_t *, GfxLightingInfo *))
KISAK_UNREACHABLE(void, Material_PreventOverrideTechniqueGeneration, ())
