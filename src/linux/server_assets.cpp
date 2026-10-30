// server_assets.cpp -- the asset-loading boundary of the native dedicated server.
//
// The native Linux target (scripts/platform/linux/CMakeLists.txt) builds the
// shared simulation, filesystem and database sources, but not the renderer,
// client, UI, sound or FX sources. Asset loading lives in src/database: a server
// still has to load code_post_gfx_mp, common_mp, the localized variants and the
// map zones into the XAsset registry, because that is where weapon, xmodel,
// clipmap, comworld and gamemap data come from. What a dedicated server must not
// do is create a device, a texture or a GPU vertex/index buffer.
//
// This file therefore supplies exactly three things:
//
//   1. The dedicated entry point CL_InitDedicated and the renderer
//      configuration it needs (GfxConfiguration, SetGfxConfig, R_MakeDedicated
//      and the fast-file manifest that R_LoadGraphicsAssets hands to
//      DB_LoadXAssets). Extracted from src/client_mp/cl_main_mp.cpp and
//      src/gfx_d3d/r_init.cpp without changing the order of operations, so the
//      server loads the same zones the original dedicated server loaded.
//
//   2. The r_loadForRenderer dvar, latched to false. Every renderer function
//      that would touch hardware already tests it first, which is how the
//      original dedicated server kept the zone's vertex/index data in CPU memory
//      and left its GPU buffers null.
//
//   3. Source-level counterparts for the renderer functions that the shared
//      database sources still call while loading and unloading a zone
//      (src/database/db_memory.cpp, db_file_load.cpp, db_registry.cpp).
//
// Not implemented here on purpose: renderer functions the dedicated data path
// never reaches. The target links with -Wl,--gc-sections so that code which only
// the client reaches is discarded, and a placeholder for such a function would
// replace the linker's silence with invented behavior. Where a renderer function
// *is* called, the counterpart below either performs the real CPU-side work or
// stops with ERR_FATAL, because on this build a reachable hardware call can only
// mean the renderer path was entered by mistake.

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
// Renderer globals the dedicated build still needs to define.
//
// The declarations must stay exactly as the renderer's own definitions were
// (src/gfx_d3d/r_init.cpp, src/gfx_d3d/r_dvars.cpp, src/client_mp/cl_main_mp.cpp)
// because the externs in r_init.h, r_dvars.h and client_mp/client_mp.h are what
// the shared sources compile against.
// ---------------------------------------------------------------------------

GfxConfiguration gfxCfg{ 0 };

// Renderer-side world state. Nothing sets rgp.world on this build (that happens
// in R_SetWorldPtr_FastFile, which only R_LoadWorld calls), but the database
// unload path checks it, so the storage has to exist to keep that check honest.
r_global_permanent_t rgp;

const dvar_t *r_loadForRenderer;

// Declared in client_mp/client_mp.h and registered below; the dedicated server
// is the only build that never goes through CL_Init.
const dvar_t *onlinegame;

// Read by the shared asset loaders: r_modelVertColor by the XModel source loader
// (xanim/xmodel_load_obj.cpp:551), r_picmip_water by the water record loader
// (Load_PicmipWater below), and the reflection-probe trio by
// R_ReflectionProbeRegisterDvars. Their definitions live in r_dvars.cpp, which is
// not part of this build.
const dvar_t *r_modelVertColor;
const dvar_t *r_picmip_water;
const dvar_t *r_reflectionProbeGenerate;
const dvar_t *r_reflectionProbeRegenerateAll;
const dvar_t *r_reflectionProbeGenerateExit;

// ---------------------------------------------------------------------------
// Renderer dvars.
//
// The full renderer dvar set belongs to the renderer, which this build does not
// contain. Dedicated keeps only the cvars that the compiled sources actually
// read. src/database, src/qcommon, src/server, src/xanim, src/bgame and the
// remaining shared sources read none of the r_* cvars, so r_loadForRenderer --
// which decides whether the renderer allocates anything at all -- is the whole
// set.
// ---------------------------------------------------------------------------

