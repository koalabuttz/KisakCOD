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
([Linear light](#linear-light)), with lit surfaces keeping what they
compute over white ([Lit surfaces](#lit-surfaces)), bloom from that image in
place of the vision set's glow ([Bloom](#bloom)) and auto exposure before the
tonemap ([Auto exposure](#auto-exposure)). Velocity comes from the
camera, and for entities' models (rigid and skinned surfaces, the view model's
included) from a velocity pass of their own
([Velocity](#velocity-of-what-moves-on-its-own)); effects do not have their own
yet. No material is drawn by shaders of the Modern path's own yet.

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
  the entity number plus one, in what was retail's padding). Entities drawn
  as one model (scene models, such as parked cars) record theirs the same way
  in `R_SkinXModel`, and dynamic entities (props the physics moves) record
  `0x8000` plus their index plus one, so they never share an owner with an
  entity. `R_SkinXModel` used to leave the rest of each record's info as the
  stack had it; it is zeroed now, because the owner and first bone are the
  key. When a rigid
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

What it does not cover yet: brush models and effects (whether drawn on the display image or, with
`KISAK_VK_MODERN_EFFECTS=linear`, in the scene). `KISAK_VK_MODERN_VELOCITY=0`
turns the pass off, for comparisons.

**Debug view.** `KISAK_VK_MODERN_DEBUG=velocity` draws the velocity image over
each anti-aliased scene (`vk_modern_debug.cpp`, `debug_velocity.frag`), so a
wrong or missing report shows at a glance. Each pixel with motion of its own
is coloured, and every other pixel is left as the scene drew it. The hue is
the direction the surface came from on screen: red from the right, cyan from
the left, about green from above, about violet from below. The brightness
grows with the distance, from a quarter at none to full at 6 pixels or more,
so a reported surface that did not move shows dim grey. TAA's history is kept
apart from the image the view draws on, so the view never feeds back into it.
It is drawn only in scenes that have a velocity image.

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

**The tonemap's curve is Khronos PBR Neutral** by default
([Tonemap curves](#tonemap-curves)): below 0.76 of light (display 227) it is
the exact inverse of the decode, so an opaque surface comes back as the Legacy
path would have stored it (a half float keeps the decoded value closely
enough) and only blending differs; brighter light, which lit surfaces now keep
([Lit surfaces](#lit-surfaces)), rolls off towards white instead of clipping.
The vision sets' film settings as the tonemap's inputs come later. Bloom is
already there ([Bloom](#bloom)).

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

**Not yet:** the vision sets' film settings in the tonemap. Lit surfaces
keep what they compute over white ([Lit surfaces](#lit-surfaces)); other
tonemap curves are there to compare ([Tonemap curves](#tonemap-curves)).

## Lit surfaces

The game's lit shaders (`lm_` world surfaces lit by lightmaps, `lp_` models
lit by the lighting volume, `l_` dynamic lights) add their light terms in
display values: a world surface adds the two halves of its secondary
lightmap and the sun, a model the lighting volume's ambient and the sun.
CoD4's maps were balanced for that. What such a shader computes over white
(sunlit sand, bright metal: colour times a light over 1) the 8-bit target
used to clip.

**Default: kept over white.** A lit pass's stored colour is decoded as every
other draw's, but without the clamp to 1 first: below white the scene is
exactly the game's, and a colour computed over white follows the sRGB curve
on (1.5 becomes 2.5 in linear light), so the tonemap and bloom see real
highlights. Unlit draws, effects, the sky and 2D keep the clamp.

* **Which draws.** `R_SetupPass` (`src/gfx_d3d/r_shade.cpp`) calls
  `R_ModernPassInputs` (`r_scene.cpp`) for every pass between `sceneBegin`
  and `sceneLit`; a pass is lit when its pixel shader's name starts with
  `lm_`, `lp_` or `l_`.
* **How.** The hook (`GfxModernHooks::litInputs`, called only when what it
  says changes, and with nothing at `sceneLit`) reaches the front's
  `SetModernLitInputs` and `legacy::SetLinearInputs`. For a draw into the
  linear-light scene the adapter keys the pipeline on it
  (`PipelineKey::linearOutput` 2 and the flags) and builds the pixel shader
  with `legacy_shader::BuildSpirv`'s `linearInputs` flag `kLinearOverWhite`:
  colour output 0 kept from going negative, then decoded.
* `KISAK_VK_MODERN_LIT=0` turns lit passes back into ordinary draws (clamped).

**Decoded inputs (an experiment, `KISAK_VK_MODERN_LIT=inputs`).** Every
colour the shader reads is decoded before its arithmetic and the result is
stored as computed: material colour and specular maps, the secondary
lightmap and reflection probe (custom samplers, bound by the BSP and static
model draws at s3 and s1 themselves, found from
`MaterialPass::customSamplerFlags`), the lighting volume (doubled: its
samples become decode(2v) / 2, as the shader doubles them), the fog, sun,
light and material colour constants, and the vertex colour. Not decoded:
the primary lightmap and shadow maps (visibility), normal maps, light
attenuation, every alpha, and detail maps (added around grey: colour +
detail - 0.5). **It does not suit CoD4's data:** the decode is convex, so a
sum of decoded terms is darker than the decoded sum, the more so the more
terms it has; sunlit ground (two lightmap halves plus the sun) lost the most
and the sun all but vanished next to the shade (PC lit runs 1 and 2,
2026-11-13). Its tools stay for experiments: `KISAK_VK_MODERN_LIT_PARTS`
names the kinds to decode (`colormap`, `specmap`, `detail`, `lightmap`,
`probe`, `volume`, `fog`, `sun`, `light`, `material`, `vertex`; `all`, the
default, is every kind but `detail`; `none`), and `KISAK_VK_MODERN_LIT_LOG=1`
prints once per lit pixel shader what it decodes and what each of its
sampler and constant registers binds (`Modern lit inputs: <shader>: ...`).
The engine reads these three.

**Cost:** one pixel shader module per lit shader (the same count as without
it). `r_vkStats` prints `vulkan d3d9: modern: N scenes with lit surfaces in
linear light, kept over white (C changes of lit inputs each)` (`computed
from decoded inputs` in the experiment).

**Checks:** the D3D9 check's `modern_lit` scene draws a grey 128 colour map
times a white vertex colour plus a light of 64 in a constant: 192 (128 + 64)
without lit inputs and kept over white, encode(decode(128) + decode(64)) =
141 with decoded inputs, 197 with the colour map doubled.
`test_vulkan_d3d9_front.py` also runs it with `KISAK_VK_MODERN_LIT=0` and
`KISAK_VK_MODERN_LINEAR=0` (192 everywhere). The translator tool takes
`--linear-inputs S,D,C,V` (V the flags: 1 the colour input, 2 over white).

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

## Tonemap curves

The tonemap can turn light into display values with one of four curves,
chosen at start-up with the environment variable `KISAK_VK_MODERN_TONEMAP`.
The default is `neutral`, chosen by eye after the four were compared on
mp_crash, mp_backlot, mp_bog and mp_farm (2026-11-13): it keeps the game's
look and only rolls off what would have clipped. The other three stay for
comparison.

| `KISAK_VK_MODERN_TONEMAP` | Curve |
|---|---|
| `clamp` | light clamped to 1, then the sRGB encode: an opaque surface comes back as Legacy stored it |
| `neutral` (default) | the Khronos PBR Neutral shoulder without its toe: unchanged up to 0.76 linear (display 227), then light above white rolls off towards 1 and its colour desaturates a little |
| `agx` | AgX (Sobotka; Wrensch's polynomial fit): a log-encoded film-like curve, lower contrast, bright colours fade to white |
| `aces` | the ACES fit by Stephen Hill (input scaled by 1/0.6 so mid grey stays near the Legacy value): more contrast, deeper shadows |

What each curve makes of grey light (display bytes after the sRGB encode):

| Linear light | 0.05 | 0.216 (Legacy 128) | 0.5 | 1 | 2 | 4 |
|---|---|---|---|---|---|---|
| clamp | 63 | 128 | 188 | 255 | 255 | 255 |
| neutral | 63 | 128 | 188 | 241 | 251 | 253 |
| agx | 70 | 136 | 174 | 202 | 224 | 239 |
| aces | 50 | 141 | 197 | 226 | 242 | 250 |

Only `neutral` keeps everything below 0.76 linear exactly as Legacy; the other
two change the whole picture. A name the switch does not know falls back to
`neutral`. The device's log names the curve once, when the tonemap is made:
`vulkan d3d9: modern: tonemap curve neutral` (with `(KISAK_VK_MODERN_TONEMAP
names no curve: clamp, neutral, agx or aces)` after an unknown name). The
curve is applied to the scene and to bloom's added light, before the Legacy
post effects (vision set film settings, blur), which still run on the
tonemapped back buffer.

**Checks:** the D3D9 check's `modern_tonemap` scene draws Legacy grey 128,
white, and white added two and four times in linear light, and expects the
curve's bytes for each (neutral's with an unknown name, clamp's with
`KISAK_VK_MODERN_LINEAR=0`). `modern_linear` and `modern_bloom` expect their
brightest colours through the curve in use. `test_vulkan_d3d9_front.py` runs it with every curve and with
an unknown name.

**Not yet:** the vision sets' film settings (contrast, brightness,
desaturation, tint) moved into the tonemap.

## Auto exposure

Before the tonemap, the linear-light scene is multiplied by an exposure that
follows its brightness, the way an eye or a camera adapts: a dark room is
brought up a little and a bright yard down, within a limited range, so the
picture stays the game's at rest. It is the first stage of the Modern lit
shaders plan (project files: the "Modern lit shaders plan" document).

Each scene (`modern::MeasureExposure`, `vk_modern_exposure.cpp`, three small
passes timed as "modern exposure"):

1. **Measure** (`exposure_measure.frag`): the scene's area on a 64x64 grid,
   each texel four bilinear taps; the log2 of their luminance (Rec. 709),
   clamped to [-12, 4] so a patch of sky or a flash cannot swing the average
   far, weighted from 1 in the middle of the view to 1/4 in its corners.
2. **Reduce** (`exposure_reduce.frag`): 8x8 blocks of the grid added up.
3. **Adapt** (`exposure_adapt.frag`, into a 1x1 image): the weighted average
   log2 luminance, the EV that brings it to the key (log2 key minus the
   average), clamped to the range, and last scene's EV moved towards it with
   a time constant of 1 s when the scene got brighter and 2.5 s when it got
   darker (`1 - exp(-dt / tau)` of the way per scene, dt clamped to 1 s). A
   camera cut, and the first scene, take the target at once. Two 1x1 images
   take turns, so nothing is read back to adapt.

The tonemap multiplies the scene's light and the bloom's by exp2 of that EV
before its curve. A copy of the scene read during it (the renderer's resolve
for distortion) uses the last scene's exposure. Effects drawn on the display
image after the scene (the default, [Effects](#effects)) are not exposed.

| Parameter | Default | Switch |
| --- | --- | --- |
| On | yes | `KISAK_VK_MODERN_EXPOSURE=0` turns it off (exposure 0 EV) |
| Key: the average luminance that needs no change | 0.115: mp_crash's and mp_backlot's outdoor views measured 0.117 and 0.110 (PC exposure run 1), so a typical outdoor scene stays at about 0 EV | `KISAK_VK_MODERN_EXPOSURE_KEY=<value>` |
| Range | -1 to +1.5 EV | `KISAK_VK_MODERN_EXPOSURE_RANGE=<min>,<max>` |

The statistics line, one in 16 results read back a few frames later:
`vulkan d3d9: modern: N scenes with auto exposure (M taken at once), exposure
E EV on average (min to max), scene log2 luminance L on average (W EV
wanted), T ms GPU each`. L and W are what a key is chosen from.

What PC exposure run 1 measured with the first key (0.1), at the framelog's
standing views (RTX 2080 SUPER): mp_crash outdoors -0.23 EV (L -3.09),
mp_crash's covered arcade +1.18 EV (L -4.51), mp_backlot -0.14 EV (L -3.18),
mp_bog at night +0.70 EV (L -4.02); no pumping standing still (the exposure
within 0.02 EV, two captures 3 s apart alike), about 0.04 ms GPU per scene.
With 0.115 the indoor view gets about +1.4 EV and mp_bog +0.9 EV.

**Checks:** the D3D9 check's `modern_exposure` scene draws four uniform greys
and expects grey 100 brought exactly to the key (shown 95), white held at -1
EV, grey 40 at +1.5 EV, and grey 40 after grey 100 without a cut only part
of the way there. `test_vulkan_d3d9_front.py` runs it with
`KISAK_VK_MODERN_EXPOSURE=1` and every other scene with it off, since auto
exposure moves their colours.

## Lit shaders of our own

The second stage of the Modern lit shaders plan: lit passes drawn with
shaders of the Modern path's own, which add their light in linear light,
in place of the game's. The first is the world's sunlit family,
`lm_[hsm_]sun_<mode>c0[d0][n0][s0]`: one source, `lm_sun.frag`, compiled
for each set of maps (16 builds), with the mode a specialization constant.

| Part of the name | Define | Reads, beyond the base |
| --- | --- | --- |
| (base) `lm_sun_r0c0` | | colour map s0, primary lightmap s2, secondary lightmap s3; fog colour c0, sun direction c17, sun colour c18; texcoord 0 (colour and lightmap UVs), texcoord 1 (normal, fog), colour 0 |
| `hsm_` | `LM_SUN_SHADOW` | the sun's shadow map s4, its partition switch c2 and scale c4; texcoord 4 (the shadow-map coordinate) |
| `n0` | `LM_SUN_NORMAL` | a normal map at the next sampler (s4, s5 with the shadow map); texcoords 2 and 3 (binormal, tangent) |
| `s0` | `LM_SUN_SPECULAR` | the reflection probe (cube) s1, a specular map after the normal map (in its place without one); envMapParms c5, the sun's specular colour c19; texcoord 5 (eye-to-point) |
| `d0` | `LM_SUN_DETAIL` | a detail map after the others; detailScale at c5 (c6 with `s0`) |
| `r0` / `b0` / `t0` | (mode) | opaque / blended / alpha-tested: nothing more |

What ours computes (from the game shaders' maths, world-lit shader facts 1
to 3 in the project files, 2026-11-13):

* **albedo** = colour map (plus the detail map around grey, `d0`) times
  vertex colour, each decoded from sRGB;
* **sky** = the secondary lightmap's two halves combined as the game
  combines them (top plus bottom times N.D, with the flat normal or, `n0`,
  the normal map's slopes, which also scale the top: one baked light,
  encoded in two images), then decoded;
* **sun** = what the sun adds to that light in the game's sum, decoded:
  decode(sky + sun) - decode(sky), the sun being shadow times N.L times
  the sun's colour, N from the normal map in the tangent frame with `n0`.
  The shadow is the primary lightmap's sun visibility, or where the shadow
  map covers the surface (and the lightmap's is not 0), the game's four
  depth-compare taps a quarter each, near and far partitions blended over
  w 0.75 to 1, faded back to the lightmap's towards the far partition's
  edge;
* **specular** (`s0`) = likewise what the game's specular adds to the
  game's diffuse colour, decoded: the specular map times the Fresnel
  (envMapParms) times (shadow times the sun's highlight, exponent from the
  gloss, times envMapParms.w, the sun's specular colour and the sun
  strength, plus the reflection probe at LOD 6 - 8 gloss times its alpha);
* **colour** = albedo times (sky times the sky strength plus sun times the
  sun strength) plus specular, then the fog
  colour (decoded) blended over it in linear light by the vertex fog factor.
  `b0` premultiplies the albedo and fog colour by colour alpha times vertex
  alpha, which is the output alpha; `t0` keeps alpha 0.5 and over and writes
  black with alpha 0 elsewhere, as the game's shader does; then the alpha
  test of the render state.

So with both strengths 1 every surface is as bright as the game's:
decode(colour times light) is decode(colour) times decode(light). The game
added its terms in display values, so in linear light a term's share
depends on what it is added to: a single sun strength over decoded terms
matched the game at 2.9 on Crash and 4.3 on Backlot (PC sun run 2), which
is why each share is taken from the game's sum. The terms stay apart, so
the **sun strength** scales the sun's share (a punchier sun above 1). The
sum is not clamped, so the tonemap and bloom see sunlit highlights as they
are.

* **Which passes.** `R_ModernPassInputs` (`r_scene.cpp`) names one in
  `GfxModernLitInputs::shader` (a code: 0x40 plus flags for the shadow 1,
  normal 2, specular 4 and detail 8 maps and the mode times 16) when the
  pass's pixel shader's name is one of the family (`_sm3`; `_sm2` only for
  `r0c0` without maps; the lightmaps, and the probe with `s0`, bound as
  custom samplers) and its pixel arguments bind exactly what ours reads,
  where ours reads it; any other pixel argument keeps the game's shader.
  `KISAK_VK_MODERN_LIT_LOG=1` prints each lit pixel shader's arguments and
  the code it was given.
* **How.** It travels with the over-white lit inputs (front
  `ModernLitInputs::shader`, `legacy::SetLinearInputs`'s `shader`,
  `PipelineKey::linearShader`). Building the pipeline's pixel module, the
  adapter (`vk_legacy_modern_lit.cpp`) takes our SPIR-V (compiled at build
  time into `vk_legacy_shaders.h`), moves its inputs from their placeholder
  locations (texcoord N at N, colour 0 at 10) to where the vertex shader
  writes those semantics, and sets the strengths, the depth reference
  clamp, the mode and whether it runs the alpha test (not where the compare
  is fixed as always) as specialization constants' defaults. The pipeline
  layout and descriptors stay the game shader's, so the adapter uses ours
  only where that shader reads at least what ours does (constants to c18 or
  c19, 2D images and the probe cube at those samplers, the alpha test's
  block), the vertex shader writes each input as one four-component vector
  alone at its location and, for the shadow, s4 is a depth image; otherwise
  it builds the game's shader, over white, and counts it.

| Switch | Default |
| --- | --- |
| `KISAK_VK_MODERN_SHADERS=0` | our shaders on (with linear light and lit passes on) |
| `KISAK_VK_MODERN_SUN_STRENGTH=<value>` | 1 (as bright as the game's) |
| `KISAK_VK_MODERN_SKY_STRENGTH=<value>` | 1 |

The statistics line, after the lit surfaces' one: `vulkan d3d9: modern: lit
shaders of our own: C changes to one each scene, N pixel modules built as
one (R could not be), sun strength S, sky strength K`.

**Checks:** the D3D9 check's `modern_lit_shader` scene draws a stand-in for
`lm_hsm_sun_r0c0` (it reads the same samplers and constants but shows black)
as `lm_sun` and `lm_hsm_sun`: grey 128 colour map, lightmap halves 64 each,
grey 128 sun overhead, a shadow map at depth 0.5. It expects 0.2159 *
(0.2159 * sky + 0.2159 * sun) of light without a shadow and with a reference
in front of the stored depth, the lightmap's alone behind it, and half fog
towards red blended in linear light; black with our shaders off. A second
stand-in, for `lm_hsm_sun_r0c0d0n0s0`, is drawn as the `d0n0s0` variant
opaque, blended and alpha-tested (kept and dropped), and as `lm_sun_r0c0n0`
with a normal map tilted along the tangent; the expectations follow the
game's formulas.
`test_vulkan_d3d9_front.py` also runs it with `KISAK_VK_MODERN_SHADERS=0`.

## Next

From the plan's E1, in order:

* a reactive mask for effects;
* then the rest of linear light: a tonemap with a shoulder and the vision
  sets' film settings in it ([Linear light](#linear-light) has the image,
  the tonemap and bloom; [Lit surfaces](#lit-surfaces) keeps the lit
  families' colours over white).

The first laptop run of this piece should compare `r_visuals legacy` and
`modern` in the standing mp_crash scene: frame rate, the pass's GPU time,
and numbers for how much the edges change.
