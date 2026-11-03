// Link boundary of the world draw probe (scripts/platform/linux/world_draw_probe.cmake).
//
// The probe links the whole renderer except the four units the client compile
// census still blocks, the effects system with the physics it steps, the
// cgame's vision sets, and none of the rest of the client: no other cgame unit,
// no dynamic-entity simulation, no sound, no client screen. The free camera's
// effects host (engine_effects.h) and vision host (engine_vision.h) make the
// cgame's calls themselves. What the linked units reach that no linked unit
// defines is here, in four kinds:
//
//   1. Storage whose owner is not linked, with the owner's exact type,
//      zero-initialized as the owner's definition is before its init runs.
//   2. Calls the scene and the effects system make into client systems this
//      process never starts. R_BeginFrame, R_GenerateSortedDrawSurfs,
//      R_EndFrame and the effects update call them every frame without testing
//      whether the system runs, as the engine's own cgame-free scene render
//      (R_GenerateReflectionRawData) does. Each definition does what its owner
//      does when that system holds nothing, says which owner state that is, and
//      checks it where the renderer can see it, in the manner of
//      server_assets.cpp's sound archiving no-ops.
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
#include <cgame_mp/cg_local_mp.h>
#include <DynEntity/DynEntity_client.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_screenshot.h>
#include <sound/snd_public.h>

#include <cstdio>
#include <cstdlib>

// ---- 1. storage ------------------------------------------------------------
const dvar_t *onlinegame;   // cl_main_mp.cpp; the probe's CL_InitDedicated registers it
const dvar_t *r_fullscreen; // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_xpos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_ypos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it
// cg_main_mp.cpp. The effects system reads the view from cgArray[0].refdef,
// which the effects host fills every frame; the cgame's entities stay empty, as
// no snapshot arrives, and cgMedia holds nothing, as CG_RegisterGraphics and
// CG_RegisterSounds never run.
cg_s cgArray[1];
centity_s cg_entitiesArray[1][1024];
cgMedia_t cgMedia;
// cl_main_mp.cpp. The cgame's vision sets ask whether the client is in a game
// (CL_GetLocalClientConnectionState) before looking through night vision; no
// client connects here, so it stays CA_DISCONNECTED.
clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];

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
void CL_UpdateSound()
{
}

// src/client/cl_debugdata.cpp: both bodies are guarded by cls.rendererStarted,
// which is false in a process with no client, so they do nothing.
void CL_FlushDebugClientData()
{
}

void CL_UpdateDebugClientData()
{
}

// src/cgame/cg_draw_debug.cpp: appends the frame time to the cgame's FPS
// history, which only the cgame's FPS display reads. No cgame runs here.
void CG_CalculateFPS()
{
}

// src/cgame_mp/cg_draw_mp.cpp, which FX_UpdateRemaining calls after the
// effects: the local entities' tracer beams (CG_AddLocalEntityTracerBeams),
// then the players' sprites and the 3D HUD elements. No cgame runs here, so no
// weapon fires a tracer, no player is in the scene and no HUD exists.
void CG_AddSceneTracerBeams(int32_t)
{
}

void CG_GenerateSceneVerts(int32_t)
{
}

// src/sound/snd.cpp: whether any sound listener is active. No sound system is
// started here, so none is, and FX_SpawnSound plays none of an effect's sounds.
char SND_AnyActiveListeners()
{
    return 0;
}

// src/EffectsCore/fx_load_obj.cpp, which FX_InitSystem and FX_ShutdownSystem
// call: clears the registry of effects loaded from source files. The probe
// loads fast files only, and FX_Register (server_assets.cpp) finds effects in
// them without that registry, so it is always empty.
void FX_UnregisterAll()
{
}


// src/DynEntity/DynEntity_client.cpp: for each client model dynamic entity with
// a physics body, copies the body's interpolated state into its pose. The map's
// dynamic entities come with its clip map, posed where the map placed them; only
// the cgame's physics gives one a body, and no cgame runs here. The loop then
// does nothing, which this checks.
void DynEntCl_ProcessEntities(int32_t localClientNum)
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
void DynEntPieces_AddDrawSurfs()
{
}

// ---- 3. Windows-only renderer services, as Linux has them -------------------

// src/gfx_d3d/r_cinematic.cpp plays Bink videos, and deps/binklib is Windows-only.
// Cinematics are unavailable on Linux: none ever starts, so the per-frame update
// has nothing to advance, the lost-device pair has no Bink textures to release
// or remake, and the scene's cinematic warning never shows.
void R_Cinematic_Init()
{
}