void __cdecl R_RegisterDvars()
{
    r_loadForRenderer = Dvar_RegisterBool(
        "r_loadForRenderer",
        1,
        DVAR_LATCH,
        "Set to false to disable dx allocations (for dedicated server mode)");

    // Both are read while asset records are loaded from a zone: the model vertex
    // colo(u)r switch from the XModel source loader and the water grid reduction
    // from Load_PicmipWater. Registered with the original defaults, flag bits and
    // ranges so a server sees the same values a client would.
    r_modelVertColor = Dvar_RegisterBool(
        "r_modelVertColor",
        1,
        0xA0u,
        "Set to 0 to replace all model vertex colors with white when loaded");
    r_picmip_water = Dvar_RegisterInt(
        "r_picmip_water",
        0,
        (DvarLimits)0x100000000LL,
        DVAR_LATCH | DVAR_ARCHIVE,
        "Picmip level of water maps.");
}

// src/gfx_d3d/r_dvars.cpp:422, called by Com_InitHunkMemory
// (universal/com_memory.cpp:276) before the hunk is reserved. The original's tail
// calls Material_PreventOverrideTechniqueGeneration when generation is enabled;
// that switches off technique-set overriding for the renderer, which does not
// exist here, so the dvar is registered and the flag is otherwise only read.
void __cdecl R_ReflectionProbeRegisterDvars()
{
    r_reflectionProbeGenerate = Dvar_RegisterBool(
        "r_reflectionProbeGenerate",
        false,
        DVAR_NOFLAG,
        "Generate cube maps for reflection probes.");
    r_reflectionProbeRegenerateAll = Dvar_RegisterBool(
        "r_reflectionProbeRegenerateAll",
        false,
        DVAR_NOFLAG,
        "Regenerate cube maps for all reflection probes.");
    r_reflectionProbeGenerateExit = Dvar_RegisterBool(
        "r_reflectionProbeGenerateExit",
        false,
        DVAR_NOFLAG,
        "Exit when done generating reflection cubes.");
}

// ---------------------------------------------------------------------------
// Dedicated graphics configuration.
//
// Extracted from src/gfx_d3d/r_init.cpp:2811 and src/client_mp/cl_main_mp.cpp:
// 2317/2331. The values are what the renderer used to size its scene buffers for
// a dedicated server: one view, one entity slot per MAX_GENTITIES entry, the
// entity-number sentinels the shared code uses, and the shared thread/critical
// section counts. The fast-file names are the zones the server loads.
// ---------------------------------------------------------------------------

void __cdecl SetGfxConfig(const GfxConfiguration *config)
{
    iassert(config);
    bcassert(config->maxClientViews, GFX_MAX_CLIENT_VIEWS);
    iassert(config->critSectCount == CRITSECT_COUNT);

    memcpy(&gfxCfg, config, sizeof(gfxCfg));
}

static void SetupGfxConfig(GfxConfiguration *config)
{
    iassert(config);

    config->maxClientViews = 1;
    config->entCount = MAX_GENTITIES;
    config->entnumNone = ENTITYNUM_NONE;
    config->entnumOrdinaryEnd = ENTITYNUM_WORLD;
    config->threadContextCount = THREAD_CONTEXT_COUNT;
    config->critSectCount = CRITSECT_COUNT;
}

// dedicatedServer selects the zone set: a dedicated server has no UI, so
// uiFastFileName stays null and no ui_mp zone is requested.
static void CL_SetFastFileNames(GfxConfiguration *config, bool dedicatedServer)
{
    iassert(config);

    config->codeFastFileName = "code_post_gfx_mp";
    config->uiFastFileName = !dedicatedServer ? "ui_mp" : 0;
    config->commonFastFileName = "common_mp";
    config->localizedCodeFastFileName = "localized_code_post_gfx_mp";
    config->localizedCommonFastFileName = "localized_common_mp";
    config->modFastFileName = DB_ModFileExists() != 0 ? "mod" : NULL;
}

