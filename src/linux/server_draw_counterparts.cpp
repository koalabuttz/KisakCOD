// server_draw_counterparts.cpp -- definitions the shared database sources call
// whose owners are renderer units a target links only when it draws.
//
// Moved out of server_assets.cpp with no change in behavior. Loading a zone calls
// R_RegisterModel (src/gfx_d3d/r_model.cpp), the shader-cache clears
// (src/gfx_d3d/rb_shade.cpp), the static model cache check
// (src/gfx_d3d/r_staticmodelcache.cpp) and Load_PicmipWater
// (src/gfx_d3d/r_water.cpp). The dedicated server, the world front-end probe and
// the world device probe link this file. The world draw probe
// (scripts/platform/linux/world_draw_probe.cmake) links those four owners
// instead, because it binds shaders, registers its world before it draws and
// needs the real cache walk, and leaves this file out.

#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <universal/com_memory.h>

#include <qcommon/qcommon.h>

#include <database/database.h>

#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_model.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_water.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/rb_shade.h>
#include <gfx_d3d/r_staticmodelcache.h>

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
XModel *R_RegisterModel(const char *name)
{
    return XModelPrecache(
        (char *)name,
        (void *(*)(int))Hunk_AllocXModelPrecache,
        (void *(*)(int))Hunk_AllocXModelPrecacheColl);
}

// ---------------------------------------------------------------------------
// Renderer calls the database makes that need no device.
// ---------------------------------------------------------------------------

// src/gfx_d3d/rb_shade.cpp:53/96/138 clear the shader and vertex declaration
// cached in gfxCmdBufState. Only drawing binds them, and nothing here draws: the
// dedicated build has no device and the world device probe issues no draw. The
// original's own assertion (prim.vertexDecl == NULL) holds trivially.
void RB_ClearPixelShader()
{
}

void RB_ClearVertexShader()
{
}

void RB_ClearVertexDecl()
{
}

// src/gfx_d3d/r_staticmodelcache.cpp:605 walks the registered world's static
// model draw instances. The dedicated build registers no world (rgp.world stays
// null), which is the single condition the original body tests. The world device
// probe gets here from its synchronous map load (DB_LoadXAssets archives assets
// first) before it registers the map's world, and it unloads no zone afterwards.
void R_ClearAllStaticModelCacheRefs()
{
    if (rgp.world)
        Com_Error(ERR_FATAL, "R_ClearAllStaticModelCacheRefs: a GfxWorld was registered in a dedicated build\n");
}

// src/gfx_d3d/r_water.cpp:292, extracted verbatim. This is CPU work on the water
// simulation grid stored in the zone, not device work, and the shared material
// loader calls it for every water texture, so the grid is reduced to the same
// dimensions the client would produce.
void Load_PicmipWater(water_t **waterRef)
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