void R_Cinematic_UpdateFrame()
{
}

bool R_Cinematic_IsStarted()
{
    return false;
}

bool R_Cinematic_IsUnderrun()
{
    return false;
}

void R_Cinematic_BeginLostDevice()
{
}

void R_Cinematic_EndLostDevice()
{
}

// R_ShutdownDirect3D's: with no playback and no Bink textures there is nothing to
// stop or release (the free-camera adapter's Shutdown reaches it through R_Shutdown).
void R_Cinematic_Shutdown()
{
}

// src/gfx_d3d/r_texturemem.cpp:150 and R_DetectCurrentTextureMemory below it,
// with R_VideoMemory's DirectDraw query answering 0: DirectDraw does not exist
// on Linux, and 0 is what R_VideoMemory returns when DirectDraw cannot tell. The
// rest is the original's own branch for that case and its reporting cache.
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
// Scene entities and marks on them: the probe adds no entity to the scene, and
// fx_marks is off.
KISAK_UNREACHABLE(void, CG_CullIn, (cpose_t *))
KISAK_UNREACHABLE(void, CG_UsedDObjCalcPose, (cpose_t *))
KISAK_UNREACHABLE(DObjAnimMat *, CG_DObjCalcPose, (const cpose_t *, const DObj_s *, int *))
KISAK_UNREACHABLE(float *, CG_GetEntityOrigin, (int, uint32_t))
KISAK_UNREACHABLE(void, CG_GetPoseOrigin, (const cpose_t *, float *))
KISAK_UNREACHABLE(void, CG_GetPoseAngles, (const cpose_t *, float *))
// Effects bolted to an entity's bone: the effects host spawns every effect
// unbolted (ENTITYNUM_NONE), and an effect's children inherit its bolt.
KISAK_UNREACHABLE(cpose_t *, CG_GetPose, (int32_t, uint32_t))
KISAK_UNREACHABLE(void, CG_GetDObjOrientation, (int32_t, int32_t, mat3x3 &, float *))
KISAK_UNREACHABLE(int32_t, CG_DObjGetWorldBoneMatrix, (const cpose_t *, DObj_s *, int32_t, float (*)[3], float *))
// Sounds: SND_AnyActiveListeners answers that no listener is active, so
// FX_SpawnSound plays nothing, and cgMedia holds no collision sounds, so
// Phys_PlayCollisionSound finds none to play.
KISAK_UNREACHABLE(int32_t, CG_PlaySoundAlias, (int32_t, int32_t, const float *, snd_alias_list_t *))
KISAK_UNREACHABLE(void, CG_AddFXSoundAlias, (int32_t, const float *, snd_alias_list_t *))
KISAK_UNREACHABLE(void, SND_AddPhysicsSound, (snd_alias_list_t *, float *))
// The physics and collision debug drawing, which only their debug dvars
// (phys_drawContacts, phys_drawCollisionWorld, ...) turn on.
KISAK_UNREACHABLE(void, CG_DebugLine, (const float *, const float *, const float *, int32_t, int32_t))
KISAK_UNREACHABLE(void, CG_DebugBox,
                  (const float *, const float *, const float *, float, const float *, int32_t, int32_t))
KISAK_UNREACHABLE(void, CG_DebugBoxOriented,
                  (const float *, const float *, const float *, const mat3x3 &, const float *, int32_t, int32_t))
KISAK_UNREACHABLE(void, CG_DebugCircle, (const float *, float, const float *, const float *, int32_t, int32_t))
// Loading from source (r_material_load_obj.cpp needs D3DX, and the BSP loader's
// source path): the probe loads fast files only, and these paths test
// IsFastFileLoad first.
KISAK_UNREACHABLE(Material *, Material_Load, (char *, int))
KISAK_UNREACHABLE(MaterialTechniqueSet *, Material_FindTechniqueSet_LoadObj, (const char *, MtlTechSetNotFoundBehavior))
KISAK_UNREACHABLE(void, Material_PreLoadAllShaderText, ())
KISAK_UNREACHABLE(Material *, R_GetBspMaterial, (uint32_t))
KISAK_UNREACHABLE(void, SCR_UpdateLoadScreen, ())
// R_Shutdown's Material_Shutdown, which it calls only when not loading from fast files.
KISAK_UNREACHABLE(void, Material_FreeAll, ())
// The screenshot console command (r_screenshot.cpp is not linked). The probe
// takes no screenshot and runs no console command.
KISAK_UNREACHABLE(void, R_ScreenshotCommand, (GfxScreenshotType))
