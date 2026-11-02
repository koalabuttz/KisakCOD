// Link boundary of the world draw probe (scripts/platform/linux/world_draw_probe.cmake).
//
// The probe links the whole renderer except the four units the client compile
// census still blocks, and none of the client: no cgame, no effects system, no
// dynamic-entity simulation, no sound, no client screen. What the linked units
// reach that no linked unit defines is here, in four kinds:
//
//   1. Storage whose owner is not linked, with the owner's exact type,
//      zero-initialized as the owner's definition is before its init runs.
//   2. Per-frame calls the scene makes into client systems this process never
//      starts. R_BeginFrame, R_GenerateSortedDrawSurfs and R_EndFrame call them
//      every frame without testing whether the system runs, as the engine's
//      own cgame-free scene render (R_GenerateReflectionRawData) does. Each
//      definition does what its owner does when that system holds nothing, says
//      which owner state that is, and checks it where the renderer can see it,
//      in the manner of server_assets.cpp's sound archiving no-ops.
//   3. The two renderer services whose owners are Windows-only, as Linux has
//      them: cinematics (r_cinematic.cpp, Bink) are unavailable, so none is
//      ever playing; video memory (r_texturemem.cpp, DirectDraw) is the
//      engine's own branch for when DirectDraw reports none.
//   4. Functions the probe must never reach. Each aborts with its name, so a
//      path that reaches one fails loudly instead of running invented code.
//
// Every entry was needed for the link when it was added. Compiled through the
// native D3D9 boundary, because R_AvailableTextureMemory asks the device.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <DynEntity/DynEntity_client.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_screenshot.h>

#include <cstdio>
#include <cstdlib>

// ---- 1. storage ------------------------------------------------------------
const dvar_t *onlinegame;   // cl_main_mp.cpp; the probe's CL_InitDedicated registers it
const dvar_t *r_fullscreen; // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_xpos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_ypos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it

namespace
{
[[noreturn]] void Unreachable(const char *what)
{
    std::fprintf(stderr, "world_draw_probe: reached %s, which this probe does not link\n", what);
    std::fflush(nullptr);
    std::abort();
}

[[noreturn]] void Broken(const char *what, const char *why)
{
    std::fprintf(stderr, "world_draw_probe: %s: %s\n", what, why);
    std::fflush(nullptr);
    std::abort();
}
} // namespace

// ---- 2. client systems this process never starts -------------------------------

// src/client_mp/cl_main_mp.cpp: SND_PlayFXSounds, SND_UpdateLoopingSounds,
// SND_Update. No sound system is started here (src/sound is not linked), so no
// sound plays and no listener exists to move.
void __cdecl CL_UpdateSound()
{
}

// src/client/cl_debugdata.cpp: both bodies are guarded by cls.rendererStarted,
// which is false in a process with no client, so they do nothing.
void __cdecl CL_FlushDebugClientData()
{
}

void __cdecl CL_UpdateDebugClientData()
{
}

// src/cgame/cg_draw_debug.cpp: appends the frame time to the cgame's FPS
// history, which only the cgame's FPS display reads. No cgame runs here.
void __cdecl CG_CalculateFPS()
{
}

// src/EffectsCore/fx_update.cpp: steps the effects physics world to the effects
// system's time. The effects system is never started here (FX_InitSystem is not
// linked), so no effect has a physics body to step.
void __cdecl FX_RunPhysics(int32_t)
{
}

// src/DynEntity/DynEntity_client.cpp: for each client model dynamic entity with
// a physics body, copies the body's interpolated state into its pose. The map's
// dynamic entities come with its clip map, posed where the map placed them; only
// the cgame's physics gives one a body, and no cgame runs here. The loop then
// does nothing, which this checks.
void __cdecl DynEntCl_ProcessEntities(int32_t localClientNum)
{
    if (localClientNum != 0)
        return;
    const uint16_t count = DynEnt_GetEntityCount(DYNENT_COLL_CLIENT_FIRST);
    for (uint16_t id = 0; id < count; ++id)
    {
        const DynEntityClient *client = DynEnt_GetClientEntity(id, DYNENT_DRAW_MODEL);
        if ((client->flags & 1) && client->physObjId)
            Broken("DynEntCl_ProcessEntities", "a dynamic entity has a physics body, but no physics runs here");
    }
}

// src/DynEntity/DynEntity_pieces.cpp: draws the pieces of broken dynamic
// entities. Only damage breaks one, and nothing here deals damage or links the
// pieces pool, so there are none to draw.
void __cdecl DynEntPieces_AddDrawSurfs()
{
}

// ---- 3. Windows-only renderer services, as Linux has them -------------------

