// Choices the GXM back end makes for the engine's frame.
//
// g_depthPrepass: draw the scene's depth before its lit pass, whatever
// r_depthPrepass says. The Vita's GPU then skips hidden pixels of the lit
// pass early: console run T (2026-11-11, mp_crash) measured the GPU's time a
// frame at 80 ms with it against 108 without at the busy view, and 27 against
// 50 facing the other way. An empty file `no-depth-prepass` in the folder of
// the compiled programs turns it off, for a run that compares.
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

#pragma once

#include <algorithm>

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern float g_sceneScale;

// A size of the screen as the scaled picture has it.
inline int SceneScaled(int size, float scale)
{
    return std::max(1, int(float(size) * scale));
}
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_SCENE_SCALE() (::gfx_gxm::g_sceneScale)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_SCENE_SCALE() 1.0f
#endif
