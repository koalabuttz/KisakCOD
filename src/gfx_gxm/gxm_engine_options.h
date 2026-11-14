// Choices the GXM back end makes for the engine's frame.
//
// g_depthPrepass: draw the scene's depth before its lit pass, whatever
// r_depthPrepass says. The Vita's GPU then skips hidden pixels of the lit
// pass early: console run T (2026-11-11, mp_crash) measured the GPU's time a
// frame at 80 ms with it against 108 without at the busy view, and 27 against
// 50 facing the other way. An empty file `no-depth-prepass` in the folder of
// the compiled programs turns it off, for a run that compares.
//
// g_resolveWhenUsed: with r_distortion on, the engine copies the picture
// for the surfaces that bend it (heat haze, glass) before the emissive pass
// of every frame, whether or not such a surface is drawn. On the Vita the
// copy also ends the frame's scene there and starts another, each costing a
// store and a load of the whole targets. With this the copy is made only in
// frames whose emissive list holds a technique that reads it: console run
// AD (2026-11-14, mp_crash, busy view, 12 MB parameter buffer) measured the
// GPU at 79.4 ms a frame without the copy against 82.6 with it, and the
// frame had 5 scenes instead of 7. An empty file `no-resolve-skip` in the
// folder of the compiled programs turns it off.

#pragma once

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern bool g_resolveWhenUsed;
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_RESOLVE_WHEN_USED() (::gfx_gxm::g_resolveWhenUsed)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_RESOLVE_WHEN_USED() false
#endif
