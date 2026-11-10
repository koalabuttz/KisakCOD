# Vulkan Legacy adapter

Opt-in, off by default (`-DKISAK_BUILD_VULKAN_LEGACY=ON`). The part of
Direct3D 9 the renderer uses, built on the shared [Vulkan core](vulkan-core.md)
and the [Legacy shader translator](vulkan-legacy-shaders.md), with a check
program, `KisakCOD-vulkan-legacy-check`, that draws small D3D9-style scenes
through it and checks their pixels. Nothing of the engine is compiled in yet
and no game file is read; the renderer will reach the adapter through its
[D3D9 front](vulkan-d3d9-front.md).

## Where it fits

The renderer calls the graphics API only through the `R_HW_*` functions in
`r_rhi.h`, which are D3D9-shaped. The adapter keeps that shape, so the shim that
connects the two passes the renderer's values straight through: a device with
render states, sampler states, textures, surfaces, vertex and index buffers,
vertex declarations, shaders and their float constants, render targets, draws,
clears, copies, present and queries (`src/gfx_vk/legacy/vk_legacy.h`). It
takes D3D9's enum values as plain numbers, so it needs no `d3d9.h` and builds on
every platform the core does. One thread calls everything, as with the D3D9
device it replaces. It records its commands through the core's `Record`, so
with the core's record thread on (the D3D9 front's default) they run on that
thread; the adapter tracks the bound pipeline and dynamic state per command
buffer by the core's `Recording()` number.

| File | What |
| --- | --- |
| `vk_legacy.h` | The public API and the D3D9 values it uses. |
| `vk_legacy_device.cpp` | Device creation (D3D9's default states, 1x1 stand-in textures for empty samplers, the back buffer), `ResetState`, errors and one-time warnings. |
| `vk_legacy_resources.cpp` | Formats, textures and their locks, surfaces, buffers and their locks, shaders, vertex declarations. |
| `vk_legacy_draw.cpp` | States, pipelines, descriptors, constants, passes, draws, clears, copies, present, queries. |

## How a draw works

* **Pipelines.** The two shaders, the vertex declaration, the render-target and
  depth formats, the topology class, the fill mode, the blend state and the
  colour write masks make a 48-byte key; each key builds one pipeline through
  the core's pipeline cache. Everything the render states change often is
  dynamic state, so it never makes a new pipeline: viewport, scissor, cull mode,
  depth test, write and compare, depth bias, stencil (both faces), blend
  constants, topology and the vertex stride. The adapter counts pipelines built
  during a frame; the plan's target for play is none (warm-up comes with the
  renderer hookup).
* **Shaders.** `CreateShader` translates D3D9 bytecode once. A vertex shader's
  module is built at once; a pixel shader is linked to the vertex shader it is
  drawn with (outputs to inputs), and linked modules are kept per pair.
* **Uniforms.** The translator turns float constants and the states shaders need
  (alpha test, sampler state, point size) into uniform blocks, which live in
  per-frame memory. The blocks are dynamic uniform buffers: each pipeline
  writes its descriptor set once per frame (and per transient buffer), and
  later draws bind that set again with their own offsets. A layout with more
  blocks than the device's `maxDescriptorSetUniformBuffersDynamic` (8 on some
  drivers), or every layout with `KISAK_VK_UNIFORMS=static`, gets plain
  uniform buffers and a new set for each draw instead. Alpha test is D3D9
  state, compared in the pixel shader.
