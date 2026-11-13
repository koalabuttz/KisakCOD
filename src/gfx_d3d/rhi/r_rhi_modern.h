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
    // The view's glow (vision set bloom), when the renderer would draw it.
    bool glow;
    float glowRadius;       // Gaussian sigma, in pixels of the scene
    float glowIntensity;
    float glowCutoff;       // display value, 0 to 1
    float glowDesaturation; // 0 to 1
    // Set by sceneDone when the back end drew bloom in place of the glow:
    // the renderer then leaves its own glow out of this view.
    bool glowDrawn;
};

struct GfxModernHooks
{
    void *user;
    // The sub-pixel jitter of scene n, in pixels, each within (-0.5, 0.5).
    void (*jitterPixels)(void *user, uint32_t frame, float *x, float *y);
    // A view's 3D scene is drawn (render target 0 and the depth surface hold
    // it), before post effects and 2D: the back end runs its passes over it.
    void (*sceneDone)(void *user, IDirect3DDevice9 *device, GfxModernScene *scene);
    // A view's 3D scene is about to be drawn (before its shadow maps and its
    // first draw into the back buffer). With linear light the back end gives
    // it an image of its own until sceneDone. May be null.
    void (*sceneBegin)(void *user, IDirect3DDevice9 *device);
    // The view's lit scene is drawn and its emissive surfaces (effects,
    // blended surfaces) come next; sceneDone follows them with the same
    // scene. A back end that draws effects on the display image finishes the
    // scene here (and then does nothing at sceneDone). May be null.
    void (*sceneLit)(void *user, IDirect3DDevice9 *device, GfxModernScene *scene);
};

// Null removes them. Called while no frame is being built (as the renderer
// makes its IDirect3D9).
void R_SetModernHooks(const GfxModernHooks *hooks);
// The hooks installed, or null.
const GfxModernHooks *R_ModernHooks();
