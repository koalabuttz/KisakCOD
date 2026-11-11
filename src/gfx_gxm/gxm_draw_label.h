// What the engine is drawing, for the GXM back end's GPU draw timer (run
// marker gpu-draw-times, docs/vita.md): the pass of the frame (lit, decal,
// emissive, post effects, 2D, ...) and the kind of surface (world, static
// model, model, effect, ...). The render thread sets them before it draws and
// the back end reads them at each draw; they are string literals, so a draw
// only copies two pointers.

#pragma once

namespace gfx_gxm
{
extern const char *g_drawPass;
extern const char *g_drawSurface;
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DRAW_PASS(name) (::gfx_gxm::g_drawPass = (name), ::gfx_gxm::g_drawSurface = nullptr)
#define KISAK_GXM_DRAW_SURFACE(name) (::gfx_gxm::g_drawSurface = (name))
#else
#define KISAK_GXM_DRAW_PASS(name) ((void)0)
#define KISAK_GXM_DRAW_SURFACE(name) ((void)0)
#endif
