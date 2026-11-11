# Vulkan Modern path

The Modern path is the second way the Vulkan back end can draw the game. The
Legacy path draws the game as it was made, with its own shaders translated
([adapter](vulkan-legacy-adapter.md)). The Modern path records passes of its own on the
shared core ([core](vulkan-core.md)), into the frame the Legacy path draws and
on the same images. It is the renderer plan's "Modern visuals" (the plan is in
the project files, `renderer/modern-renderer-plan.md`, sections 3, 6 and 7).

**State: experimental.** So far the path has the first piece of the plan's
early experiment (E1): temporal anti-aliasing (TAA) over the scene the Legacy
path drew. Velocity comes from the camera alone. There is no linear lighting,
HDR or material of its own yet.

## Turning it on

```
+set r_backend vulkan +set r_visuals modern
```

`r_visuals` is `legacy` (default) or `modern`. It is archived and latched, so
it applies at the next `vid_restart`, like `r_backend`. The Vulkan back end's
host registers it (`src/linux/client/vulkan/vulkan_backend.cpp`,
`src/win32/vulkan/win_vulkan_backend.cpp`), so it exists only in programs built
with the back end and does nothing with `r_backend d3d9`. The console line
`Graphics back end: Vulkan (...)` names the visuals in use.

It needs a one-sample back buffer (`r_aaSamples 1`). With more, the first scene
is refused, the reason is logged once (`vulkan d3d9: modern: the scene is not
anti-aliased: the scene is multisampled`), and the jitter stops, so the game
looks as it does with `legacy`.

## How a frame goes

1. **Jitter (front end).** `R_RenderScene` sets up the scene's view
   (`R_SetViewParmsForScene`). Then `R_ModernJitterScene` (`src/gfx_d3d/r_scene.cpp`)
   moves the projection by a sub-pixel offset: it adds `2 x / width` and `-2 y /
   height` to `projectionMatrix[2][0]` and `[2][1]`, and remakes the
   view-projection matrices. The offsets are the Halton (2, 3) sequence over 8
   frames (`gfx_vk::modern::JitterPixels`). Everything the Legacy path draws in
   the 3D scene is moved the same way, the view model included. 2D and the HUD
   have their own projection and are not moved.
2. **The scene is drawn** by the Legacy path as always: depth prepass if on,
   lit, decals, sun, point lights, emissive (effects).
3. **Hand-over (render thread).** `RB_EndSceneRendering` (`src/gfx_d3d/rb_draw3d.cpp`)
   calls `R_ModernSceneDone` first, before post effects and 2D. It gives the
   hook the view and projection matrices and the scene viewport. It also sets a
   cut when the camera moved more than 256 units since the last scene (a spawn,
   a teleport, a killcam).
4. **The front** (`gfx_vk::d3d9::ModernSceneDone`, `src/gfx_vk/d3d9`) takes
   render target 0 and the depth surface as the engine left them set. Its
   Vulkan back end (`BackendModernScene`, `vk_d3d9_backend_vulkan.cpp`) asks the
   Legacy adapter to end its pass and forget what it has bound
   (`lg::HandOverRecording`), takes the two surfaces' core images
   (`lg::SurfaceImage`) and calls the Modern path.
5. **TAA** (`gfx_vk::modern::Resolve`, `src/gfx_vk/modern/vk_modern_taa.cpp`,
   shaders in `src/gfx_vk/modern/shaders`) records one full-screen pass. The
   pass writes the result into one of two `R16G16B16A16_SFLOAT` history
   images, then blits that history image over the scene's colour image within
   the viewport. Post effects, 2D and the HUD then draw on top, unchanged.

The hooks (`src/gfx_d3d/rhi/r_rhi_modern.h`) sit next to `R_HW_Direct3DCreate9`.
The host installs them when it makes the renderer's IDirect3D9 with
`r_visuals modern`. Any IDirect3D9 made without it removes them, including a
switch to `r_backend d3d9`. Without the hooks the renderer runs exactly as
before: no jitter and no extra pass.

## The anti-aliasing

For each pixel, `taa.frag`:

