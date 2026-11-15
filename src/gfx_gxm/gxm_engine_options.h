// Choices the GXM back end makes for the engine's frame.
//
// g_depthPrepass: draw the scene's depth before its lit pass, whatever
// r_depthPrepass says. The Vita's GPU then skips hidden pixels of the lit
// pass early: console run T (2026-11-11, mp_crash) measured the GPU's time a
// frame at 80 ms with it against 108 without at the busy view, and 27 against
// 50 facing the other way. With the GPU at 222 MHz the pass costs the CPU
// more than it saves the GPU in some views (console run AM), so the back end
// times both ways while the game runs and sets this to the faster
// (gxm_prepass_governor.h). An empty file `no-depth-prepass` in the folder of
// the compiled programs turns it off, for a run that compares, and
// `no-prepass-governor` keeps it on in every frame.
//
// g_prepassWorldOnly: the forced depth pass draws the map's own surfaces
// and leaves the models out. The pass draws every opaque surface a second
// time, and console run AR (2026-11-14, mp_crash) found static models the
// largest part of the render thread's draws (240 runs and 12 ms a frame over
// both passes) though walls and ground hide most of what a view hides. The
// lit pass tests and writes depth itself, so what the pass leaves out is
// drawn as it would be with no pass at all and the picture is the same.
// The engine sets g_prepassSkipModels while it draws that pass
// (rb_draw3d.cpp), and the surface loop (rb_backend.cpp) passes the models
// by. Only the pass this back end forces: a pass r_depthPrepass asks for,
// and the one that fills the depth copy for soft particles and depth of
// field, draw everything. An empty file `no-world-only-prepass` in the
// folder of the compiled programs, or "vita_gxm no-world-only-prepass 1",
// turns it off.
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
//
// g_sceneScale: the 3D picture of this frame is drawn at this fraction of
// the screen's width and height, in the screen's top left corner, and
// stretched to the whole screen before the HUD, which keeps the screen's
// own resolution (rb_draw3d.cpp, RB_StretchScaledScene). The Vita's GPU time
// grows with the pixels it shades: console run V (2026-11-12, mp_crash,
// busy view) took 95 ms a frame at full size, 77.7 at 0.7 and 58.8 at 0.5
// (r_scaleViewport, which shrinks the picture instead of stretching it).
// The engine sets it from vita_sceneScale when a full-screen 3D view starts
// and puts it back to 1 once the picture is stretched, so nothing drawn
// after (the sun's flare, the HUD) is scaled.
//
// g_stretchInFilter: the copy and the stretch of the scaled picture are two
// more scenes a frame (console run AH, 2026-11-14: 5 became 7). When the
// only post effect of the view is the map's film colour filter, which reads
// a copy of the picture anyway and draws it over the whole screen, the
// filter reads just the drawn corner of that copy instead and so does the
// stretch itself: no scene is added. RB_StretchScaledScene then sets
// g_filterReadS and g_filterReadT, the part of the copy the filter reads,
// and the filter puts them back to 1. An empty file `no-filter-stretch` in
// the folder of the compiled programs, or "vita_gxm no-filter-stretch 1",
// turns it off.
//
// Fewer scenes a frame. Every scene makes the GPU load the whole colour and
// depth targets at its start and store them at its end, whatever is drawn
// (console run V: 2.25 ms for a scene with no draw), and console run BX
// (2026-11-15) found the frame's five scenes at 16.9 ms of fragment work in
// a view of 65 draws. Three choices take two of the five out and a store
// off a third, with the same picture:
//
// - g_prepassClears: the frame's first scene held only the clear of the
//   back buffer, since the depth pass that follows has a colour target of
//   its own. With this the depth pass's scene clears the depth and stencil
//   itself and the lit pass's scene clears the colour (when r_clear asks),
//   as the engine already does for the depth copy of soft particles.
//   `no-prepass-clear` turns it off.
// - g_prepassNoColour: the depth pass writes no colour and nothing reads
//   its colour target, so its scene has none: the engine sets
//   g_colourUnread while it binds that target and the back end opens the
//   scene with sceGxm's disabled colour surface. `no-depth-only-scene`
//   turns it off.
// - g_sceneInCopy: when the view's only post effect is the film colour
//   filter, the 3D passes draw into the image the filter reads
//   (R_RENDERTARGET_RESOLVED_SCENE) and not into the back buffer, so the
//   copy between the two, a scene of its own, is not made. g_sceneIsCopy is
//   true from the start of such a view until the filter (or the stretch of
//   a scaled picture) has taken the picture from the image.
//   `no-scene-in-copy` turns it off.
//
// The three markers are empty files in the folder of the compiled programs,
// or "vita_gxm <name> 1" while the game runs.

#pragma once

#include <algorithm>

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern bool g_prepassWorldOnly;
extern bool g_prepassSkipModels;
extern bool g_resolveWhenUsed;
extern float g_sceneScale;
extern bool g_stretchInFilter;
extern float g_filterReadS;
extern float g_filterReadT;
extern bool g_prepassClears;
extern bool g_prepassNoColour;
extern bool g_colourUnread;
extern bool g_sceneInCopy;
extern bool g_sceneIsCopy;

// A size of the screen as the scaled picture has it.
inline int SceneScaled(int size, float scale)
{
    return (std::max)(1, int(float(size) * scale));
}
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_PREPASS_SKIP_MODELS() (::gfx_gxm::g_prepassSkipModels)
#define KISAK_GXM_SET_PREPASS_SKIP_MODELS(on) (::gfx_gxm::g_prepassSkipModels = (on) && ::gfx_gxm::g_prepassWorldOnly)
#define KISAK_GXM_RESOLVE_WHEN_USED() (::gfx_gxm::g_resolveWhenUsed)
#define KISAK_GXM_SCENE_SCALE() (::gfx_gxm::g_sceneScale)
#define KISAK_GXM_STRETCH_IN_FILTER() (::gfx_gxm::g_stretchInFilter)
#define KISAK_GXM_PREPASS_CLEARS() (::gfx_gxm::g_prepassClears)
#define KISAK_GXM_SET_COLOUR_UNREAD(on) (::gfx_gxm::g_colourUnread = (on) && ::gfx_gxm::g_prepassNoColour)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_PREPASS_SKIP_MODELS() false
#define KISAK_GXM_SET_PREPASS_SKIP_MODELS(on) ((void)0)
#define KISAK_GXM_RESOLVE_WHEN_USED() false
#define KISAK_GXM_SCENE_SCALE() 1.0f
#define KISAK_GXM_STRETCH_IN_FILTER() false
#define KISAK_GXM_PREPASS_CLEARS() false
#define KISAK_GXM_SET_COLOUR_UNREAD(on) ((void)0)
#endif
