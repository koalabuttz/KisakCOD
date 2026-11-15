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
// g_sceneScaleLeast, g_sceneScaleMost, g_sceneScaleGoalMs, g_sceneScaleNow:
// with vita_sceneScaleMin under vita_sceneScale the back end picks the size
// between the two, frame by frame, from the time each frame waited for the
// GPU (gxm_scale_governor.h): the picture shrinks only as far as the GPU
// needs to keep the frame's goal, and is whole again where the view allows.
// The engine hands over the two limits and the goal (vita_sceneScaleGoal, in
// ms; 0 lets the governor choose 16.7 or 33.3 ms) when a full-screen 3D view
// starts and reads the size for that view. The back end's thread writes the
// size at each Present. Console run BV (2026-11-15, mp_crash, busy view):
// 35.5 ms a frame at the full size, held by the GPU, and 29.6 ms at 0.7,
// held by the CPU.
//
// Keeping the smaller picture sharp:
// - g_mipBias: a picture drawn smaller makes the GPU pick smaller mip
//   levels of every texture, so the stretched picture would show blurrier
//   textures than the full-size one. While the 3D picture is scaled, the
//   back end biases each mipped texture's level of detail by log2 of the
//   scale (in sceGxm's steps of 1/8 of a level), which gives the texels the
//   full-size picture would have used. An empty file `no-mip-bias`, or
//   "vita_gxm no-mip-bias 1", turns it off.
// - g_sceneWidthOnly (vita_sceneScaleWidthOnly): the picture keeps its
//   height and gives up the same share of its pixels in width alone (the
//   width's factor is the scale squared, no less than a half), so every
//   line of the screen is still drawn.
// - g_sceneSharpen (vita_sceneSharpen): the stretch is drawn by a program
//   of the back end's that reads the picture five times and raises each
//   pixel's contrast against its four neighbours, held inside the range of
//   the five so that no halo appears. It costs the two scenes of the copy
//   and the stretch, which g_stretchInFilter otherwise saves.
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
#include <atomic>
#include <cmath>

namespace gfx_gxm
{
extern bool g_depthPrepass;
extern bool g_prepassWorldOnly;
extern bool g_prepassSkipModels;
extern bool g_resolveWhenUsed;
extern float g_sceneScale;
extern std::atomic<float> g_sceneScaleLeast;
extern std::atomic<float> g_sceneScaleMost;
extern std::atomic<float> g_sceneScaleGoalMs;
extern std::atomic<float> g_sceneScaleNow;
extern bool g_mipBias;
extern bool g_sceneWidthOnly;
extern bool g_sceneSharpen;
// True while the engine asks for the scaled picture's stretch (StretchRect).
extern bool g_sharpenThisStretch;
extern bool g_stretchInFilter;
extern float g_filterReadS;
extern float g_filterReadT;

// A size of the screen as the scaled picture has it.
inline int SceneScaled(int size, float scale)
{
    return (std::max)(1, int(float(size) * scale));
}

// The factors of the picture's width and height for a scale.
inline float SceneScaleX(float scale)
{
    return g_sceneWidthOnly ? (std::max)(scale * scale, 0.5f) : scale;
}
inline float SceneScaleY(float scale)
{
    return g_sceneWidthOnly ? 1.0f : scale;
}

// Steps of 1/8 of a mip level (sceGxm's unit) that a picture scaled by
// `smallest` on its smaller side moves every texture's level of detail by:
// 8 * log2(smallest), rounded; 0 or less.
inline int SceneMipBiasSteps(float smallest)
{
    if (smallest >= 1.0f)
        return 0;
    return int(std::lround(8.0f * std::log2((std::max)(smallest, 0.25f))));
}

// The size of this frame's 3D picture: the engine's own (`most`), or the
// back end's choice between `least` and it. Hands the back end the limits
// for the frames to come.
inline float SceneScaleFor(float least, float most, float goalMs)
{
    g_sceneScaleLeast.store(least, std::memory_order_relaxed);
    g_sceneScaleMost.store(most, std::memory_order_relaxed);
    g_sceneScaleGoalMs.store(goalMs, std::memory_order_relaxed);
    if (least >= most)
        return most;
    return std::clamp(g_sceneScaleNow.load(std::memory_order_relaxed), least, most);
}
} // namespace gfx_gxm

#ifdef KISAK_GFX_GXM
#define KISAK_GXM_DEPTH_PREPASS() (::gfx_gxm::g_depthPrepass)
#define KISAK_GXM_PREPASS_SKIP_MODELS() (::gfx_gxm::g_prepassSkipModels)
#define KISAK_GXM_SET_PREPASS_SKIP_MODELS(on) (::gfx_gxm::g_prepassSkipModels = (on) && ::gfx_gxm::g_prepassWorldOnly)
#define KISAK_GXM_RESOLVE_WHEN_USED() (::gfx_gxm::g_resolveWhenUsed)
#define KISAK_GXM_SCENE_SCALE() (::gfx_gxm::g_sceneScale)
#define KISAK_GXM_STRETCH_IN_FILTER() (::gfx_gxm::g_stretchInFilter)
#else
#define KISAK_GXM_DEPTH_PREPASS() false
#define KISAK_GXM_PREPASS_SKIP_MODELS() false
#define KISAK_GXM_SET_PREPASS_SKIP_MODELS(on) ((void)0)
#define KISAK_GXM_RESOLVE_WHEN_USED() false
#define KISAK_GXM_SCENE_SCALE() 1.0f
#define KISAK_GXM_STRETCH_IN_FILTER() false
#endif