* reads this frame's colour and builds the box of colours in its 3x3
  neighbourhood in YCoCg: the minimum and maximum, narrowed to the mean ±1.25
  standard deviations;
* takes the nearest depth in the same 3x3 (so edges take the motion of the
  surface in front), clamped to the depth of a point at infinity
  (`projection[2][2]` for the engine's infinite projection), so the sky
  reprojects by the camera's rotation alone;
* finds where that surface was last frame with one matrix. The matrix is this
  frame's clip space without the jitter, mapped to last frame's
  (`gfx_vk::modern::Reprojection`: `inverse(VP_now) * VP_before`, worked in
  double on the CPU because world positions reach tens of thousands of
  units). The pixel moves by the camera's motion at that surface. A camera that
  stands still reads the history at the same pixel whatever the jitter;
* reads the history there (bilinear), clips it towards the box's centre until
  it is inside the box, and blends it with this frame's colour. The history's
  weight is 0.9, and both weights are divided by 1 + brightness (Karis 2014),
  so one bright sample does not flicker through.

There is no history on the first scene, after a cut, or when the scene's size
changes. Those scenes are drawn as they came. Pixel positions follow D3D9's,
as the Legacy path draws: pixel (i, j) holds the scene at NDC
`x = 2 i / width - 1`, `y = 1 - 2 j / height` of the viewport.

## What it does not do yet

* **No motion of its own for what moves:** players, vehicles, effects and the
  view model are reprojected as if they stood still in the world. Clipping the
  history to this frame's colours keeps them from leaving trails, at the cost
  of some softness on them while they move. The view model moves with the
  camera, so while the camera turns it gets the world's motion and loses most
  of its history. The plan's next steps give it velocity of its own (rigid and
  skinned models, the view model's own projection) and give effects a reactive
  mask.
* **No sharpening.** TAA softens the picture somewhat. A sharpening pass would
  come with the plan's temporal upscaler (MD3).
* **One history for all views.** Splitscreen would share it; the PC
  multiplayer game has one view.
* **Gamma space.** The scene is still the Legacy path's gamma-space colour.
  Linear light and HDR come with the plan's MD1.
* **Legacy shaders that read the scene** (the distortion resolve after the
  sun, `R_RENDERTARGET_RESOLVED_POST_SUN`) read it before the pass, jittered.

## Measuring

`r_vkStats` (and the statistics a device logs when it goes) prints
`vulkan d3d9: modern: N scenes anti-aliased (M without history), X ms GPU
each`. The last number is the pass's own GPU time from the core's pass
timestamps (`KISAK_VK_PASS_TIMESTAMPS`). It leaves out the blit back, which
the frame's GPU time includes.

## Checks

The D3D9 check's `modern_taa` scene (`src/gfx_vk/tools/vk_d3d9_check.cpp`,
[front](vulkan-d3d9-front.md)) covers the following on lavapipe, with the
validation layer where it is installed:

* the camera reprojection, in double and as the float matrix the shader gets;
* the jitter sequence;
* a still scene, which comes back exactly as drawn;
* a jittered edge, which ends between its two colours;
* a scene without a depth surface, which is refused.

`scripts/tests/test_vulkan_d3d9_front.py` checks the scene in the full run and
the `modern:` statistics line. `scripts/tools/vulkan/gen_spirv_header.py
--check` (CI) checks that `vk_modern_shaders.h` matches the GLSL sources and
passes `spirv-val`. Regenerate the header with
`gen_spirv_header.py` (it needs `glslangValidator`, Debian's `glslang-tools`)
after changing a shader.

## Next

From the plan's E1, in order:

* velocity for rigid models (a previous placement per entity), skinned models
  (the previous frame's skinned vertices, which the renderer already keeps
  per frame) and the view model (its own current and previous projection);
* a reactive mask for effects;
* then linear light: the world-lit family drawn by the Modern path's own
  shaders into an HDR target, with a tonemap that matches Legacy at neutral
  settings.

The first laptop run of this piece should compare `r_visuals legacy` and
`modern` in the standing mp_crash scene: frame rate, the pass's GPU time,
and numbers for how much the edges change.
