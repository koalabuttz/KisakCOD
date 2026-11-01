// Link boundary for world_frontend_test.cpp.
//
// The test links the production renderer front end (DPVS, BSP draw-surface
// batching, the draw-surface merge), the scene setup in r_scene.cpp and the
// buffer storage in r_buffers.cpp (both compiled through the native D3D9
// boundary), and the runtime they need (dvars, commands, strings, math, memory,
// threads). Everything else those translation units can reach is defined here,
// in exactly three kinds, so nothing is silently faked:
//
//  1. Host services the test owns, as every native test host in this tree does
//     (src/linux/client/diagnostic_host.cpp): console output, and assertions and
//     errors that FAIL the run instead of being survived.
//
//  2. Global storage whose owning translation unit cannot be linked here:
//     r_init.cpp does not compile on Linux yet (monitor and window-style code the
//     SDL host replaces); db_registry.cpp, common.cpp, sv_main_mp.cpp and the
//     Windows window code pull in the whole engine. Each object below has the
//     owner's exact type and is zero-initialized, which is what the owner's
//     definition is before its init code runs. Dvar pointers are registered by
//     R_RegisterDvars or by the test's main(), or stay null as they do before
//     their owner's init. (Where the owner links cleanly it is linked instead:
//     `scene` comes from r_scene.cpp and `gfxBuf` from r_buffers.cpp (both
//     through the boundary), the collision map `cm` from cm_load.cpp and the
//     dynamic-entity accessors from DynEntity_load_obj.cpp, so an empty world
//     returns what a real one with no dynamic entities would.)
//
//  3. Functions the synthetic world must never reach: entity posing and
//     culling, effects, model lighting, skinning, shadows, debug drawing and
//     localization. Each one aborts with its name. The world has no entities,
//     effects or static models and the debug dvars stay at their defaults, so
//     reaching any of these is a test failure, not a skipped feature. (The
//     model, brush-model and DObj surface adders are production code from
//     r_scene.cpp now, with their accessors linked from their owners.)

#include <universal/q_shared.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/fxprimitives.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_model_lighting.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_spotshadow.h>
#include <gfx_d3d/r_staticmodelcache.h>
#include <qcommon/qcommon.h>
#include <stringed/stringed_hooks.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// ---- 1. host services --------------------------------------------------------

void MyAssertHandler(const char *filename, int line, int type, const char *fmt, ...)
{
    std::fprintf(stderr, "FAIL: production assertion %s:%d (type %d): ", filename, line, type);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::abort();
}

void Com_Error(errorParm_t code, const char *fmt, ...)
{
    std::fprintf(stderr, "FAIL: Com_Error(%d): ", static_cast<int>(code));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::abort();
}

void Com_Printf(int channel, const char *fmt, ...)
{
    std::printf("[con %d] ", channel);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

void Com_PrintMessage(int channel, const char *msg, int error)
{
    std::printf("[con %d%s] %s", channel, error ? " error" : "", msg);
}

void Com_PrintWarning(int channel, const char *fmt, ...)
{
    std::printf("[con %d warning] ", channel);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

void Com_PrintError(int channel, const char *fmt, ...)
{
    std::printf("[con %d error] ", channel);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

// No console log file exists in this host; that is the true state, not a stub.
bool Com_LogFileOpen()
{
    return false;
}

// The fill helper behind Com_Memset lives in common.cpp, which is not linked.
void _copyDWord(uint32_t *dest, const uint32_t constant, const uint32_t count)
{
    std::fill_n(dest, count, constant);
}

// ---- 2. storage owned by translation units that are not linked ---------------

DxGlobals dx;                         // r_init.cpp
r_global_permanent_t rgp;             // r_init.cpp
GfxConfiguration gfxCfg;              // r_init.cpp
r_globals_t rg;                       // db_registry.cpp (moved there for DEDICATED)
const dvar_t *sys_smp_allowed;        // common.cpp, registered by the test
const dvar_t *com_statmon;            // common.cpp
const dvar_t *com_logfile;            // common.cpp
const dvar_t *sv_cheats;              // sv_main_mp.cpp
const dvar_t *r_fullscreen;           // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_xpos;               // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_ypos;               // win_wndproc.cpp on Windows; R_RegisterDvars registers it

// ---- 3. paths the synthetic world must never reach ---------------------------

namespace
{
[[noreturn]] void Unreachable(const char *what)
{
    std::fprintf(stderr, "FAIL: the world front-end test reached %s, which it has no data for\n", what);
    std::abort();
}
} // namespace

#define KISAK_UNREACHABLE(ret, name, params) \
    ret name params { Unreachable(#name); }

KISAK_UNREACHABLE(int, BoxOnPlaneSide, (const float *, const float *, const cplane_s *))
KISAK_UNREACHABLE(void, NET_Sleep, (int))
KISAK_UNREACHABLE(int, CG_CullIn, (cpose_t *))
KISAK_UNREACHABLE(void, CG_UsedDObjCalcPose, (cpose_t *))
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
// Only the setfromlocstring console command reaches localization; the test runs no console commands.
KISAK_UNREACHABLE(char *, SEH_LocalizeTextMessage, (const char *, const char *, msgLocErrType_t))
