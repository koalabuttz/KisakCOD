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
device it replaces.

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
  (alpha test, sampler state, point size) into uniform blocks. For each draw
  the adapter writes them into one per-frame allocation and a descriptor set
  points at it. Alpha test is D3D9 state, compared in the pixel shader.
* **Textures.** Each sampler slot gets the bound texture's sample view (with the
  format's swizzle: L8, A8, A8L8, X8R8G8B8 and the rest read as D3D9 reads them)
  and a sampler cached per slot and state. Empty slots read a 1x1 black
  texture of the kind the shader declares. Depth textures sample with compare
  (`LESS_OR_EQUAL`), as D3D9 shadow maps do.
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
* **Present.** The back buffer is an offscreen A8R8G8B8 image; `Present` blits it
  into the swapchain image and presents. A minimised window skips the frame.

## Resources and D3D9's lock rules

| Resource | How |
| --- | --- |
| Managed and default textures | A lock returns CPU memory for that level (or box); unlock uploads it through staging and frees the CPU copy. Compressed formats lock whole 4x4 blocks. |
| SYSTEMMEM and SCRATCH textures | CPU memory only, kept; the source of `UpdateTexture` and the destination of `GetRenderTargetData`. |
| Render targets and depth surfaces | 2D, one level. A texture with RENDERTARGET usage can be drawn to and sampled. |
| Static buffers | Device memory with a CPU shadow; unlock uploads the locked range. |
| Dynamic buffers | Mapped upload memory. `D3DLOCK_DISCARD` while the GPU still reads the old contents renames the buffer onto new memory; `NOOVERWRITE` writes in place; a plain lock of a busy buffer renames and copies the old contents. |
| Queries | Event queries finish when the GPU passes the point they were issued at. |

## Known limits

These are not needed by the shaders and calls checked so far; each is listed
so the renderer hookup can see where it stands.

* Occlusion queries are a stub: they finish at once and report 0xffffff pixels.
* A managed texture's CPU copy is freed after the upload, so locking it again
  to read returns zeros, not the old texels.
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
| `cube_volume` | Cube and volume textures. |
| `shader_model_2` | `vs_2_0` and `ps_2_0` through their fixed output registers. |
| `partial_precision` | `_pp` instructions computed at 32-bit precision, as D3D9 hardware and DXVK compute them (a 16-bit overflow would turn the pixel white). |
| `missing_input` | A vertex shader input the declaration does not supply reads zero. |
| `event_query` | An event query finishes after the GPU passes it. |

The scenes' shaders are D3D9 assembly in
`scripts/tools/vulkan/gen_legacy_check_shaders.py`, assembled with the shader
spike's assembler (plain Python) into `src/gfx_vk/tools/vk_legacy_check_shaders.h`;
`--check` rebuilds the header in memory and fails if it differs.

## Checks

`scripts/tests/test_vulkan_legacy_adapter.py --binary <check>` runs the check
several ways: bad arguments exit 2; an unknown scene fails at the `usage`
stage; one scene runs alone; and a full run passes every scene in order with no
mismatched pixel, presents every scene, renames the dynamic buffer, finishes
the event query, builds pipelines and has no validation errors.

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
(18 scenes), passes there too, and failed (white) before the fix below.

Laptop run 1 (2026-11-08, Intel RPL-U iGPU, Mesa 25.0.7 ANV, x86_64 and i386
RelWithDebInfo, SDL offscreen and Wayland, no validation layer installed): all
17 scenes match in all four runs, with identical scene lines; no adapter
warnings; the probe test still passes on both builds (report in the project
files, `renderer/vulkan-legacy-laptop-run-1.md`). Not yet run on the PC.

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
adapter, so the renderer's `R_HW_*` calls reach it unchanged. Next is
`r_backend vulkan`, which hands the renderer the front at device creation, then
mp_crash drawn through it and compared image by image with DXVK on the laptop
and the PC.
