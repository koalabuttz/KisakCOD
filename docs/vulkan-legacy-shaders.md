# Legacy shader translator

Opt-in, off by default (`-DKISAK_BUILD_VULKAN_SHADERS=ON`). This is the second
piece of the Vulkan back end's Legacy path, after the [Vulkan core](vulkan-core.md).
It turns the game's Direct3D 9 shader bytecode (vs/ps 1.1 to 3.0) into SPIR-V
for Vulkan 1.3. Nothing of the engine uses it yet; the Legacy adapter will call
it when a material's shaders load.

## Where it fits

The shader spike ([shader-translator-spike.md](shader-translator-spike.md))
chose dxbc-spirv (MIT, by the DXVK author), the converter DXVK 3.1 uses for
D3D9. On the laptop, DXVK 3.1.1 drew CoD4 through it the same as 2.7.1 did.
dxbc-spirv turns the bytecode into its own IR. What D3D9 keeps outside the
shader reaches the IR as "legacy inputs":

- constant registers;
- alpha test, fog, clip planes and point sprites;
- the bound textures' types and depth-compare state;
- ps 1.x bump-mapping state.

DXVK lowers those inputs to its own binding model. This translator does the
same for ours, in `src/gfx_vk/shader/vk_legacy_shader.{h,cpp}`:

