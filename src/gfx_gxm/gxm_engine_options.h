// Choices the GXM back end makes for the engine's frame.
//
// g_depthPrepass: draw the scene's depth before its lit pass, whatever
// r_depthPrepass says. The Vita's GPU then skips hidden pixels of the lit
// pass early: console run T (2026-11-11, mp_crash) measured the GPU's time a
// frame at 80 ms with it against 108 without at the busy view, and 27 against
// 50 facing the other way. An empty file `no-depth-prepass` in the folder of
// the compiled programs turns it off, for a run that compares.

#pragma once

namespace gfx_gxm
{
extern bool g_depthPrepass;
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#endif
