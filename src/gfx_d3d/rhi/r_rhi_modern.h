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
struct IDirect3DCubeTexture9;

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

// The colours a lit surface's pixel shader reads (world surfaces lit by
// lightmaps, models lit by the lighting volume, dynamic lights). With linear
// light the back end takes them from display values to linear light, so the
// shader adds its light terms and blends its fog in linear light.
struct GfxModernLitInputs
{
    uint64_t constants; // float constants c0-c63, bit per register
    uint16_t samplers;  // pixel samplers s0-s15 whose results are colours
    uint16_t doubled;   // of those, the ones the shader doubles (2x overbright)
    bool colorInput;    // the vertex colour (colour 0 input)
    // Instead of the above (all zero then): no colour read is decoded, and the
    // stored colour is decoded without its clamp to 1, so a lit surface
    // computed over white stays over white for the tonemap and bloom.
    bool overWhite;
    // With overWhite: a Modern lit shader of the back end's own that this
    // pass's pixel shader can be drawn as instead (it reads the same samplers,
    // registers and vertex outputs, which the renderer has checked), or
    // GFX_MODERN_LIT_SHADER_NONE.
    uint16_t shader;
};

// The Modern lit shader's code: the family, plus flags for its maps and its
// mode (the same values as the Vulkan back end's ModernLitShader).
enum GfxModernLitShader : uint16_t
{
    GFX_MODERN_LIT_SHADER_NONE = 0,
    // lm_[hsm_]sun_<mode>c0[d0][n0][s0]: world surfaces lit by lightmaps and
    // the sun, with a colour map.
    GFX_MODERN_LIT_SHADER_LM_SUN = 0x40,
    // lp_[hsm_][i_]sun_<mode>c0[d0][n0][s0]: models lit by the lighting volume
    // and the sun, with a colour map.
    GFX_MODERN_LIT_SHADER_LP_SUN = 0x80,
    // lm_<mode>c0[d0][n0][s0] and lp_[i_]<mode>c0[d0][n0][s0]: surfaces
    // without a primary light (interiors), with a colour map; models set
    // GFX_MODERN_LIT_SHADER_NO_SUN_MODEL (where the shadow bit is, which this
    // family never has).
    GFX_MODERN_LIT_SHADER_NO_SUN = 0xc0,
    GFX_MODERN_LIT_SHADER_FAMILY = 0xc0,
    GFX_MODERN_LIT_SHADER_NO_SUN_MODEL = 0x01,
    GFX_MODERN_LIT_SHADER_SHADOW = 0x01,   // hsm: the sun's shadow map
    GFX_MODERN_LIT_SHADER_NORMAL = 0x02,   // n0: a normal map
    GFX_MODERN_LIT_SHADER_SPECULAR = 0x04, // s0: a specular map and the reflection probe
    GFX_MODERN_LIT_SHADER_DETAIL = 0x08,   // d0: a detail map
    GFX_MODERN_LIT_SHADER_BLENDED = 0x10,  // b0 (else r0, opaque)
    GFX_MODERN_LIT_SHADER_ALPHA_TESTED = 0x20, // t0
    // The primary light, in bits 8-9, with the LM_SUN and LP_SUN families (0
    // is the sun): a spot or omni light in its place (lit shader facts 6),
    // or, for models only, lp_[i_]amb[_<light>]_t0c0: the volume's light plus
    // its visibility times the sun's colour, with no normal.
    GFX_MODERN_LIT_SHADER_SPOT = 0x100,
    GFX_MODERN_LIT_SHADER_OMNI = 0x200,
    GFX_MODERN_LIT_SHADER_AMBIENT = 0x300,
    GFX_MODERN_LIT_SHADER_LIGHT = 0x300,
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
    // The last DrawIndexedPrimitive drew a surface that moves on its own
    // (an entity's model, the view model) into the scene: the back end draws
    // it again for its velocity when the scene is finished (at sceneLit or
    // sceneDone). clip and prevClip take its vertices to this frame's and
    // last frame's clip space (row vectors, p_clip = p * M), each with its
    // frame's jitter (r_modern_motion.cpp). May be null.
    void (*surfaceMoved)(void *user, IDirect3DDevice9 *device, const float clip[4][4], const float prevClip[4][4]);
    // The same for a skinned surface, whose vertices were written this frame
    // (an entity's animated model, the view model's hands), reported in every
    // scene it is drawn in, the first time with a null prevClip. The back end
    // keeps the vertices the draw read, under `key` (the same surface's key in
    // the next scene), and draws the surface's velocity from last scene's
    // vertices and this scene's. May be null.
    void (*skinnedSurfaceMoved)(void *user, IDirect3DDevice9 *device, uint64_t key, const float clip[4][4],
                                const float prevClip[4][4]);
    // From sceneBegin to sceneLit, when a pass is set up for draws: the
    // colours its pixel shader reads when the pass is lit, else null (the
    // shader's colour is then taken as a display value, as before). Called
    // only when that changes, and with null at sceneLit. May be null.
    void (*litInputs)(void *user, IDirect3DDevice9 *device, const GfxModernLitInputs *inputs);
    // Whether the back end wants the view's opaque depth before its lit
    // surfaces (sceneDepth): the renderer then draws its depth prepass
    // (r_depthPrepass) for the view, whatever the dvar says. Asked once per
    // view, after sceneBegin. May be null (no).
    bool (*wantsSceneDepth)(void *user, IDirect3DDevice9 *device);
    // A view's depth prepass has drawn its opaque surfaces into the depth
    // surface, and its lit surfaces come next (the scene is the same one
    // sceneLit and sceneDone get after). May be null.
    void (*sceneDepth)(void *user, IDirect3DDevice9 *device, GfxModernScene *scene);
    // One of the world's reflection probes, as the world is set up (each
    // time it is): the back end may make the cube's levels below the first
    // again from level 0 (the same levels each time). May be null.
    void (*reflectionProbe)(void *user, IDirect3DDevice9 *device, IDirect3DCubeTexture9 *probe);
};

// Null removes them. Called while no frame is being built (as the renderer
// makes its IDirect3D9).
void R_SetModernHooks(const GfxModernHooks *hooks);
// The hooks installed, or null.
const GfxModernHooks *R_ModernHooks();
