#pragma once

// Struct sizes the world front-end probe checks before it reads a world.
//
// The probe executable compiles the database loader and the dedicated server
// with the dedicated definitions, and the renderer front end with the client
// definitions (scripts/platform/linux/world_frontend_probe.cmake). Both sides
// share the structs below: the loader fills s_world, comWorld, rgp and the
// material assets, the dedicated Com_GetPrimaryLight hands the front end's light
// culling its ComPrimaryLight records, and the front end reads all of them.
// KISAK_WORLD_FRONTEND_LAYOUT() expands in the including translation unit, so
// each side reports the sizes it was compiled with, and the probe refuses to run
// when any differs.
//
// The world device probe (world_device_probe.h) holds its own side to the same
// list: it is compiled with the D3D9 types from DXVK's headers, the loader with
// the type-only stand-in, and both hand each other images, shaders, vertex
// declarations, imageGlobals and the zone table as well.
//
// Include the engine headers that define these types first (r_bsp.h, r_init.h,
// r_image.h, r_material.h, xanim.h, q_shared.h).

#include <cstddef>
#include <cstdint>

#define KISAK_WORLD_FRONTEND_LAYOUT_TYPES(X) \
    X(GfxWorld)                              \
    X(GfxSurface)                            \
    X(GfxCell)                               \
    X(GfxPortal)                             \
    X(GfxAabbTree)                           \
    X(GfxCullGroup)                          \
    X(GfxWorldDpvsPlanes)                    \
    X(GfxWorldDpvsStatic)                    \
    X(GfxWorldDpvsDynamic)                   \
    X(GfxStaticModelDrawInst)                \
    X(Material)                              \
    X(MaterialTechniqueSet)                  \
    X(MaterialTechnique)                     \
    X(MaterialPass)                          \
    X(MapEnts)                               \
    X(ComWorld)                              \
    X(ComPrimaryLight)                       \
    X(GfxShadowGeometry)                     \
    X(GfxLightRegion)                        \
    X(GfxConfiguration)                      \
    X(r_global_permanent_t)                  \
    X(r_globals_t)                           \
    X(dvar_s)                                \
    X(GfxImage)                              \
    X(GfxImageLoadDef)                       \
    X(ImgGlobals)                            \
    X(MaterialVertexDeclaration)             \
    X(MaterialVertexShader)                  \
    X(MaterialPixelShader)                   \
    X(MaterialTextureDef)                    \
    X(water_t)                               \
    X(XZone)

namespace kisak_world_frontend
{
#define KISAK_WORLD_FRONTEND_LAYOUT_COUNT_ONE(type) +1
constexpr size_t kLayoutTypeCount = 0 KISAK_WORLD_FRONTEND_LAYOUT_TYPES(KISAK_WORLD_FRONTEND_LAYOUT_COUNT_ONE);
#undef KISAK_WORLD_FRONTEND_LAYOUT_COUNT_ONE

struct Layout
{
    uint32_t size[kLayoutTypeCount];
};

#define KISAK_WORLD_FRONTEND_LAYOUT_NAME_ONE(type) #type,
inline constexpr const char *kLayoutTypeNames[kLayoutTypeCount] = {
    KISAK_WORLD_FRONTEND_LAYOUT_TYPES(KISAK_WORLD_FRONTEND_LAYOUT_NAME_ONE)};
#undef KISAK_WORLD_FRONTEND_LAYOUT_NAME_ONE

// The index of the first type whose sizes differ, or kLayoutTypeCount.
inline size_t FirstLayoutDifference(const Layout &a, const Layout &b)
{
    for (size_t i = 0; i < kLayoutTypeCount; ++i)
        if (a.size[i] != b.size[i])
            return i;
    return kLayoutTypeCount;
}
} // namespace kisak_world_frontend

// A macro, not a function: an inline function would be one definition shared by
// both sides, and would report one view twice.
#define KISAK_WORLD_FRONTEND_LAYOUT_SIZE_ONE(type) uint32_t(sizeof(type)),
#define KISAK_WORLD_FRONTEND_LAYOUT() \
    (::kisak_world_frontend::Layout{{KISAK_WORLD_FRONTEND_LAYOUT_TYPES(KISAK_WORLD_FRONTEND_LAYOUT_SIZE_ONE)}})
