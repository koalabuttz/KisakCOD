// server_renderer_counterparts.cpp -- dedicated counterparts for definitions
// that production renderer units own.
//
// Moved out of server_assets.cpp with no change in behavior. Every definition
// below has an owner in src/gfx_d3d that the dedicated server does not compile:
//
//   r_loadForRenderer, r_modelVertColor, r_picmip_water, the reflection-probe
//   dvars, R_RegisterDvars and R_ReflectionProbeRegisterDvars   r_dvars.cpp
//   the static vertex/index buffer calls and Load_VertexBuffer   r_buffers.cpp
//   the render-thread handoff (remote screen update,
//   R_ReleaseThreadOwnership, R_SyncRenderThread)                r_rendercmds.cpp
//   XSurfaceGetVerts and XSurfaceGetNumVerts                     r_xsurface.cpp
//   R_GetXModelBounds                                             r_bsp_load_obj.cpp
//
// The dedicated server and the offline asset export link this file through the
// src/linux/*.cpp glob. A target that links those owners instead -- the world
// front-end probe (scripts/platform/linux/world_frontend_probe.cmake) and the
// world device probe (world_device_probe.cmake) -- leaves this file out, so every
// name keeps exactly one definition in each link and the linker reports any
// overlap or gap. The dedicated renderer start and the counterparts for the
// owners that create device objects are in server_device_counterparts.cpp;
// everything else the database needs from the renderer is in server_assets.cpp,
// which every native target links.

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
// Renderer dvar storage (src/gfx_d3d/r_dvars.cpp).
//
// The declarations must stay exactly as the renderer's own definitions are,
// because the externs in r_dvars.h are what the shared sources compile against.
// ---------------------------------------------------------------------------

const dvar_t *r_loadForRenderer;

// Read by the shared asset loaders: r_modelVertColor by the XModel source loader
// (xanim/xmodel_load_obj.cpp:551), r_picmip_water by the water record loader
// (Load_PicmipWater in server_assets.cpp), and the reflection-probe trio by
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

void R_RegisterDvars()
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
void R_ReflectionProbeRegisterDvars()
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

void *R_AllocStaticVertexBuffer(IDirect3DVertexBuffer9 **vb, int sizeInBytes)
{
    iassert(vb);
    iassert(sizeInBytes > 0);

    if (!DedicatedRendererAllocates())
        return 0;

    Com_Error(ERR_FATAL, "R_AllocStaticVertexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");
    return 0;
}

void *R_AllocStaticIndexBuffer(IDirect3DIndexBuffer9 **ib, int sizeInBytes)
{
    iassert(ib);
    iassert(sizeInBytes > 0);

    if (!DedicatedRendererAllocates())
        return 0;

    Com_Error(ERR_FATAL, "R_AllocStaticIndexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");
    return 0;
}

void R_FinishStaticVertexBuffer(IDirect3DVertexBuffer9 *vb)
{
    if (!vb)
        return;

    Com_Error(ERR_FATAL, "R_FinishStaticVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void R_FinishStaticIndexBuffer(IDirect3DIndexBuffer9 *ib)
{
    if (!ib)
        return;

    Com_Error(ERR_FATAL, "R_FinishStaticIndexBuffer: a device index buffer was created in a dedicated build\n");
}

void R_UnlockVertexBuffer(IDirect3DVertexBuffer9 *handle)
{
    if (!handle)
        return;

    Com_Error(ERR_FATAL, "R_UnlockVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void R_UnlockIndexBuffer(IDirect3DIndexBuffer9 *handle)
{
    if (!handle)
        return;

    Com_Error(ERR_FATAL, "R_UnlockIndexBuffer: a device index buffer was created in a dedicated build\n");
}

void R_FreeStaticVertexBuffer(IDirect3DVertexBuffer9 *vb)
{
    if (!vb)
        return;

    Com_Error(ERR_FATAL, "R_FreeStaticVertexBuffer: a device vertex buffer was created in a dedicated build\n");
}

void R_FreeStaticIndexBuffer(IDirect3DIndexBuffer9 *ib)
{
    if (!ib)
        return;

    Com_Error(ERR_FATAL, "R_FreeStaticIndexBuffer: a device index buffer was created in a dedicated build\n");
}

// src/gfx_d3d/r_buffers.cpp:122, called by the generated zone loaders
// (src/database/db_load.cpp). It reproduces the branch the original takes when
// r_loadForRenderer->current.enabled is false.
void Load_VertexBuffer(IDirect3DVertexBuffer9 **vb, uint8_t *bufferData, int sizeInBytes)
{
    // Without a device there is no buffer to copy into, so the record's vertex
    // data stays in the zone and the handle is null. The size is the caller's to
    // interpret; nothing here reads it.
    if (DedicatedRendererAllocates())
        Com_Error(ERR_FATAL, "Load_VertexBuffer: r_loadForRenderer is enabled but this build has no renderer\n");

    *vb = 0;
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

void R_BeginRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1642 -- increments the nesting depth only when a render
    // thread is running, which never happens here.
    iassert(IsFastFileLoad());
}

void R_EndRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1664 -- the matching decrement. Nothing was incremented,
    // so there is nothing to wait on or release.
    iassert(IsFastFileLoad());
}

void R_PushRemoteScreenUpdate(int remoteScreenUpdateNesting)
{
    // r_rendercmds.cpp:1699 -- nests Begin calls. With no nesting possible the
    // only valid input is zero.
    iassert(remoteScreenUpdateNesting >= 0);
    iassert(remoteScreenUpdateNesting == 0);
    (void)remoteScreenUpdateNesting;
}

int R_PopRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1718 -- returns the depth it unwound, which is always zero
    // here. Callers store this and pass it back to R_PushRemoteScreenUpdate.
    return 0;
}

bool R_IsInRemoteScreenUpdate()
{
    // r_rendercmds.cpp:1740 -- r_glob.isRenderingRemoteUpdate is set by the render
    // thread's own update path, which does not exist here.
    return false;
}

void R_ReleaseThreadOwnership()
{
    // r_rendercmds.cpp:264 -- hands the front end to the render thread when one is
    // running. DB_FindXAssetHeader calls it before waiting on the database thread.
    iassert(Sys_IsMainThread());
}

void R_SyncRenderThread()
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

// ---------------------------------------------------------------------------
// XSurface vertex accessors (src/gfx_d3d/r_xsurface.cpp:8/38).
//
// R_GetXModelBounds below reads them. Both are vertex maths over
// the XModel data the database already holds -- no device, no shader -- so they
// are the real bodies, not stubs.
// ---------------------------------------------------------------------------

void XSurfaceGetVerts(const XSurface *surf, float *pVert, float *pTexCoord, float *pNormal)
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

int XSurfaceGetNumVerts(const XSurface *surface)
{
    iassert(surface);
    return surface->vertCount;
}

// ---------------------------------------------------------------------------
// Model bounds (src/gfx_d3d/r_bsp_load_obj.cpp:2498).
//
// It reads the two surface accessors above. All three are vertex maths over the
// XModel data the database already holds -- no device, no shader. The shared
// XModel source loader calls R_GetXModelBounds, so the behaviour has to be the
// real one rather than a stub that leaves an entity's bounds unset.
// ---------------------------------------------------------------------------

void R_GetXModelBounds(XModel *model, const float (*axes)[3], float *mins, float *maxs)
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
