#pragma once

// The Modern path's hooks into the renderer (r_visuals modern, on the Vulkan
// back end only; see docs/vulkan-modern.md).
//
// The Vulkan back end's host (src/linux/client/vulkan/vulkan_backend.cpp,
// src/win32/vulkan/win_vulkan_backend.cpp) installs them when it makes the
// renderer's IDirect3D9 with Modern visuals, and takes them away whenever an
// IDirect3D9 is made without. Without them the renderer draws exactly as it
// always has. The hooks live with R_HW_Direct3DCreate9
// (r_rhi_d3d9_calls.cpp), which every program that holds the host also holds;
// the renderer calls them from r_scene.cpp and rb_draw3d.cpp.

#include <cstdint>

struct IDirect3DDevice9;

struct GfxModernScene
{
    float view[4][4];
    float projection[4][4];  // with the frame's jitter in [2][0] and [2][1]
    int x, y, width, height; // the scene's viewport
    bool cut;                // the camera jumped: no history from before
};

struct GfxModernHooks
{
    void *user;
    // The sub-pixel jitter of scene n, in pixels, each within (-0.5, 0.5).
    void (*jitterPixels)(void *user, uint32_t frame, float *x, float *y);
    // A view's 3D scene is drawn (render target 0 and the depth surface hold
    // it), before post effects and 2D: the back end runs its passes over it.
    void (*sceneDone)(void *user, IDirect3DDevice9 *device, const GfxModernScene *scene);
    // A view's 3D scene is about to be drawn (before its shadow maps and its
    // first draw into the back buffer). With linear light the back end gives
    // it an image of its own until sceneDone. May be null.
    void (*sceneBegin)(void *user, IDirect3DDevice9 *device);
    // The last DrawIndexedPrimitive drew a surface that moves on its own
    // (an entity's model, the view model) into the scene: the back end draws
    // it again for its velocity before sceneDone. clip and prevClip take its
    // vertices to this frame's and last frame's clip space (row vectors,
    // p_clip = p * M), each with its frame's jitter (r_modern_motion.cpp).
    // May be null.
    void (*surfaceMoved)(void *user, IDirect3DDevice9 *device, const float clip[4][4], const float prevClip[4][4]);
};

// Null removes them. Called while no frame is being built (as the renderer
// makes its IDirect3D9).
void R_SetModernHooks(const GfxModernHooks *hooks);
// The hooks installed, or null.
const GfxModernHooks *R_ModernHooks();