// The manifest is built in the same order and with the same zone flags as
// src/gfx_d3d/r_init.cpp:3643, so the allocation classes the zone loader uses
// for code, localized code, ui, common, localized common and mod are unchanged.
// Optional entries (localized names and mod) are skipped when the name is null.
static void R_LoadGraphicsAssets()
{
    XZoneInfo zoneInfo[6]{ 0 };
    uint32_t zoneCount;

    zoneInfo[0].name = gfxCfg.codeFastFileName;
    zoneInfo[0].allocFlags = DB_ZONE_CODE;
    zoneInfo[0].freeFlags = 0;
    zoneCount = 1;

    if (gfxCfg.localizedCodeFastFileName)
    {
        zoneInfo[zoneCount].name = gfxCfg.localizedCodeFastFileName;
        zoneInfo[zoneCount].allocFlags = DB_ZONE_CODE_LOC;
        zoneInfo[zoneCount].freeFlags = 0;
        zoneCount++;
    }
    if (gfxCfg.uiFastFileName)
    {
        zoneInfo[zoneCount].name = gfxCfg.uiFastFileName;
        zoneInfo[zoneCount].allocFlags = DB_ZONE_GAME;
        zoneInfo[zoneCount].freeFlags = 0;
        zoneCount++;
    }

    zoneInfo[zoneCount].name = gfxCfg.commonFastFileName;
    zoneInfo[zoneCount].allocFlags = DB_ZONE_COMMON;
    zoneInfo[zoneCount].freeFlags = 0;
    zoneCount++;

    if (gfxCfg.localizedCommonFastFileName)
    {
        zoneInfo[zoneCount].name = gfxCfg.localizedCommonFastFileName;
        zoneInfo[zoneCount].allocFlags = DB_ZONE_COMMON_LOC;
        zoneInfo[zoneCount].freeFlags = 0;
        zoneCount++;
    }

    if (gfxCfg.modFastFileName)
    {
        zoneInfo[zoneCount].name = gfxCfg.modFastFileName;
        zoneInfo[zoneCount].allocFlags = DB_ZONE_MOD;
        zoneInfo[zoneCount].freeFlags = 0;
        zoneCount++;
    }

    DB_LoadXAssets(zoneInfo, zoneCount, 0);
}

// src/gfx_d3d/r_init.cpp:4466. The latch is the load-bearing part: setting the
// cvar and then making the latched value current is what fixes
// r_loadForRenderer->current.enabled to false before the first zone is loaded, so
// the database keeps vertex/index data in CPU memory instead of handing it to a
// device.
void __cdecl R_MakeDedicated(const GfxConfiguration *config)
{
    SetGfxConfig(config);
    if (!r_loadForRenderer)
        R_RegisterDvars();
    Dvar_SetBool(r_loadForRenderer, 0);
    Dvar_MakeLatchedValueCurrent((dvar_s *)r_loadForRenderer);
    R_LoadGraphicsAssets();
}

// src/gfx_d3d/r_init.cpp:3615 hides the client's splash window with
// ShowWindow(g_splashWnd, 0). Sys_CreateSplashWindow is empty on this platform, so
// g_splashWnd is always null and there is no window to hide. The Windows call has
// no equivalent here, so the null check is the whole function; a non-null handle
// would mean a window was created without a display server.
void __cdecl Sys_HideSplashWindow()
{
    if (g_splashWnd)
        Com_Error(ERR_FATAL, "Sys_HideSplashWindow: a splash window exists on a platform without windows\n");
}

// src/client_mp/cl_main_mp.cpp:4163. onlinegame is registered here rather than in
// CL_Init so the server has the same online-game dvar the original dedicated
// server had; it is registered, not set, matching the original default of true.
void __cdecl CL_InitDedicated()
{
    GfxConfiguration config;

    if (!onlinegame)
        onlinegame = Dvar_RegisterBool(
            "onlinegame",
            1,
            DVAR_CHEAT,
            "Current game is an online game with stats, custom classes, unlocks");

    SetupGfxConfig(&config);
    CL_SetFastFileNames(&config, 1);
    R_MakeDedicated(&config);
    Sys_HideSplashWindow();
    Sys_ShowConsole();
    Sys_NormalExit();
}

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
// Zone geometry buffers (src/database/db_memory.cpp, db_file_load.cpp).
//
// DB_AllocXZoneMemory asks the renderer for a device vertex buffer and, if it
// gets writable bytes back, treats those as the zone's vertex/index storage. With
// r_loadForRenderer false the renderer returns null before touching the out
// parameter, and the database then keeps the data in the zone's CPU blocks
// (blocks[7] / blocks[8]) with zoneMem->vertexBuffer and lockedVertexData left
// null. That null is the accepted value on this path -- the callers do not test
// it -- so returning it is the correct behavior, not a failure.
//
// Consequently the finish/unlock/free calls below can only ever see a null
// buffer. A non-null buffer would mean a device buffer was created, which cannot
// happen on this build, so those cases stop instead of pretending to release.
// ---------------------------------------------------------------------------

