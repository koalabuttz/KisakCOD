// Choices the GXM back end makes for the engine's frame.
//
// g_depthPrepass: draw the scene's depth before its lit pass, whatever
// r_depthPrepass says. The Vita's GPU then skips hidden pixels of the lit
// pass early: console run T (2026-11-11, mp_crash) measured the GPU's time a
// frame at 80 ms with it against 108 without at the busy view, and 27 against
// 50 facing the other way. An empty file `no-depth-prepass` in the folder of
// the compiled programs turns it off, for a run that compares.

//
// The Vita's GPU works a scene at a time: every change of render target ends
// one, stores its whole colour and depth targets, and loads them again when
// the frame comes back to them. Console run V (2026-11-12, mp_crash) counted
// seven scenes a frame at the busy view, two of them copies of the picture
// with no draws at 2.25 ms each. The two choices below take scenes out of
// the frame without changing what is drawn.
//
// g_depthInScene: the forced depth pass draws into the scene's own targets.
// The engine sends it to the dynamic shadows target (which shares the
// scene's depth), so the frame was: the scene's clear, the depth pass in a
// scene of its own with a colour target nothing reads, and back to the
// scene. With it the clear, the depth pass and the lit pass are one scene.
// Not when shadow cookies are on: they draw into that target after the
// depth pass and the lit pass reads it. `no-depth-in-scene` turns it off.
//
// g_resolveWhenUsed: with r_distortion on, the engine copies the picture
// for the surfaces that bend it (heat haze, glass) before the emissive pass
// of every frame, whether or not such a surface is drawn. With it the copy
// is made only in frames whose emissive list holds a technique that reads
// it. `no-resolve-skip` turns it off.

#pragma once

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern bool g_depthInScene;
extern bool g_resolveWhenUsed;
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_DEPTH_IN_SCENE() (::gfx_gxm::g_depthInScene)
#define KISAK_GXM_RESOLVE_WHEN_USED() (::gfx_gxm::g_resolveWhenUsed)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_DEPTH_IN_SCENE() false
#define KISAK_GXM_RESOLVE_WHEN_USED() false
#endif