// src/gfx_d3d/r_cinematic.cpp plays Bink videos, and deps/binklib is Windows-only.
// Cinematics are unavailable on Linux: none ever starts, so the per-frame update
// has nothing to advance, the lost-device pair has no Bink textures to release
// or remake, and the scene's cinematic warning never shows.
void __cdecl R_Cinematic_Init()
{
}

void __cdecl R_Cinematic_UpdateFrame()
{
}

bool __cdecl R_Cinematic_IsStarted()
{
    return false;
}

bool __cdecl R_Cinematic_IsUnderrun()
{
    return false;
}

void __cdecl R_Cinematic_BeginLostDevice()
{
}

void __cdecl R_Cinematic_EndLostDevice()
{
}

// src/gfx_d3d/r_texturemem.cpp:150 and R_DetectCurrentTextureMemory below it,
// with R_VideoMemory's DirectDraw query answering 0: DirectDraw does not exist
// on Linux, and 0 is what R_VideoMemory returns when DirectDraw cannot tell. The
// rest is the original's own branch for that case and its reporting cache.
static uint32_t s_maxReportedTexMem;

uint32_t __cdecl R_AvailableTextureMemory()
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

// ---- 4. functions this probe must never reach --------------------------------

#define KISAK_UNREACHABLE(ret, name, params) \
    ret name params { Unreachable(#name); }

// The render thread is never started (the probe calls R_Init, not
// R_BeginRegistration), so no remote screen update asks whether a client is in
// game or draws the client's loading screen.
KISAK_UNREACHABLE(bool, CL_IsLocalClientInGame, (int32_t))
KISAK_UNREACHABLE(void, SCR_UpdateScreen, ())
// The backend's text cursor blink; the probe draws no 2D text.
KISAK_UNREACHABLE(int32_t, CL_ScaledMilliseconds, ())
// Scene entities: the probe adds none to the scene.
KISAK_UNREACHABLE(void, CG_CullIn, (cpose_t *))
KISAK_UNREACHABLE(void, CG_UsedDObjCalcPose, (cpose_t *))
KISAK_UNREACHABLE(DObjAnimMat *, CG_DObjCalcPose, (const cpose_t *, const DObj_s *, int *))
KISAK_UNREACHABLE(float *, CG_GetEntityOrigin, (int, uint32_t))
KISAK_UNREACHABLE(FxSystem *, FX_GetSystem, (int32_t))
// Effects work: only the cgame queues the effects worker commands, and the probe
// turns fx_marks off, so no mark vertices are generated.
KISAK_UNREACHABLE(void, FX_UpdateSpotLight, (FxCmd *))
KISAK_UNREACHABLE(void, FX_UpdateNonDependent, (FxCmd *))
KISAK_UNREACHABLE(void, FX_UpdateRemaining, (FxCmd *))
KISAK_UNREACHABLE(void, FX_AddNonSpriteDrawSurfs, (FxCmd *))
KISAK_UNREACHABLE(void, FX_EndUpdate, (int32_t))
KISAK_UNREACHABLE(void, FX_FillGenerateVertsCmd, (int32_t, FxGenerateVertsCmd *))
KISAK_UNREACHABLE(void, FX_GenerateVerts, (FxGenerateVertsCmd *))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForWorld, (int32_t))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForStaticModels, (int32_t, int32_t, const uint8_t *))
KISAK_UNREACHABLE(void, FX_BeginGeneratingMarkVertsForEntModels, (int32_t, uint32_t *))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForEntXModel,
                  (int32_t, int32_t, uint32_t *, uint16_t, uint8_t, const GfxScaledPlacement *))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForEntDObj,
                  (int32_t, int32_t, uint32_t *, uint16_t, uint8_t, const DObj_s *, const cpose_t *))
KISAK_UNREACHABLE(void, FX_GenerateMarkVertsForEntBrush, (int32_t, int32_t, uint32_t *, uint8_t, const GfxPlacement *))
KISAK_UNREACHABLE(void, FX_EndGeneratingMarkVertsForEntModels, (int32_t))
// Loading from source (r_material_load_obj.cpp needs D3DX, and the BSP loader's
// source path): the probe loads fast files only, and these paths test
// IsFastFileLoad first.
KISAK_UNREACHABLE(Material *, Material_Load, (char *, int))
KISAK_UNREACHABLE(MaterialTechniqueSet *, Material_FindTechniqueSet_LoadObj, (const char *, MtlTechSetNotFoundBehavior))
KISAK_UNREACHABLE(void, Material_PreLoadAllShaderText, ())
KISAK_UNREACHABLE(Material *, R_GetBspMaterial, (uint32_t))
KISAK_UNREACHABLE(void, SCR_UpdateLoadScreen, ())
// The screenshot console command (r_screenshot.cpp is not linked). The probe
// takes no screenshot and runs no console command.
KISAK_UNREACHABLE(void, R_ScreenshotCommand, (GfxScreenshotType))