* **Less work for a draw like the one before.** The engine draws sorted by
  material, so most draws differ from the one before in a few vertex shader
  constants and nothing else. The adapter does and records only what changed:
  * *Uniform blocks.* Each set 0 binding remembers the block the frame's
    memory holds for it. A draw writes a block only when its bytes would
    differ: the float constants when a register the block reaches was set to
    another value (setting a register to the value it has changes nothing),
    the alpha test block when the alpha test state differs, the sampler state
    block when the shader or the kinds of the bound textures differ. The
    other blocks are the same for every draw. A draw's blocks must share one
    buffer under dynamic uniform buffers, so the first draw after the frame's
    memory moves to another buffer writes all of its blocks again.
  * *Binds.* The command buffer keeps its vertex buffers, index buffer and
    descriptor sets between draws (and between passes), so a bind equal to
    the one already there is not recorded. A new command buffer starts with
    nothing bound, and a change of pipeline layout forgets the sets.
    `DeviceStats::bindsSkipped` counts them.
  * *Pipeline and dynamic state.* The pipeline key and the dynamic state are
    made again only after something they are made of was set: a render state
    (except the three of the alpha test, which are in a uniform block), a
    shader, the vertex declaration, the viewport, the scissor rectangle, or
    the targets of the pass. Where the device fixes vertex strides or depth
    compare in the pipeline, the key also holds stream strides and bound
    textures and is made for every draw.

  With the validation layer on, the adapter checks every one of those
  shortcuts as it takes it: the kept block is written again elsewhere and
  compared, and the kept pipeline key and dynamic state are made again and
  compared. A difference fails the device (`uniform block N kept from an
  earlier draw...`, `the pipeline kept...`, `the dynamic state kept...`). So
  every check run with validation, and the game with `r_vkValidation` on,
  tests the rules above against what a draw without shortcuts would do.
* **Textures.** Each sampler slot gets the bound texture's sample view (with the
  format's swizzle: L8, A8, A8L8, X8R8G8B8 and the rest read as D3D9 reads them)
  and a sampler cached per slot and state. Empty slots read a 1x1 black
  texture of the kind the shader declares. Depth textures sample with compare
  (`LESS_OR_EQUAL`), as D3D9 shadow maps do. A draw that needs the same views
  and samplers as the draw before, in the same layout, binds that draw's
  texture set again instead of writing a new one. Destroying a texture forgets
  the remembered set, since its view handle can be handed out again.
* **Descriptor counts.** `DeviceStats::descriptorSets` counts sets allocated and
  written, and `descriptorSetsReused` the binds that took an earlier set. The
  D3D9 front's frame stats line prints both per frame.
* **Vertices.** Streams bind at their offsets. Shader inputs the declaration
  does not supply read zero from a small buffer, as D3D9 does; a draw from a
  stream with no buffer is skipped with a warning.
* **Half-pixel offset.** D3D9 samples pixel centres at integer coordinates; the
  viewport is shifted half a pixel, as DXVK does, so every pixel lands where
  D3D9 puts it.
* **Passes.** A pass opens lazily at the first draw or clear for the current
  targets and ends when targets change, a resource is uploaded, or a copy
  needs to run. Clears inside a pass are `vkCmdClearAttachments`, limited by
  viewport and scissor like D3D9's.
