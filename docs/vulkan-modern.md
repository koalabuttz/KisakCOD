# Vulkan Modern path

The Modern path is the second way the Vulkan back end can draw the game. The
Legacy path draws the game as it was made, with its own shaders translated
([adapter](vulkan-legacy-adapter.md)). The Modern path records passes of its own on the
shared core ([core](vulkan-core.md)), into the frame the Legacy path draws and
on the same images. It is the renderer plan's "Modern visuals" (the plan is in
the project files, `renderer/modern-renderer-plan.md`, sections 3, 6 and 7).

**State: experimental.** So far the path has the first piece of the plan's
early experiment (E1), temporal anti-aliasing (TAA) over the scene the Legacy
path drew, and the first piece of linear light (the plan's MD1): the scene is
drawn into a linear-light HDR image and tonemapped back
([Linear light](#linear-light)), with bloom from that image in place of the
vision set's glow ([Bloom](#bloom)). Velocity comes from the camera, and for
entities' models (rigid and skinned surfaces, the view model's included) from
a velocity pass of their own ([Velocity](#velocity-of-what-moves-on-its-own));
effects do not have their own yet. No material is drawn by shaders of the
Modern path's own yet.

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
   lit, decals, sun, point lights, emissive (effects). Each rigid surface of
   an entity's model it draws into the scene is reported with its matrix
   ([Velocity](#velocity-of-what-moves-on-its-own)).
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
5. **Velocity** (`gfx_vk::modern::DrawVelocity`,
   `src/gfx_vk/modern/vk_modern_velocity.cpp`) draws the reported surfaces
   again into a motion image, when there are any.
6. **TAA** (`gfx_vk::modern::Resolve`, `src/gfx_vk/modern/vk_modern_taa.cpp`,
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

* compresses every colour it reads with `c / (1 + max(c))` and expands the
  result again at the end (Karis 2014), so the box, the clip and the blend
  below work on values in [0, 1): one bright sample does not outweigh its dark
  neighbours, and on linear light, light above 1 does not flicker through the
  average;
* reads this frame's colour and builds the box of colours in its 3x3
  neighbourhood in YCoCg: the minimum and maximum, narrowed to the mean ±1.25
  standard deviations;
* takes the nearest depth in the same 3x3 (so edges take the motion of the
  surface in front), clamped to the depth of a point at infinity
  (`projection[2][2]` for the engine's infinite projection), so the sky
  reprojects by the camera's rotation alone;
* where the velocity image holds a motion at that nearest pixel (a surface
  that moves on its own), takes that motion;
* otherwise finds where that surface was last frame with one matrix. The matrix is this
  frame's clip space without the jitter, mapped to last frame's
  (`gfx_vk::modern::Reprojection`: `inverse(VP_now) * VP_before`, worked in
  double on the CPU because world positions reach tens of thousands of
  units). The pixel moves by the camera's motion at that surface. A camera that
  stands still reads the history at the same pixel whatever the jitter;
* reads the history there (bilinear), clips it towards the box's centre until
  it is inside the box, and blends it with this frame's colour. The history's
  weight is 0.9.

## Velocity of what moves on its own

Camera reprojection is exact for the world and static models, which do not
move. Entities' models do, and the view model moves with the camera, so the
Modern path draws their surfaces a second time with their own motion.

* **The renderer** (`src/gfx_d3d/r_modern_motion.cpp`). When it skins a scene
  entity, each surface records its entity (`GfxModelSurfaceInfo::modernOwner`,
  the entity number plus one, in what was retail's padding). When a rigid
  surface is drawn into the scene's render target (`r_draw_xmodel.cpp`), the
  matrix that took it to clip space (`CONST_SRC_CODE_WORLD_VIEW_PROJECTION_MATRIX`,
  with the jitter, and for the view model its own depth-hack projection) is
  kept by entity, model and surface. From the second scene it is seen in, it
  is reported through the `surfaceMoved` hook with last scene's matrix, once
  per scene however many passes draw it.
* **The front** (`gfx_vk::d3d9::ModernSurfaceMoved`) takes the Legacy
  adapter's last indexed draw as it is still bound (`lg::LastIndexedDraw`:
  stream 0, the index buffer and range, the viewport) and keeps it with both
  matrices until the scene is finished: at `ModernSceneLit` when effects
  blend on the display image (the default), else at `ModernSceneDone`. The
  renderer closes its scene's motion at either, so surfaces drawn after
  `R_ModernSceneLit` (effects) report none.
* **The velocity pass** (`vk_modern_velocity.cpp`, `velocity.vert` and
  `velocity.frag`) draws those surfaces into an `R16G16_SFLOAT` image cleared
  to "no motion of its own" (65504). Each fragment whose depth matches the
  scene depth (so the surface is what the scene shows there) writes where the
  surface was last frame relative to where it is now, in NDC without either
  frame's jitter. Hidden fragments write nothing, so what hides a moving
  surface keeps the camera's motion. The pipeline reads 32-byte vertices with
  the position first, as the engine's models are.
* **Skinned surfaces** (characters' bodies, the view model's arms) have
  vertices the renderer writes again each frame, on the CPU, into a buffer it
  locks with DISCARD (its skinned cache, or the dynamic vertex buffer when
  the cache is off, as in singleplayer); last frame's are gone by the next
  scene. `rb_tess.cpp` reports them through the `skinnedSurfaceMoved` hook in
  every scene, with the surface's key and a null last matrix the first time.
  The front keeps the vertices each draw read: when the scene is finished,
  `modern::KeepVertices` copies them on the GPU into a buffer of the pass's
  own (two halves of 8 MB, one written each scene while the other holds the
  last scene's). The next scene draws the surface's velocity with this
  scene's vertices as one vertex stream and the kept copy as a second, so its
  motion is the skinning's as well as the entity's. A camera cut, or a scene
  that keeps nothing, forgets what was kept.

What it does not cover yet: models added without an entity (scene models),
brush models and effects (whether drawn on the display image or, with
`KISAK_VK_MODERN_EFFECTS=linear`, in the scene). `KISAK_VK_MODERN_VELOCITY=0`
turns the pass off, for comparisons.

There is no history on the first scene, after a cut, or when the scene's size
changes. Those scenes are drawn as they came. Pixel positions follow D3D9's,
as the Legacy path draws: pixel (i, j) holds the scene at NDC
`x = 2 i / width - 1`, `y = 1 - 2 j / height` of the viewport.

## What it does not do yet

* **Effects have no motion of their own:** they are reprojected as if they
  stood still in the world. Clipping the history to this frame's colours keeps
  them from leaving trails, at the cost of some softness on them while they
  move. Entities' models and the view model have their own
  ([Velocity](#velocity-of-what-moves-on-its-own)); effects get a reactive
  mask next.
* **No sharpening.** TAA softens the picture somewhat. A sharpening pass would
  come with the plan's temporal upscaler (MD3).
* **One history for all views.** Splitscreen would share it; the PC
  multiplayer game has one view.
* **Light from Legacy shaders only.** In linear light the scene is the
  Legacy path's colour decoded as it is drawn (below), so nothing in it is
  brighter than white yet; the HDR range comes with the world drawn by the
  Modern path's own shaders.
* **Legacy shaders that read the scene** (the distortion resolve after the
  sun, `R_RENDERTARGET_RESOLVED_POST_SUN`) read it before the pass, jittered.

## Measuring

`r_vkStats` (and the statistics a device logs when it goes) prints
`vulkan d3d9: modern: N scenes anti-aliased (M without history), X ms GPU
each`. The last number is the pass's own GPU time from the core's pass
timestamps (`KISAK_VK_PASS_TIMESTAMPS`). It leaves out the blit back, which
the frame's GPU time includes. When surfaces that move on their own were
drawn, it also prints `vulkan d3d9: modern: N scenes with surfaces that move
on their own (M surfaces), X ms GPU each for their velocity`, and
`vulkan d3d9: modern: N scenes kept skinned vertices (M surfaces, K KB each
scene), S skinned surfaces drawn from last scene's`: the copies at the end of
each scene, and how many skinned surfaces had last scene's vertices to be
drawn from.

## Checks

The D3D9 check's `modern_taa` scene (`src/gfx_vk/tools/vk_d3d9_check.cpp`,
[front](vulkan-d3d9-front.md)) covers the following on lavapipe, with the
validation layer where it is installed:

* the camera reprojection, in double and as the float matrix the shader gets;
* the jitter sequence;
* a still scene, which comes back exactly as drawn;
* a jittered edge, which ends between its two colours;
* a scene without a depth surface, which is refused.

Its `modern_velocity` scene moves a band of 2-pixel white and black stripes 2
pixels a frame, drawn indexed from 32-byte vertices as the engine's models
are, behind a static striped quad. Reported to the velocity pass
(`front::ModernSurfaceMoved`), the band's stripes stay exactly as drawn; not
reported, they go grey (the scene checks that too, so it would notice a pass
that does nothing); and the static quad in front stays as drawn, so the band's
motion is not written where it is hidden.

`scripts/tests/test_vulkan_d3d9_front.py` checks the scenes in the full run and
the `modern:` statistics lines. `scripts/tools/vulkan/gen_spirv_header.py
--check` (CI) checks that `vk_modern_shaders.h` matches the GLSL sources and
passes `spirv-val`. Regenerate the header with
`gen_spirv_header.py` (it needs `glslangValidator`, Debian's `glslang-tools`)
after changing a shader.

## Linear light

The renderer plan's MD1 puts the Modern path in linear light: the scene is
kept as light, in an HDR image, and turned into display values once, by a
tonemap. This first piece gives the scene that image and the tonemap, with
everything still drawn by the Legacy path. It is on with `r_visuals modern`;
`KISAK_VK_MODERN_LINEAR=0` turns it off (the scene is then drawn into the back
buffer as before, with the anti-aliasing over it).

1. **Begin (render thread).** `RB_Draw3DInternal` (`src/gfx_d3d/rb_draw3d.cpp`)
   calls `R_ModernSceneBegin` before a view's 3D scene, shadow maps included.
   The hook (`GfxModernHooks::sceneBegin`) reaches the front's
   `ModernSceneBegin`, whose back end hands it an `R16G16B16A16_SFLOAT` image
   of the back buffer's size (`BackendModernSceneTarget`, made at the first
   scene and again when the back buffer changes size).
2. **The scene goes there.** Until the scene is done, whenever the engine
   sets the back buffer as render target 0 the front gives the Legacy adapter
   that image instead (`Device::DrawTarget`); `GetRenderTarget` still answers
   the back buffer. The image is marked with `legacy::SetLinearLight`, so the
   adapter builds each pixel shader that draws into it with its colour output
   decoded: red, green and blue clamped to [0, 1], as an 8-bit target would
   have kept them, then the sRGB curve (`legacy_shader::BuildSpirv`'s
   `linearOutput`, `DecodeColorOutput` in `vk_legacy_shader.cpp`). Clears of
   it decode their colour the same way. Alpha is not decoded. **Blending
   therefore happens in linear light**: effects, decals and other blended
   surfaces add and mix light rather than display values, and look different
   (brighter where they add up, softer where they fade).
3. **Copies during the scene.** The renderer resolves the scene for
   distortion after the sun (`R_RENDERTARGET_RESOLVED_POST_SUN`), a
   `StretchRect` from the back buffer. While the scene is in linear light the
   front has the tonemap write the rectangle into the copy instead
   (`BackendModernCopyScene`, pass `modern scene copy`), so the Legacy shaders
   that read it get display values as before.
4. **Done.** At `ModernSceneDone` the anti-aliasing runs on the linear-light
   image, where its 1 + brightness weighting belongs, and then the tonemap
   (`gfx_vk::modern::WriteDisplay`, `vk_modern_tonemap.cpp`,
   `shaders/tonemap.frag`, pass `modern tonemap`) writes the scene's viewport
   into the back buffer. The back buffer is render target 0 again, and post
   effects, 2D and the HUD draw on it as always. If the anti-aliasing is
   refused the tonemap still runs. A scene begun and never handed back is
   tonemapped whole at `Present` (logged once).

**The tonemap today is neutral**: the light clamped to 1 and encoded with the
sRGB curve, the exact inverse of the decode. An opaque surface therefore comes
back exactly as the Legacy path would have stored it (a half float keeps the
decoded value closely enough), and only blending differs. A tonemap with a
shoulder and the vision sets' film settings as its inputs come once the world
is drawn by the Modern path's own shaders and the scene holds light brighter
than white. Bloom is already there ([Bloom](#bloom)).

**Cost:** the scene image (8 bytes a pixel, 8 MB at 1280x800), the decode in
every pixel shader that draws the scene, one full-screen pass for the tonemap
and one more for each distortion resolve. `r_vkStats` and the device's
statistics print `vulkan d3d9: modern: N scenes in linear light (M copies read
from them), X ms GPU each for the tonemap`.

**Checks:** the D3D9 check's `modern_linear` scene draws a scene in linear
light with a clear, an opaque quad and white at 50% over black, reads a copy
during the scene, and draws 2D after it. The clear and the opaque quad come
back byte for byte, the blend is 188 (128 in gamma space), the copy holds the
same values, the back buffer outside the scene's viewport is left as it was,
and the 2D goes straight to the back buffer. With `KISAK_VK_MODERN_LINEAR=0`
the scene is Skipped and drawn as always (the blend is 128);
`test_vulkan_d3d9_front.py` runs it both ways. The translator's test builds
every pixel shader case with `--linear-output` too
([vulkan-legacy-shaders.md](vulkan-legacy-shaders.md)).

**Not yet:** the world-lit family drawn by the Modern path's own shaders
(colour maps decoded as sRGB, lightmaps and the light grid as light), the
tonemap's shoulder and the vision sets' film settings. The plan's MD1 lists
them; the retail shaders' lightmap decoding is being read on a machine with
the game first, as derived facts only.

## Effects

The renderer draws a view's effects and other blended surfaces (its emissive
surfaces, `R_DrawEmissive`) last, after the lit scene and the distortion
resolve. They were made for blending in display values: in linear light a
stack of dim additive sprites adds up to much less (PC screenshots 1: the
fire on mp_crash, a big yellow-white blob in Legacy, became a small orange
flame). So by default the Modern path finishes the scene before them:

* `RB_StandardDrawCommands` (`src/gfx_d3d/rb_draw3d.cpp`) calls
  `R_ModernSceneLit` just before `R_DrawEmissive`. The hook
  (`GfxModernHooks::sceneLit`) reaches the front's `ModernSceneLit`, which
  runs what `ModernSceneDone` would (anti-aliasing, bloom, tonemap); the
  effects then blend on the tonemapped back buffer exactly as in the Legacy
  path, and the host does nothing at the scene's `sceneDone`.
* The engine builds the hooks' `GfxModernScene` once per view
  (`R_ModernScene`), so the camera-cut test runs once whichever hook comes
  first.
* `KISAK_VK_MODERN_EFFECTS=linear` keeps the effects in the linear-light
  scene instead: they blend as light, bloom and are anti-aliased, and look
  weaker than made.

What finishing early costs: effects are not anti-aliased (they move by the
jitter, half a pixel at most, which soft sprites hide; it also means they
cannot ghost) and do not bloom. The Legacy glow, when bloom is off, still
sees them. The fullbright and debug draw modes have no emissive step and
finish at `sceneDone` as before.

**Checks:** the D3D9 check's `modern_effects` scene draws black, finishes
the lit scene, then adds two layers of grey 64: 128 on the display image
(and without linear light), 90 with `KISAK_VK_MODERN_EFFECTS=linear`.
`test_vulkan_d3d9_front.py` runs it both ways.

## Bloom

In linear light the Modern path draws bloom from the scene image in place of
the glow the vision set asks for (`r_glow`, the vision set's `glow*` values).
The Legacy glow cuts the 8-bit scene off at a display value, blurs a quarter-
size copy with a Gaussian and adds it back to display values. The bloom
works on light instead, before the tonemap, so it adds light where light is
(an HDR source blooms by how bright it really is once the scene holds such
light).

1. **What the renderer passes.** `R_ModernSceneDone` (`src/gfx_d3d/r_scene.cpp`)
   fills `GfxModernScene`'s glow fields when `R_UsingGlow` says the view
   would draw its glow: the radius as a Gaussian sigma in scene pixels (as
   `RB_ApplyGlowFilter` sizes it: radius x scene height / 1920), the
   intensity, the cutoff and the desaturation. The host passes them to the
   front's `ModernSceneDone` (`ModernSceneView::glow`).
2. **The chain** (`gfx_vk::modern::BuildBloom`, `vk_modern_bloom.cpp`,
   `shaders/bloom_down.frag`, `bloom_up.frag`, every pass named
   `modern bloom`). After the anti-aliasing, the scene's viewport goes down a
   chain of `R16G16B16A16_SFLOAT` images, each half the size of the one
   before, through the 13-tap filter of Jimenez ("Next Generation Post
   Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014). The first
   step keeps the light above the threshold with a soft knee, takes the
   desaturation out of it and weights each block of taps by 1 / (1 + luma)
   so single bright pixels do not flicker. Then each level is blurred with a
   3x3 tent and added to the level above it. The number of levels follows the
   glow's radius (`BloomLevelsFor`: log2 of the sigma plus 3, from 3 to 7, no
   level under 4 pixels).
3. **The tonemap adds it.** The tonemap samples the largest level and adds
   its light times the intensity divided by the number of levels, then
   tonemaps as before.
4. **The renderer leaves its glow out.** The host sets
   `GfxModernScene::glowDrawn`; `R_ModernDrewGlow` then keeps
   `RB_ProcessPostEffects` (`src/gfx_d3d/rb_postfx.cpp`) from drawing the
   Legacy glow for that view. Depth of field, the film colour settings and
   blur stay with the Legacy post effects, on the tonemapped back buffer, so
   the order changes: the bloom now comes before the film colour settings
   instead of after them.

**Mapping.** The glow's cutoff is a display value: its linear light is the
threshold, and the knee reaches a quarter of it below. The intensity is the
amount of the bloom's light added (shared among the levels). Exact parity
with the Legacy glow is not the aim, since the bloom adds light where the
glow added display values; it should look like the same vision set, wider and
softer. `KISAK_VK_MODERN_BLOOM=0` turns the bloom off, and the Legacy glow is
then drawn on the tonemapped scene as before. Without linear light there is no
bloom either.

**Cost:** the chain's images (about a third of the scene image's memory for
all levels), one pass per level down and one per level up but the last
(7 passes for 4 levels), a few small full-screen passes. `r_vkStats` and the
device's statistics print `vulkan d3d9: modern: N scenes with bloom in place
of the glow (P passes each), X ms GPU each`.

**Checks:** the D3D9 check's `modern_bloom` scene draws three scenes in
linear light: a white square on black with glow (light spreads around the
square, grey, fading with distance, and the renderer is told to leave its
glow out); grey below the cutoff with glow (nothing spreads); the white
square without glow (nothing spreads, the renderer keeps its glow).
`test_vulkan_d3d9_front.py` also runs it with `KISAK_VK_MODERN_BLOOM=0` and
with `KISAK_VK_MODERN_LINEAR=0`, where it never blooms.

**Not yet:** the chain in `R11G11B10_UFLOAT` (half the memory traffic; the
plan allows it once a test shows it is harmless); the film colour settings
moved into the tonemap, so that bloom and film keep the Legacy order.

## Next

From the plan's E1, in order:

* a reactive mask for effects;
* then the rest of linear light: the world-lit family drawn by the Modern
  path's own shaders into the HDR scene ([Linear light](#linear-light) has
  the image, the tonemap and bloom), a tonemap with a shoulder, and the
  vision sets' film settings in it.

The first laptop run of this piece should compare `r_visuals legacy` and
`modern` in the standing mp_crash scene: frame rate, the pass's GPU time,
and numbers for how much the edges change.