| Step | What |
| --- | --- |
| Translate | Converts the bytecode (dxbc-spirv's SM1-3 front end), cuts each constant array to the registers the shader reads, lowers every legacy input to a uniform buffer at a fixed binding, runs the legalisation DXVK 3.1.1 runs for D3D9 shaders, and records what the shader reads and writes. The result is serialised IR plus reflection, which can be cached. Runs once per shader, at load. |
| BuildSpirv | Turns the IR into SPIR-V. For a pixel shader given its vertex shader: if the pixel shader's inputs are not where the vertex shader writes them, they are moved there by semantic, and inputs the vertex shader does not write read zero. Runs once per vertex/pixel shader pair. |

The descriptor layout (constants and helpers in `vk_legacy_shader.h`):

| Set | Bindings |
| --- | --- |
| 0, uniform buffers | Vertex shader at 0-8, pixel shader at 16-24, one per `Uniform`: float constants, int constants, bool constants, alpha test, fog, clip planes, point sprites, sampler state, texture stage. |
| 1, textures | Pixel shader images at 0-15 and samplers at 16-31; vertex shader (vs 3.0 vertex textures) images at 32-47 and samplers at 48-63. Images and samplers are separate descriptors. |

Every binding is optional. A shader declares only the blocks and samplers it
reads (`Reflection::uniforms`, `Reflection::samplers`), so the adapter writes
only those.

dxbc-spirv lays uniform blocks out tightly ("scalar block layout"), so the core
now requires `scalarBlockLayout`. Every device we target has it: lavapipe, ANV,
the NVIDIA driver and MoltenVK. The block structs in the header match the
layout byte for byte. Constant registers have a 16-byte stride, and bools
have a 4-byte stride.

Reflection gives the adapter:

- **Constants:** how many float, int and bool registers to bind. When the
  shader indexes c[] with a0 or aL, it gets the stage's whole range instead.
- **Samplers:** which samplers are used, and the texture type each one
  declares. ps 1.x declares no type, so its samplers show `any` and the shader
  picks by `SamplerStateBlock::textureType`.
- **Vertex attributes:** each one, with its D3DDECLUSAGE and index, so a
  vertex declaration maps to attribute locations.
- **Inputs and outputs:** every input and output with its location and
  semantic.

## Float rules

D3D9 hardware gives 0 for 0 times anything, infinity and NaN included, and
Vulkan does not. dxbc-spirv offers two ways to match it, and the translator
uses the one DXVK picks on drivers without a native multiply of that kind
(Intel's and NVIDIA's):

- **Fast** (the default): plain multiplies, with infinities kept out instead.
  `rcp`, `rsq`, `exp`, `expp` and `nrm` are clamped to the largest float and
  `log` and `logp` to the lowest, and `pow` with a zero exponent gives 1, so a
  product never meets an infinity (dxbc-spirv's `fastFloatEmulation`).
- **Strict** (`KISAK_VK_STRICT_FLOATS=1`): every multiply, multiply-add and dot
  product checks both factors for zero, two compares and two selects per
  component (dxbc-spirv's `*_legacy` functions). Up to 2026-11-11 this was the
  only rule. Laptop run 17 had Mesa's Intel compiler print its statistics for
  every shader of a mp_crash scene: our vertex shaders were about 3.5 times as
  long as DXVK's (median 545 against 154 instructions) and our pixel shaders
  1.8 times (635 against 359). With the fast rule (laptop run 18) the vertex
  shaders are as long as DXVK's (151) and the pixel shaders 1.3 times (457),
  and the laptop draws the scene at 129 fps against 106, with a picture that
  cannot be told from DXVK's by its numbers.

The rule is set once per process (`legacy_shader::SetStrictFloats`, from the
core's `OptionalFeatures::strictFloats`), before the first shader is
translated. `TranslatorRevision` carries it, so a shader cache file made under
the other rule is not used. The adapter's `device ready:` line says
`floats fast` or `floats strict`, and `KisakCOD-vulkan-shaders` follows the
same variable and prints `legacy_shader.floats=`.

## Legacy state

dxbc-spirv turns the D3D9 state a shader depends on into uniform blocks read
at run time, where DXVK specialises it per pipeline. The Legacy adapter always
writes the same values for the parts the game never changes (see
`FillUniformBlock` in `vk_legacy_draw.cpp`), so `Translate` makes them
constants after legalising and folds what they decide
(`FixLegacyState` in `vk_legacy_shader.cpp`):

| Block | Fixed as | What goes |
| --- | --- | --- |
| Fog | off | ps 1.x and 2.x: the fog branch and its function |
| Clip planes | none | vertex shaders: six dot products and the block (the clip distances are written as 0) |
| Point sprites | not a sprite; size 1, 1 to 64 | pixel shaders: a select between each texture coordinate and `PointCoord`; vertex shaders: the size's clamp is constant |
| Sampler state | no projection, no gather, never unbound, D24 depth scale | around each sample: the unbound branch, the gather path and the projection select |

An unbound sampler reads the adapter's null texture, which is (0, 0, 0, 1) as
D3D9 gives, so the unbound branch is not needed. What stays in the sampler
state block is the texture type (shader model 1 only), the depth compare
(unless fixed per pipeline, as on MoltenVK) and the depth clamp. Blocks with
nothing left to read are not declared, so the adapter binds and writes fewer
of them. Over the spike cases the shaders have 22% fewer SPIR-V instructions.
On the laptop (run 19) holding the legacy state as constants is worth 15 fps
(158 to 173) and the alpha test fixed per pipeline 33 (140 to 173); with both,
Mesa's Intel compiler makes our pixel shaders shorter than DXVK's (median 232
against 359 instructions).

`KISAK_VK_LEGACY_STATE=dynamic` keeps all of it as dxbc-spirv emits it. Like
the float rule it is set once per process (`legacy_shader::SetDynamicLegacyState`,
from the core's `OptionalFeatures::dynamicLegacyState`), `TranslatorRevision`
carries it, the adapter's `device ready:` line says `legacy state fixed` or
`dynamic`, and `KisakCOD-vulkan-shaders` follows the same variable and prints
`legacy_shader.legacy_state=`.

`Translate` also removes unused calls of functions without side effects:
dxbc-spirv's `RemoveUnusedPass` keeps a call of a function it has not met yet
in code order, and the vertex shaders' clipping function comes before the
dot product it calls.

## Known limits

- **Predication.** `setp`, predicated instructions, and `if`/`break` on p0
  are refused with a reason. dxbc-spirv at the pinned commit gives invalid
  SPIR-V for them, or aborts on `breakp`. The game never uses them (S0 corpus
  scan).
- **Binding model.** It is simple on purpose. DXVK puts render state into push
  data and specialisation constants and packs constants. Here each block is a
  plain uniform buffer, and `BuildSpirv` fixes two states per pipeline by
  replacing their loads with constants and folding what they decide: depth
  compare (on MoltenVK) and the alpha test's compare. `Translate` fixes the
  state the game never changes ([Legacy state](#legacy-state)). Most pixel
  shaders read two to three blocks per draw (constants, alpha test, plus
  sampler state when they sample), most vertex shaders one. If the game ever
  sets fog, clip planes or point sprites, they need
  `KISAK_VK_LEGACY_STATE=dynamic` or a pipeline key bit, as the alpha test has.

## Pin

dxbc-spirv `bf14419e5fa7eacb817b7b632f03cb61d61bbad7`, the commit DXVK 3.1.1
ships as `subprojects/dxbc-spirv`, with its SPIR-V headers submodule at
`c8ad050fcb29e42a2f57d9f59e97488f465c436d`. CMake fetches both with git at
configure time and refuses any other commit. Only the SM1-3 front end, the IR
passes and the SPIR-V back end are built; the SM4/5 front end is left out.

On a machine without network access, point
`FETCHCONTENT_SOURCE_DIR_KISAK_DXBC_SPIRV` at a checkout of that commit with its
submodule. A DXVK 3.1.1 tree's `subprojects/dxbc-spirv` qualifies.

## Building and running

```sh
cmake -S . -B build-vk -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=x86_64 \
    -DKISAK_BUILD_VULKAN_SHADERS=ON
cmake --build build-vk --target KisakCOD-vulkan-shaders
# One shader: reflection, and with --vertex a link check
KisakCOD-vulkan-shaders translate shader.ps --vertex shader.vs --out shader.spv [--ir]
# A folder (for example shader_corpus.py --dump-dir): counts only
KisakCOD-vulkan-shaders batch ~/s0/bytecode --out-dir ~/s0/spirv --threads 4
```

On Windows, `scripts/vulkan_probe/CMakeLists.txt` builds it with MSVC when
given `-DKISAK_BUILD_VULKAN_SHADERS=ON`. It needs no Vulkan SDK; the translator
uses no Vulkan headers.

## Checks

`scripts/tests/test_vulkan_legacy_shaders.py --binary <KisakCOD-vulkan-shaders>`
needs no GPU, game data or network. It covers:

- **Spike cases:** the shader spike's 81 op-by-op shaders (`scripts/tools/shader_spike/cases.py`).
  76 translate to SPIR-V that `spirv-val` accepts for Vulkan 1.3 with scalar
  block layout, and the 5 predicated ones are refused with a reason.
- **Layout:** every uniform block, texture and sampler sits at its binding
  with the header's offsets and strides, and constant arrays are as long as
  the reflection says.
- **Reflection:** constants, samplers, render targets and vertex attributes
  of chosen cases.
- **Shader model 1.1:** a hand-assembled vs_1_1 and ps_1_1 translate and link.
- **Linking:** a pixel shader linked to a vertex shader that writes its input
  at another location reads it from there. An input the vertex shader does not
  write is dropped. fxc-style texcoord pairs link without changes.
- **Fixed depth compare:** every case with samplers is built again with
  `--depth-samplers 0` and `0xffff`, the build MoltenVK gets
  ([vulkan-legacy-adapter.md](vulkan-legacy-adapter.md#moltenvk-macos)). Both
  pass `spirv-val`. With `0` no depth-reference sample is left. With `0xffff`,
  2D and cube images are sampled only with a depth reference.
- **Fixed alpha test:** every pixel shader case that reads the alpha test
  block is built again with `--alpha-compare 7` (ALWAYS) and `4` (GREATER).
  Both pass `spirv-val`. With ALWAYS the test's discard is gone (a `texkill`
  keeps its own) and the block is not read; with GREATER the test stays.
- **Legacy state:** every case is translated again with
  `KISAK_VK_LEGACY_STATE=dynamic` and passes `spirv-val` and the layout check
  that way too (the fog, clip plane and point blocks are checked there). By
  default no case declares the fog, clip plane or point blocks and none
  gathers; with the switch every vertex shader declares the clip planes,
  every ps 2.x one the fog, and some cases gather. The test prints the
  instructions summed over the cases both ways.
- **Float rules:** a case with matrix multiplies uses dxbc-spirv's `*_legacy`
  functions with `KISAK_VK_STRICT_FLOATS=1` and none without, and the tool says
  which rule it used.

Hosted CI runs this test in two places:

- The Linux `vulkan_window` job, x86_64 Debug and RelWithDebInfo, with spirv-val.
- The Windows `vulkan-probe` job, MSVC x64, without spirv-val (the runner
  has none).

`shader_corpus.py --dump-dir` now also dumps the 37 shader model 1.1 programs,
so a `batch` run over the dump covers every shader the game has.

## Results

**Cloud sandbox** (no GPU, x86_64 Debug and i386 RelWithDebInfo, clang 18):

- The test passes.
- Both builds give byte-identical SPIR-V for all 76 cases.
- Translation averages about 5 ms per case shader in a Debug build.

**Laptop run 1** (2026-11-08, ee6d019, David's laptop, x86_64 and i386
RelWithDebInfo, dxbc-spirv from the DXVK 3.1.1 checkout with no fetch):

- Both builds compile with 0 warnings, and the test passes on both.
- Every shader the game has translates: 936 of 936 (ps_1_1 7, ps_2_0 255,
  ps_3_0 468, vs_1_1 30, vs_2_0 124, vs_3_0 52).
- spirv-val (Vulkan 1.3, scalar block layout) accepts all 936 on both builds.
- The two builds' SPIR-V is byte-identical for all 936.
- Translation averages 7.1 ms per shader on x86_64 and 10.8 ms on i386.

What the game's shaders use:

- Float constants only. No int or bool constants, and no shader indexes c[].
  The most read is 32 registers in a vertex shader and 22 in a pixel shader.
- No texture-stage (bump-mapping) state.
- Samplers declare 2D 2682 times, 3D 332 times and cube 218 times. 4 ps 1.x
  samplers have no declared type.

Shaders executed on a GPU are covered by the layers above: the
[Legacy adapter](vulkan-legacy-adapter.md)'s pixel check, and the game drawn
with `r_backend vulkan` on an Intel and an NVIDIA GPU
([vulkan-d3d9-front.md](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan)).
