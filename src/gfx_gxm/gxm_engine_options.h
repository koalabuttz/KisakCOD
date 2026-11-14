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

#pragma once

#include <algorithm>

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern bool g_resolveWhenUsed;
extern float g_sceneScale;
extern bool g_stretchInFilter;
extern float g_filterReadS;
extern float g_filterReadT;

// A size of the screen as the scaled picture has it.
inline int SceneScaled(int size, float scale)
{
    return (std::max)(1, int(float(size) * scale));
}
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_RESOLVE_WHEN_USED() (::gfx_gxm::g_resolveWhenUsed)
#define KISAK_GXM_SCENE_SCALE() (::gfx_gxm::g_sceneScale)
#define KISAK_GXM_STRETCH_IN_FILTER() (::gfx_gxm::g_stretchInFilter)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_RESOLVE_WHEN_USED() false
#define KISAK_GXM_SCENE_SCALE() 1.0f
#define KISAK_GXM_STRETCH_IN_FILTER() false
#endif
