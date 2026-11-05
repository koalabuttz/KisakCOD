# Packaged world-surface replay — 2026-11-01

> Initial one-surface checkpoint. Later independent pixel, multi-triangle and lit
> validation supersedes the next-gate section below; see
> [renderer validation](linux-renderer-validation.md) and the
> [current status](status.md). Counts here describe this initial run.

## Bounded result

A native ELF32 DXVK Native 2.7.1/SDL3 replay executed one original packaged
vertex/pixel shader pair and one indexed BSP world surface from `mp_crash`.
The parent rebuilt and ran the final guarded implementation on Intel RPL-U,
Mesa ANV 25.0.7, native Wayland. All nine image/lifecycle checks passed:

- 562 of 65,536 pixels differ from the magenta sentinel, with 105 distinct colors;
- moving the declared camera/constants changes 509 pixels;
- omitting the draw and looking away both leave the target exactly clear;
- replacing the texture changes all 562 covered pixels;
- after unbinding/releasing default-pool objects, real device Reset, recreation
  and explicit rebinding, zero pixels differ from the initial image.

The parent inspected the output image: a small textured triangle on the sentinel
background. This is a standalone **unlit surface** test. It is not a captured game
frame, original lighting, engine integration, image parity, full scene rendering
or gameplay. A readback verifies rendered pixels, not compositor presentation.

## Inputs and provenance

The offline opt-in export tool loaded the map through the existing database
without starting networking, gameplay or a renderer. Inventory examined 5,558
surfaces; 5,180 met the static candidate gate and 378 multipass candidates were
rejected. Selection 249 uses material `wc/me_concrete_dest1`, unlit `vcsh`, shaders
`vertcol_simple_fog.hlsl` (146 DWORDs) and `vertcol_shaded_fog.hlsl` (95 DWORDs).
Raw 44-byte vertices and surface-local indices are copied unchanged; the three
referenced indices are 2836, 2837 and 2838 within the 6,491-vertex slice.

The declaration is resolved from the recorded routing through transcribed engine
WORLD/source/destination tables, explicitly not a runtime table-parity claim.
The bundle records arguments, material state bits and unresolved dependencies.
It remains `render_ready:false`; a complete live scene was never captured.

The private preparer preserves payload bytes, checks bounded shader framing,
extracts genuine IWI dependencies from the authorized installation and records
archive/entry and SHA-256 provenance. The parent ran all 41 synthetic preparation
tests and reverified the prepared output before final replay. The manifest digest
was `5e0b2a216defcea0106939b0e1a9da9d4a28e5b6e6f42e39b093d78276d85726`.
Shader SHA-256 values:

- VS: `67347d94a01def7f120fbe52dd3cf46cb638ad440093e0be05482d3f7ca7ea5e`
- PS: `3e557844d0bb4f2b5d56ea25c1dfb69437ea2f971098672dc06d9b641fa47af7`

The active colormap `images/me_concrete_dest02_col.iwi` was found once, in
`main/iw_02.iwd`, 87,428 bytes, SHA-256
`96cbb71650176d682bc6501600075948f279362938447ece5c9f04ad3425765f`.
The replay uses the existing IWI decoder and uploads its full ten-level mip chain.
A separate engine-compiled decoder object keeps engine compatibility headers out
of the DXVK-native translation unit.

## Explicit test context

Camera placement is derived from the referenced triangle and recorded winding,
not from observing whether a trial draw passed. Matrix/fog payloads use audited
equivalents of the engine's formulas. Parent 32-bit numeric tests pass 221 checks;
these tests do not establish bit-exact production or image parity. Fog is disabled
with an explicitly declared prior fog color, and sampler inputs explicitly select
`r_texFilterDisable=1`. These are test settings, not captured game settings.

Material code 40 is required on the command line. The parent decoded the actual
pixel-shader tokens: material alpha weights replacement of textured vertex color
with material RGB. Thus `(1,1,1,1)` replaces the texture with white and failed the
texture control; `(1,1,1,0)` preserves the texture term. Existing editor world-draw
code also supplies alpha-zero contexts. The final run declares `(1,1,1,0)`; it is
not an inferred default or a recovered game-frame value.

## Validation and important failed attempts

Parent verification includes 46 asset-free bundle-loader checks, state/sampler
self-tests, 221 numeric-context checks, 41 Python tests, verified private assets,
and the final guarded GPU run. Review exposed and corrected:

- declaration-source indices confused with stream IDs;
- incorrect camera handedness shared by the original numeric fixtures;
- non-finite intermediate payloads for extremely small FOV tangents;
- 32-bit geometry-size overflow and excessive constant-row host reads;
- incorrect cull and mip/anisotropy decoding;
- inconsistent shader COMMENT/instruction framing;
- preparation reads before trusted caps, missing aggregate texture budget, and
  verification that checked hashes without repeating structural checks;
- an object-recreation check that did not actually reset the device.

The initial zero/white-output and failed texture-control runs are failures, not
accepted evidence. Reset initially failed when RT0 was set to null; the repaired
path restores the implicit back buffer before releasing the test targets.

The replay accepts only the measured constant-route shapes. A fixed FNV pair
allowlist catches accidental shader substitution before creation; it is not a
cryptographic security boundary. SHA-256 provenance must be checked separately
with the preparer. Framing is not full opcode/operand validation, and this is not
a safe general-purpose runner for hostile shader inputs.

## Reproduction outline

Build the opt-in exporter with isolated runtime outputs. Use an absolute new
private output directory and a separate `fs_homepath`, with installed assets read
only. `KISAK_WORLD_SURFACE_MAP=mp_crash` inventories;
`KISAK_WORLD_SURFACE_INDEX=249` additionally writes the static bundle.

```sh
python3 scripts/tools/prepare_linux_world_surface.py \
  --bundle /private/export/world-surface-bundle.json \
  --game '/authorized/Call of Duty 4' --out /private/new-prepared
python3 scripts/tools/prepare_linux_world_surface.py --verify-only /private/new-prepared

# Native renderer build dependencies and flags: tests/linux_renderer/README.md.
# Enable KDXK_WORLD_SURFACE_REPLAY and build world_surface_replay,
# world_surface_bundle_test. Runtime libraries must match the i386 ABI.
world_surface_replay --bundle /private/new-prepared/export.json \
  --texture /private/new-prepared/images/me_concrete_dest02_col.iwi \
  --tex-filter-disable 1 --material-color 1,1,1,0 \
  --output /private/render.ppm
```

No licensed payload, generated image, game binary or build output belongs in Git.
The next rendering gate should increase geometric/image coverage and establish an
independent pixel oracle, then address a genuinely lit pass and complete resource
closure. The original rendering path remains the reference; enhanced visuals are
not part of this experiment.