* **Frames in parts (the GPU feed).** A frame of many draws goes to the GPU
  in parts while it is still being drawn, as DXVK's does, instead of whole at
  its `Present`. After a draw, once the command buffer holds 128 draws
  (`KISAK_VK_GPU_FEED_DRAWS`) and the GPU has fewer than two submissions left
  to finish, what is recorded is submitted; for each further unfinished
  submission the adapter waits for 128 draws more, up to four times, so a GPU
  that is behind is left alone (DXVK's rule, counted in draws instead of
  command chunks). The draws that follow start a new command buffer and a new
  pass that loads the targets, so the picture is the same.
  Why: the engine holds every frame back until the GPU has finished the one
  before (`r_gpuSync 1`: the render thread waits for the last frame's fence
  when it has drawn a frame, and the main thread waits for it before it
  starts the next). A frame that is submitted whole cannot start on the GPU
  before it is fully recorded, so with that rule the GPU idles from the end of
  each frame until the next one has been recorded. Laptop run 8 measured it:
  6.3 ms of GPU work in every 8.2 ms frame, and 121 fps against DXVK's 150
  (`renderer/vulkan-backend-laptop-run-8.md` in the project files).
  Each part costs a tile-based GPU a store and a load of every target, so the
  feed is off by default on Apple, Arm, Qualcomm, Imagination and Broadcom
  GPUs and under MoltenVK. `KISAK_VK_GPU_FEED=0` turns it off and `=1` on
  anywhere; `=always` submits after every draw, which the checks use to give
  every draw of every scene a command buffer and a pass of its own. The
  device's `device ready:` log line says which it is (`GPU feed every 128
  draws`, `off`, `after every draw`), `DeviceStats::feedFlushes` counts the
  parts, and the front's `frame stats:` line prints them. Its GPU frame time
  runs from a frame's first command to its last, so with the feed it includes
  the time the GPU waited for the next part.
  The setting can change while the device runs: `GetGpuFeed` and `SetGpuFeed`
  (`GpuFeed`: on, after every draw, the draws to a part) take effect at the
  next draw and log `GPU feed now ...`. The D3D9 front drives them from the
  host's settings, which in the game are dvars, so one launch can compare
  feed settings ([front](vulkan-d3d9-front.md)).
  `GpuFeed::idleDraws` adds the idle rule, which is off unless set and is
  there to be measured: while the GPU has no submission left at all, a part
  goes out after that many draws, the frame's next such part after twice as
  many, and so on until the count reaches the plain one. Under `r_gpuSync`
  that is every frame's start: the engine lets the render thread begin a
  frame about when the GPU finishes the one before, so every draw recorded
  before the frame's first part goes out is time the GPU stands still (128
  draws are about half a millisecond of recording on the laptop). The rule
  adds a few parts to a frame and no more; `DeviceStats::feedIdleFlushes`
  counts them. While it applies, the adapter asks the GPU's progress every
  `idleDraws` draws, which costs a system call at most every 10 microseconds.
* **Present.** The back buffer is an offscreen A8R8G8B8 image; `Present` blits it
  into the swapchain image and presents. A minimised window skips the frame.
  There are two such images, and `Present` swaps them, as D3D9's DISCARD swap
  effect allows: the one just shown becomes the front buffer (`FrontBuffer`),
  and the next frame draws into the other. So the front buffer always holds
  the last frame shown, even while the next one is half drawn. The engine's
  screenshot command runs on the main thread while the render thread draws,
  so a screenshot read from the back buffer could catch a half-drawn frame
  (one PC run 2 shot had an odd tint). The 1.8x brightness PC runs 2-3
  measured was not the screenshot: that PC's desktop shows every window at
  0.553 times its brightness, so desktop captures, and real D3D9's windowed
  screenshots (a copy of the desktop), read darker than the frame drawn. In
  exclusive fullscreen, d3d9 and Vulkan screenshots agree (PC run 4).

## Resources and D3D9's lock rules

| Resource | How |
| --- | --- |
| Managed and default textures | A lock returns CPU memory for that level (or box); unlock uploads it through staging and frees the CPU copy. A level locked again without `DISCARD` is read back from the GPU once, and its CPU copy is kept from then on, so a lock keeps the texels it does not overwrite, as in D3D9 (the game's model lighting volume relies on this). Compressed formats lock whole 4x4 blocks. |
| SYSTEMMEM and SCRATCH textures | CPU memory only, kept; the source of `UpdateTexture` and the destination of `GetRenderTargetData`. |
| Render targets and depth surfaces | 2D, one level. A texture with RENDERTARGET usage can be drawn to and sampled. |
| Static buffers | Device memory with a CPU shadow; unlock uploads the locked range. |
| Dynamic buffers | Mapped upload memory. `D3DLOCK_DISCARD` while the GPU still reads the old contents renames the buffer onto new memory; `NOOVERWRITE` writes in place; a plain lock of a busy buffer renames and copies the old contents. |
| Queries | Event queries finish when the GPU passes the point they were issued at: the open recording's submission, or the last one submitted when nothing is being recorded (right after `Present`). Until laptop run 4, an event issued after `Present` waited for the next frame's submission instead. The engine polls such an event (its swap fence) before ending every frame, so the CPU and GPU never overlapped. A poll with `D3DGETDATA_FLUSH` that finds such a frame fence unfinished also submits what was recorded after it, once per issue: the engine has drawn the next frame by then and only waits, in steps of up to 1 ms, so the GPU gets that frame queued behind the one it is on instead of idling from the end of one until `Present` submits the next. `Present` then submits only the copy to the swapchain. `KISAK_VK_FENCE_FLUSH=0` turns this off, to compare; the front's `frame stats:` line counts these flushes. |
| Occlusion queries | Report the samples that passed the depth and stencil tests in the draws between `BEGIN` and `END`, as D3D9's do. The engine divides one such number by another for the sun's flare (`RB_DrawSunQuerySprite` over `RB_CalcSunSpriteSamples`' 16x16 sprite), so they are exact: precise Vulkan queries, taken through the core's occlusion counts ([vulkan-core.md](vulkan-core.md)). A Vulkan query lives inside one pass, and a D3D9 query does not: it may span target changes, the parts the GPU feed cuts a frame into, a `Present`, and other queries. So a query is a sum. The first draw after `BEGIN` starts a count; the pass's end, or any query's `BEGIN` or `END`, ends it; the next draw starts another; and every query open at the time shares each count. `GetData` adds a query's counts up once the GPU has passed its `END`, and until then answers as an event query does, flushing its own unsubmitted work when asked to. A query begun again starts from nothing; one never issued, or ended without a `BEGIN`, reports 0. A device without precise occlusion queries or host query reset cannot count: every finished query then reports 0xffffff, all samples passed, which is what the adapter always did before 2026-11-10 (the engine then takes the sun as fully visible wherever it is on screen, behind a wall too). `KISAK_VK_OCCLUSION_QUERIES=0` forces that path, and the `device ready:` log line says which it is (`occlusion queries counted` or `not counted`). The front's `occlusion queries:` line (at teardown, and from `r_vkStats` for a stretch of a run) gives the Vulkan queries they took, about one a frame while the game draws its sun, and what the queries answered (`DeviceStats::occlusionResults` and the three fields after it): the results, how many had no sample, their average and the newest. |

## The shader cache

Translating a D3D9 shader (dxbc-spirv's conversion and passes, and the vertex
shader's SPIR-V) was the largest part of a Vulkan load: laptop run 4 measured
1270 shaders and 5.5 s in `KisakCOD-mp`, every start
(`renderer/vulkan-backend-laptop-run-4.md`). `vk_legacy_shader_cache.cpp` keeps
each translation by its bytecode:

* In the process, for every device: a shader created again (another device, a
  `vid_restart`, the same bytecode twice) is not translated again.
* In a file between runs, when `DeviceDesc::shaderCachePath` is set (the game's
  hosts use `<fs_homepath>/vulkan/shaders.bin`, dvar `r_vkShaderCache`). The
  first device reads it; a device that goes writes it if anything new was
  translated, through a temporary file renamed over the old one.

The file holds the translator's revision, a digest of
`vk_legacy_shader.cpp`/`.h` and dxbc-spirv's commit that
`vulkan_shaders.cmake` computes at configure time (editing either source
re-runs the configure step). A file from another revision, another file
version, or with a wrong checksum is ignored whole and replaced at the next
write. The device stats count the shaders the cache served (`shadersCached`).
The front's `device stats` line gives the rest: `N translated`. Bytecode
created twice in one run is translated once, so even with the cache off this
is below the shader count.

## Known limits

These are not needed by the shaders and calls checked so far; each is listed
so the renderer hookup can see where it stands.

* Occlusion queries count samples only where the device has precise queries
  and host query reset (every desktop driver checked so far); elsewhere they
  report 0xffffff, every sample passed.
* Render targets are single-level 2D (no rendering into a cube face or a mip
  level yet).
* No fixed-function pipeline: draws without both shaders are skipped with a
  warning.
* Border colours: transparent black, opaque black and opaque white only.
* Shader model 1.x samplers without a declared type are treated as 2D.
* Fog, user clip planes, integer and boolean constants and texture-stage state
  read as zero.
* `StretchRect` covers what the core can blit: same-kind colour images, or
  depth to depth with point filtering. Others are skipped with a warning.

## The validation layer and depth compare

The Legacy shaders sample every 2D texture through a branch on a uniform: a
depth-compare sample for depth textures, a plain one otherwise. The validation
layer checks the compare instruction's format rule
(`VUID-vkCmdDraw-None-06479`) statically, against colour textures the branch
never compares, so it reports every textured draw. The core counts that one
message apart (`CoreStats::validationIgnored`, printed as
`validation_ignored`), not as an error, and logs it once.

## MoltenVK (macOS)

On a device with the portability subset (MoltenVK), the core enables every
subset feature the device has. The D3D9 formats need swizzled image views
(`imageViewFormatSwizzle`), and the validation layer holds the device to what
was enabled. The adapter also takes three other paths. The Legacy device's
ready line names them (`vertex stride static, depth compare fixed, 16-bit
targets wide`):

- **Vertex strides in the pipeline.** MoltenVK has
  `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` only from macOS 14 on Mac2 or
  Apple4 GPUs, and no Vulkan query says so. The pipeline key carries each
  stream's stride instead, and buffers are bound without strides.
- **Depth compare fixed per pipeline.** MoltenVK's shader converter
  (SPIRV-Cross) declares an image sampled both with and without a depth
  reference as a Metal depth texture, and a depth texture cannot sample colour.
  So the pixel shader is built per set of samplers that have a depth texture
  bound (`PipelineKey::depthSamplers`). `legacy_shader::BuildSpirv` replaces
  the `useDepthCompare` loads with constants and removes the branch not taken.
  Vertex shaders are built with no depth samplers.
- **16-bit render targets as B8G8R8A8.** Metal has the 16-bit packed formats
  only on Apple GPUs, so MoltenVK on Intel and AMD Macs (and the Intel CI VM)
  cannot render to R5G6B5, X1R5G5B5 or A1R5G5B5. A render target in one of
  them is made as B8G8R8A8 instead, with the same channel order and swizzle.
  The renderer's sun shadow pass uses an R5G6B5 colour target
  (`R_SetShadowmapFormats_DX`). This is decided per device, not only on
  MoltenVK. Copies between such a target and a lockable texture of the D3D9
  format are refused, since the formats differ.

`KISAK_VK_VERTEX_STRIDE=static`, `KISAK_VK_DEPTH_COMPARE=fixed` and
`KISAK_VK_RENDER_TARGETS=wide` force each path on any device. The Linux
lavapipe job runs the adapter and D3D9 front checks with all three set, and
with `KISAK_VK_UNIFORMS=static` (plain uniform buffers, above), with the
validation layer required.

## Building and running

```sh
cmake -S . -B build-vk -G Ninja -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=x86_64 \
  -DKISAK_BUILD_VULKAN_PROBE=ON -DKISAK_BUILD_VULKAN_SHADERS=ON -DKISAK_BUILD_VULKAN_LEGACY=ON \
  -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3