// The dvar is registered by R_RegisterDvars through R_MakeDedicated; anything
// that asks for storage before that has no renderer either, so the disabled
// answer is the right one in both cases.
static bool DedicatedRendererAllocates()
{
    return r_loadForRenderer && r_loadForRenderer->current.enabled;
}

void *__cdecl R_AllocStaticVertexBuffer(IDirect3DVertexBuffer9 **vb, int sizeInBytes)
{
    iassert(vb);
    iassert(sizeInBytes > 0);

    if (!DedicatedRendererAllocates())
        return 0;

    Com_Error(ERR_FATAL, "R_AllocStaticVertexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");
    return 0;
}

void *__cdecl R_AllocStaticIndexBuffer(IDirect3DIndexBuffer9 **ib, int sizeInBytes)
{
    iassert(ib);
    iassert(sizeInBytes > 0);

    if (!DedicatedRendererAllocates())
        return 0;

    Com_Error(ERR_FATAL, "R_AllocStaticIndexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");
    return 0;
}

void __cdecl R_FinishStaticVertexBuffer(IDirect3DVertexBuffer9 *vb)
{
    if (!vb)
        return;

    Com_Error(ERR_FATAL, "R_FinishStaticVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void __cdecl R_FinishStaticIndexBuffer(IDirect3DIndexBuffer9 *ib)
{
    if (!ib)
        return;

    Com_Error(ERR_FATAL, "R_FinishStaticIndexBuffer: a device index buffer was created in a dedicated build\n");
}

void __cdecl R_UnlockVertexBuffer(IDirect3DVertexBuffer9 *handle)
{
    if (!handle)
        return;

    Com_Error(ERR_FATAL, "R_UnlockVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void __cdecl R_UnlockIndexBuffer(IDirect3DIndexBuffer9 *handle)
{
    if (!handle)
        return;

    Com_Error(ERR_FATAL, "R_UnlockIndexBuffer: a device index buffer was created in a dedicated build\n");
}

void __cdecl R_FreeStaticVertexBuffer(IDirect3DVertexBuffer9 *vb)
{
    if (!vb)
        return;

    Com_Error(ERR_FATAL, "R_FreeStaticVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void __cdecl R_FreeStaticIndexBuffer(IDirect3DIndexBuffer9 *ib)
{
    if (!ib)
        return;

    Com_Error(ERR_FATAL, "R_FreeStaticIndexBuffer: a device index buffer was created in a dedicated build\n");
}

// ---------------------------------------------------------------------------
// Database asset-registry callbacks that reach into the renderer.
//
// Each of these is called from src/database while a zone is loaded, unloaded or
// freed. The XAsset records themselves -- including GfxImage, Material and
// MaterialTechniqueSet -- stay in the database and keep their zone memory; only
// the device objects and the render-side bookkeeping are absent.
// ---------------------------------------------------------------------------

// src/gfx_d3d/r_image.cpp:87. On a client this moves delayed pixels from the zone
// into a texture. There is no texture here, so the pixels stay where the zone put
// them and delayLoadPixels stays set; clearing the flag would falsely claim the
// image had been uploaded.
void __cdecl R_DelayLoadImage(XAssetHeader header)
{
    if (!DedicatedRendererAllocates())
        return;

    Com_Error(ERR_FATAL, "R_DelayLoadImage: image '%s' wants a texture but this build has no renderer\n",
        header.image->name);
}

// src/gfx_d3d/r_image_load_obj.cpp:272 answers whether an image is one of the
// renderer-created procedural images in g_imageProgs. Nothing creates those on
// this build, so no image can be one.
bool __cdecl Image_IsProg(GfxImage *image)
{
    (void)image;
    return false;
}

// src/gfx_d3d/r_material.cpp:477 originally set rgp.needSortMaterials, which is
// the flag the renderer's material sort polls. The flag is real state, so it is
// still set: the sort simply never runs.
void __cdecl Material_DirtySort()
{
    rgp.needSortMaterials = 1;
}

// src/gfx_d3d/r_material_override.cpp:304. On a client this either keeps the
// technique set as-is (when r_rendererInUse is not the shader-model-3 path) or
// remaps it to an "sm2/" variant. With no shader model there is no remapping, so
// the set maps to itself -- the same result the original produced for a
// non-SM3 renderer -- and remappedTechniqueSet never points at a foreign set.
void __cdecl Material_OriginalRemapTechniqueSet(MaterialTechniqueSet *techSet)
{
    iassert(techSet);

    techSet->remappedTechniqueSet = techSet;
}

// src/gfx_d3d/rb_uploadshaders.cpp:14 queues a technique set for the render
// thread to upload on its next frame. The queue is drained only by
// RB_UploadShaderStep, which runs on a render thread this build never starts, so
// enqueueing would fill the 1024-entry ring and then trip its own overflow check.
// Nothing is uploaded, so nothing is queued.
void __cdecl Material_UploadShaders(MaterialTechniqueSet *techSet)
{
    iassert(Sys_IsDatabaseThread());
    (void)techSet;
}

// src/gfx_d3d/r_material_override.cpp:328 resets the upload queue described
// above. The queue is permanently empty here, so there is nothing to reset; the
// call is kept because DB_LoadXAssets makes it before every zone load.
void __cdecl Material_ClearShaderUploadList()
{
}

// src/gfx_d3d/r_material_override.cpp:323 marks the technique-set override table
// dirty. That table is rebuilt by Material_OverrideTechniqueSets for the render
// thread, which does not run here, so the flag has no reader.
void __cdecl Material_DirtyTechniqueSetOverrides()
{
}

// src/gfx_d3d/r_material_override.cpp:283 picks technique-set variants from the
// renderer's shader-model feature bits and then re-syncs the render thread. With
// no feature bits to read and no render thread to sync, the sets loaded from the
// zone stay as they are. This is called from DB_PostLoadXZone.
void __cdecl Material_OverrideTechniqueSets()
{
    iassert(Sys_IsMainThread());
}

// src/gfx_d3d/r_init.cpp:2934 guards against unloading the BSP the renderer is
// still drawing. rgp.world is never set on this build, so the guard is extracted
// verbatim and simply never fires.
void __cdecl R_UnloadWorld()
{
    iassert(IsFastFileLoad());

    if (rgp.world)
        Sys_Error("Cannot unload bsp while it is in use");
}

// ---------------------------------------------------------------------------
// Render-thread handoff.
//
// The database brackets zone loads with the remote-screen-update calls so the
// render thread is not reading a zone while it is replaced, and frees cached
// device state afterwards. All of that machinery is keyed on
// r_glob.startedRenderThread, which is set only by R_InitRenderThread; a
// dedicated server never starts one, so the original code took the "no render
// thread" branch of every function below. These counterparts are that branch made
// explicit, which is also why the nesting depth stays zero.
//
// r_glob (GfxGlobals) itself lives in the renderer, so the counterparts keep no
// mirror of it: a nesting counter that no one increments is zero by construction.
// ---------------------------------------------------------------------------

void __cdecl R_BeginRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1642 -- increments the nesting depth only when a render
    // thread is running, which never happens here.
    iassert(IsFastFileLoad());
}

void __cdecl R_EndRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1664 -- the matching decrement. Nothing was incremented,
    // so there is nothing to wait on or release.
    iassert(IsFastFileLoad());
}

void __cdecl R_PushRemoteScreenUpdate(int remoteScreenUpdateNesting)
{
    // r_rendercmds.cpp:1699 -- nests Begin calls. With no nesting possible the
    // only valid input is zero.
    iassert(remoteScreenUpdateNesting >= 0);
    iassert(remoteScreenUpdateNesting == 0);
    (void)remoteScreenUpdateNesting;
}

int __cdecl R_PopRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1718 -- returns the depth it unwound, which is always zero
    // here. Callers store this and pass it back to R_PushRemoteScreenUpdate.
    return 0;
}

bool __cdecl R_IsInRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1740 -- r_glob.isRenderingRemoteUpdate is set by the render
    // thread's own update path, which does not exist here.
    return false;
}

void __cdecl R_ReleaseThreadOwnership()
{
    // r_rendercmds.cpp:264 -- hands the front end to the render thread when one is
    // running. DB_FindXAssetHeader calls it before waiting on the database thread.
    iassert(Sys_IsMainThread());
}

void __cdecl R_SyncRenderThread()
{
    // r_rendercmds.cpp:231 -- once the renderer has registered, the main thread
    // sleeps until the render thread takes the front end. rg.registered is
    // defined in src/database/db_registry.cpp precisely so the dedicated build
    // keeps this check; it can only be non-zero if renderer registration ran.
    if (!Sys_IsRenderThread())
    {
        iassert(Sys_IsMainThread());
        if (rg.registered)
            Com_Error(ERR_FATAL, "R_SyncRenderThread: the renderer registered in a dedicated build\n");
    }
}

// src/gfx_d3d/r_image.cpp:1290 clears the samplers bound on a device.
void __cdecl RB_UnbindAllImages()
{
    // Guarded by dx.device in the original; no device is ever created here, so
    // there is no bound sampler and no device to clear it on.
}

// src/gfx_d3d/r_init.cpp:2828 clears the stream sources bound to a device.
void __cdecl R_ShutdownStreams()
{
    // Guarded by dx.device in the original.
}

// src/gfx_d3d/rb_shade.cpp:53/96/138 clear the shader and vertex declaration
// cached in gfxCmdBufState. Nothing ever binds them without a device, and the
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
// model draw instances. No world is registered on this build (rgp.world stays
// null), which is the single condition the original body tests.
void __cdecl R_ClearAllStaticModelCacheRefs()
{
    if (rgp.world)
        Com_Error(ERR_FATAL, "R_ClearAllStaticModelCacheRefs: a GfxWorld was registered in a dedicated build\n");
}

// ---------------------------------------------------------------------------
// Generated zone loaders (src/database/db_load.cpp).
//
// The asset loaders are generated and shared, so they call the renderer's record
// loaders unconditionally; each one decides what to do with the record. Every
// function below reproduces the branch the original takes when
// r_loadForRenderer->current.enabled is false, which is the branch the original
// dedicated server ran: device handles are written as null, the record's CPU data
// is left exactly as the zone provided it.
// ---------------------------------------------------------------------------

// src/gfx_d3d/r_buffers.cpp:122.
void __cdecl Load_VertexBuffer(IDirect3DVertexBuffer9 **vb, uint8_t *bufferData, int sizeInBytes)
{
    // Without a device there is no buffer to copy into, so the record's vertex
    // data stays in the zone and the handle is null. The size is the caller's to
    // interpret; nothing here reads it.
    if (DedicatedRendererAllocates())
        Com_Error(ERR_FATAL, "Load_VertexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");

    *vb = 0;
}

// src/gfx_d3d/r_image.cpp:270. The disabled branch is the assignment above the
// device work: the texture handle is null and the pixels stay in the zone, with
// delayLoadPixels left as the record had it.
void __cdecl Load_Texture(GfxTexture *remoteLoadDef, GfxImage *image)
{
    GfxImageLoadDef *loadDef = remoteLoadDef->loadDef;

    iassert(loadDef == image->texture.loadDef);

    if (DedicatedRendererAllocates())
        Com_Error(ERR_FATAL, "Load_Texture: image '%s' wants a texture but this build has no renderer\n", image->name);

    image->texture.basemap = 0;
}

// src/gfx_d3d/r_material.cpp:289 and 304. Both write the null program handle and
// leave the byte-code load definition in the record.
void __cdecl Load_CreateMaterialPixelShader(GfxPixelShaderLoadDef *loadDef, MaterialPixelShader *mtlShader)
{
    iassert(loadDef == &mtlShader->prog.loadDef);

    mtlShader->prog.ps = 0;
}

void __cdecl Load_CreateMaterialVertexShader(GfxVertexShaderLoadDef *loadDef, MaterialVertexShader *mtlShader)
{
    iassert(loadDef == &mtlShader->prog.loadDef);

    mtlShader->prog.vs = 0;
}

// src/gfx_d3d/r_material.cpp:353. Every declaration handle is null, and isLoaded is
// still set so the pass is not rebuilt by Material_ReloadPassResources -- the same
// state a client reaches when it loads a pass with the renderer switched off.
void __cdecl Load_BuildVertexDecl(MaterialVertexDeclaration **mtlVertDecl)
{
    int vertDeclType;

    for (vertDeclType = 0; vertDeclType < 16; ++vertDeclType)
        (*mtlVertDecl)->routing.decl[vertDeclType] = 0;

    (*mtlVertDecl)->isLoaded = 1;
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
// Asset release paths (src/database/db_registry.cpp DB_RemoveXAssetHandler,
// Material_FreeAllTechniqueSets, DB_UnloadXZone).
// ---------------------------------------------------------------------------

// src/gfx_d3d/r_image.cpp:794. Same predicate as the renderer's, needed because
// Image_Free below uses it and r_image.cpp is not part of this build.
bool __cdecl Image_IsCodeImage(int track)
{
    return track >= IMAGE_TRACK_MISC && (track <= IMAGE_TRACK_DEBUG || track == IMAGE_TRACK_LIGHTMAP);
}

// src/gfx_d3d/r_image.cpp:636 calls Image_Release (r_image.cpp:175). With no
// device the image never had card memory taken from the tracking total and never
// had a basemap, so the accounting subtraction is the original's and the release
// itself has nothing to do. A non-null basemap would mean a texture was created.
void __cdecl Image_Free(GfxImage *image)
{
    int platform;

    iassert(image);

    if (!Image_IsCodeImage(image->track))
    {
        for (platform = 0; platform < 2; ++platform)
            imageGlobals.totalMemory.platform[platform] -= image->cardMemory.platform[platform];
    }

    if (image->texture.basemap)
        Com_Error(ERR_FATAL, "Image_Free: image '%s' owns a texture in a dedicated build\n", image->name);
}

// src/gfx_d3d/r_material.cpp:753 (Material_ReleasePassResources), reduced to the
// device objects this build can hold: the shader handles are always null, so the
// only state to unwind is the isLoaded flag Load_BuildVertexDecl set -- which is
// what makes a later Material_ReloadPassResources rebuild the declaration.
static void DedicatedReleasePassResources(MaterialPass *pass)
{
    iassert(pass->pixelShader);
    iassert(pass->vertexShader);
    iassert(pass->vertexDecl);

    if (pass->pixelShader->prog.ps || pass->vertexShader->prog.vs)
        Com_Error(ERR_FATAL, "Material_ReleasePassResources: a material pass owns shaders in a dedicated build\n");

    if (pass->vertexDecl->isLoaded)
    {
        pass->vertexDecl->isLoaded = 0;
        for (int declIndex = 0; declIndex < 16; ++declIndex)
            pass->vertexDecl->routing.decl[declIndex] = 0;
    }
}

// src/gfx_d3d/r_material.cpp:852 -> Material_ReleaseTechniqueSetResources (:797).
// Registered in DB_RemoveXAssetHandler, so it runs for every technique set when a
// zone is unloaded.
void __cdecl Material_ReleaseTechniqueSet(XAssetHeader header, void *crap)
{
    MaterialTechnique *technique;
    int techType;
    int passIndex;

    (void)crap;

    for (techType = TECHNIQUE_DEPTH_PREPASS; techType < TECHNIQUE_COUNT; ++techType)
    {
        technique = header.techniqueSet->techniques[techType];
        if (technique)
        {
            for (passIndex = 0; passIndex < technique->passCount; ++passIndex)
                DedicatedReleasePassResources(&technique->passArray[passIndex]);
        }
    }
}

// ---------------------------------------------------------------------------
// Model bounds.
//
// src/gfx_d3d/r_bsp_load_obj.cpp:2498 and the two surface accessors it reads
// (src/gfx_d3d/r_xsurface.cpp:8/38). All three are vertex maths over the XModel
// data the database already holds -- no device, no shader. The shared XModel
// source loader calls R_GetXModelBounds, so the behaviour has to be the real one
// rather than a stub that leaves an entity's bounds unset.
// ---------------------------------------------------------------------------

void __cdecl XSurfaceGetVerts(const XSurface *surf, float *pVert, float *pTexCoord, float *pNormal)
{
    int render_count;
    GfxPackedVertex *verts1;
    GfxPackedVertex *verts0;

    verts0 = surf->verts0;
    iassert(verts0);
    verts1 = verts0;
    for (render_count = surf->vertCount; render_count; --render_count)
    {
        if (pNormal)
        {
            Vec3UnpackUnitVec(verts1->normal, pNormal);
            pNormal += 3;
        }
        if (pTexCoord)
        {
            Vec2UnpackTexCoords(verts1->texCoord, pTexCoord);
            pTexCoord += 2;
        }
        *pVert = verts0->xyz[0];
        pVert[1] = verts0->xyz[1];
        pVert[2] = verts0->xyz[2];
        ++verts0;
        ++verts1;
        pVert += 3;
    }
}

int __cdecl XSurfaceGetNumVerts(const XSurface *surface)
{
    iassert(surface);
    return surface->vertCount;
}

void __cdecl R_GetXModelBounds(XModel *model, const float (*axes)[3], float *mins, float *maxs)
{
    float coord;
    int surfaceCount;
    int axisIndex;
    XSurface *surfaces;
    int index;
    XSurface *xsurf;
    int vertCount;
    int vertIndex;
    float (*vert)[3];

    *mins = FLT_MAX;
    mins[1] = FLT_MAX;
    mins[2] = FLT_MAX;
    *maxs = -FLT_MAX;
    maxs[1] = -FLT_MAX;
    maxs[2] = -FLT_MAX;
    surfaceCount = XModelGetSurfaces(model, &surfaces, 0);
    iassert(surfaces);
    vert = (float(*)[3])Hunk_AllocateTempMemory(393216, "R_GetXModelBounds");
    iassert(surfaceCount > 0);
    for (index = 0; index < surfaceCount; ++index)
    {
        xsurf = &surfaces[index];
        vertCount = XSurfaceGetNumVerts(xsurf);
        XSurfaceGetVerts(xsurf, (float *)vert, 0, 0);
        iassert(vertCount > 0);
        for (vertIndex = 0; vertIndex < vertCount; ++vertIndex)
        {
            for (axisIndex = 0; axisIndex < 3; ++axisIndex)
            {
                coord = vert[vertIndex][0] * (*axes)[axisIndex]
                    + vert[vertIndex][1] * (*axes)[axisIndex + 3]
                        + vert[vertIndex][2] * (*axes)[axisIndex + 6];
                if (coord < mins[axisIndex])
                    mins[axisIndex] = coord;
                if (coord > maxs[axisIndex])
                    maxs[axisIndex] = coord;
            }
        }
    }
    Hunk_FreeTempMemory((char *)vert);
}

// ---------------------------------------------------------------------------
// Name-keyed asset registration.
//
// Both are only reached with fast files in use on this build, which is how a
// shipped server runs; the source (.efx / .mtl) loaders they dispatch to live in
// the excluded renderer and FX sources. The fast-file side is a real database
// lookup, so a missing asset is reported as a missing asset rather than papered
// over with a null success.
// ---------------------------------------------------------------------------

// src/gfx_d3d/r_material.cpp:585. The "always use the default material"
// substitution and the "$default" fallback both need rgp.defaultMaterial, which
// only the renderer creates, so those cases cannot be served here.
Material *__cdecl Material_RegisterHandle(const char *name, int imageTrack)
{
    PROF_SCOPED("Material_RegisterHandle");

    iassert(name);

    if (!IsFastFileLoad())
        Com_Error(ERR_FATAL, "Material_RegisterHandle('%s'): loading materials from source is not supported in the dedicated build\n", name);

    if (!*name)
        Com_Error(ERR_FATAL, "Material_RegisterHandle: the empty name resolves to the renderer's default material\n");

    // src/gfx_d3d/r_material.cpp:493: the fast-file registration is a database
    // lookup. A zone that does not carry the material yields null, exactly as it
    // does for a client.
    return DB_FindXAssetHeader(ASSET_TYPE_MATERIAL, name).material;
}

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