cmake --build build-vk --target KisakCOD-vulkan-legacy-check
KisakCOD-vulkan-legacy-check [--validation off|auto|require] [--device N] [--scene NAME]
```

On Windows the standalone `scripts/vulkan_probe` build takes the same three
options. The check opens a small window, draws each scene into a 128x128 back
buffer (or a render target), reads it back with `GetRenderTargetData`, compares
known pixels (within 2 per channel) and presents. Output is `key=value` lines:
one `legacy_check.scene=<name> points=<n> mismatched=<n>` line per scene,
statistics, the validation counts, and `legacy_check=passed` or
`legacy_check=failed stage=<stage>` last. Exit status 0 is a pass, 1 a failure,
2 a usage error.

| Scene | Checks |
| --- | --- |
| `clear` | A full clear, a scissor-limited clear and a rectangle clear. |
| `vertex_color` | A shader-model-3 draw with per-vertex colour; a vertex shader constant moving a quad; quad edges on exact pixels. |
| `half_pixel` | Every one of the 16384 pixels of a full-target quad showing its own texel, so texel centres land on pixel centres. |
| `texture_levels` | Mip levels uploaded one by one, sampled one by one. |
| `formats` | L8, A8, A8L8, X8R8G8B8, A8B8G8R8, R5G6B5, A1R5G5B5, DXT1 and DXT5 read as D3D9 reads them (formats the device lacks are skipped). |
| `alpha_test` | Alpha test with GREATER and LESS against a reference. |
| `blend` | Source-alpha blending, separate alpha blend, additive blending with a colour write mask. |
| `depth` | Depth test, write and compare functions, depth bias. |
| `stencil` | Stencil REPLACE with colour writes off, then an EQUAL test. |
| `cull` | D3D9's clockwise and counter-clockwise culling. |
| `render_to_texture` | Drawing into a render-target texture, then sampling it. |
| `dynamic_buffer` | `DISCARD` and `NOOVERWRITE` locks across draws in one frame (renames counted). |
| `static_buffers` | Static vertex buffers with 16- and 32-bit index buffers, a base vertex, and a rewrite between draws. |
| `cube_volume` | Cube and volume textures; a volume locked again keeps the slice it does not write, with one read-back. |
| `shader_model_2` | `vs_2_0` and `ps_2_0` through their fixed output registers. |
| `partial_precision` | `_pp` instructions computed at 32-bit precision, as D3D9 hardware and DXVK compute them (a 16-bit overflow would turn the pixel white). |
| `missing_input` | A vertex shader input the declaration does not supply reads zero. |
| `event_query` | An event query finishes after the GPU passes it. |
| `occlusion_query` | Occlusion queries report exact sample counts: a quad's pixels, the visible half of a quad behind another, 0 for a hidden one, and two overlapping queries across a change of targets (0xffffff each where the device cannot count). |
| `state_reuse` | What a draw writes and records when it is like the one before, by the constant bytes written and the binds skipped: the same state again (nothing written, set 0 and the vertex buffer not bound again), a vertex shader constant set to another value (its block alone), to the same value and outside the block (nothing), another texture, an unbound and a rebound texture (the sampler state block), the alpha test turned on and changed (its block), a layout with pixel shader constants and back, a new command buffer (binds recorded again, nothing written), indexed draws that keep their buffers, a stream offset and 32-bit indices, the frame's memory moving to another buffer (every block again under dynamic uniform buffers), a viewport, a scissor rectangle and a vertex declaration each changed alone between two draws, the same vertex shader code created a second time (another object, so its pipeline is looked up), a pixel shader constant block that was last written before the frame's memory moved (written again under dynamic uniform buffers, kept otherwise), and the next frame (every block again). Each step's pixel is read back. The counts follow the device: no bind is skipped with `KISAK_VK_GPU_FEED=always`, where every draw has a command buffer of its own. |

The scenes' shaders are D3D9 assembly in
`scripts/tools/vulkan/gen_legacy_check_shaders.py`, assembled with the shader
spike's assembler (plain Python) into `src/gfx_vk/tools/vk_legacy_check_shaders.h`;
`--check` rebuilds the header in memory and fails if it differs.

## Checks

`scripts/tests/test_vulkan_legacy_adapter.py --binary <check>` runs the check
several ways: bad arguments exit 2; an unknown scene fails at the `usage`
stage; one scene runs alone; and a full run passes every scene in order with no
mismatched pixel, presents every scene, renames the dynamic buffer, finishes
the event query, counts the occlusion queries (always on lavapipe, never with
`KISAK_VK_OCCLUSION_QUERIES=0`), builds pipelines and has no validation
errors. The
`shader_cache` scene translates two shaders, takes one again from memory,
writes and reads back a cache file in the temporary directory, draws with the
shaders read from it, and checks that a damaged file is refused.

The hosted CI job `vulkan_window` (Linux) builds the check for x86_64 in Debug
and RelWithDebInfo, runs `gen_legacy_check_shaders.py --check`, and runs that
script on Mesa's lavapipe through SDL's offscreen video driver with the
validation layer and synchronisation checks required. The Windows workflow's
`vulkan-probe` job builds it with MSVC for x64, checks that it does not import
`vulkan-1.dll` and that it starts, and adds it to the `KisakCOD-vulkan-probe-x64`
artifact for a run on the PC.

Results so far (2026-11-08, cloud sandbox, no GPU, x86_64 Debug, lavapipe from
Mesa 25.2.8, validation with synchronisation checks): all 17 scenes match, no
validation errors, 11 pipelines. The `partial_precision` scene, added later
(18 scenes), passes there too, and failed (white) before the fix below. The
`occlusion_query` scene (2026-11-10, same sandbox, 20 scenes by then) gets
its sample counts exactly, in Debug and RelWithDebInfo, with the record thread
on and off and the GPU feed off, at its default and after every draw, and
reports 0xffffff for each query with `KISAK_VK_OCCLUSION_QUERIES=0`. No GPU
has run it yet.

Less work for a draw like the one before (2026-11-10, same sandbox, 21 scenes
with `state_reuse`): its 59 points match in Debug and RelWithDebInfo with
validation, with the GPU feed at its default, off and after every draw, the
record thread on, static uniforms (also with a submission after every draw),
static strides with fixed depth compare and wide targets, and occlusion
counting off; the D3D9 front's check passes with the same switches and with
its record thread off, and both checks under ThreadSanitizer. Measured with
callgrind on the front check's `bench_runs` scene (frames of 768 draws in runs
of eight that share their state), the drawing thread spends 2,914 instructions
on a draw where it spent 4,626, and the constants written per frame fall from
464 KB to 14 KB. Each of the 18 rules that say when something kept is stale
was taken out in turn, and the checks fail on every one: 11 at the adapter's
own cross-check, 7 by a wrong pixel, a wrong count or a validation error (5 of
the 18 went unseen at first, and the scene got a step for each). No GPU has
run it and no frame rate was measured: what it is worth in the game is for a
laptop run to say.

Laptop run 1 (2026-11-08, Intel RPL-U iGPU, Mesa 25.0.7 ANV, x86_64 and i386
RelWithDebInfo, SDL offscreen and Wayland, no validation layer installed): all
17 scenes match in all four runs, with identical scene lines; no adapter
warnings; the probe test still passes on both builds (report in the project
files, `renderer/vulkan-legacy-laptop-run-1.md`). This standalone check has
no recorded run on the PC; the game itself draws through the adapter there
([vulkan-d3d9-front.md](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan)).

The translator computes `_pp` (partial precision) at 32 bits:
`vk_legacy_shader.cpp` turns off dxbc-spirv's mapping of minimum-precision
types to 16-bit floats, as DXVK does unless `d3d9.useFP16` is set. With the
16-bit mapping, the game's specular terms overflowed and every lit surface of
mp_crash drew brighter than through DXVK (laptop Vulkan back end runs 1 and 2,
`renderer/vulkan-backend-laptop-run-{1,2}.md`: the difference went away with
`r_specular 0`).

Synchronisation validation found one core hazard while this was written: a
read-back's memory, reused after the host had waited for the copy, was written
again without a barrier the layer could see (it does not track host waits on
timeline semaphores). The read-back barrier now covers later commands too.

## Next

The [D3D9 front](vulkan-d3d9-front.md) implements the D3D9 interfaces on the
adapter, so the renderer's `R_HW_*` calls reach it unchanged. `r_backend
vulkan` hands the renderer the front at device creation; mp_crash drawn
through it has been compared image by image with DXVK on the laptop and with
real D3D9 on the PC
([vulkan-d3d9-front.md](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan)).
