# Renderer validation follow-up — 2026-11-01

This follows the original [one-surface experiment](linux-world-surface-probe.md).
All results below are bounded standalone tests, not a playable client, complete
renderer integration or original-frame image parity. Licensed payloads and images
remain in private temporary directories, never in Git.

## Asset-free coverage

The Linux workflow adds an independent world-surface job for Debug and
RelWithDebInfo. The runner compiles strict ELF32/i386 tests directly, without the
standalone renderer CMake project's DXVK/SDL dependencies. It runs inventory,
context math, bundle loader, pixel oracle, lit loader, image capture, and Python
preparation tests. Timeouts, nonzero exits and explicit FAIL output fail the job;
logs are retained. Both local configurations passed all seven suites. Workflow
contract tests passed 17 cases, runner tests 18. Hosted execution of this added
job has not yet been observed.

```sh
python3 scripts/tests/run_world_surface_checks.py --config Debug
python3 scripts/tests/run_world_surface_checks.py --config RelWithDebInfo
```

## Independent bounded unlit pixel predictions

The actual packaged 146/95-DWORD pair was audited instruction-by-instruction.
The CPU evaluator is specific to those programs and gated to their measured
identities; it is not a general shader interpreter. It consumes the original
geometry and the same CPU-decoded texture bytes uploaded to the GPU. Thus it
independently tests transforms, interpolation, upload/channel order, addressing
and shader arithmetic, but does not independently validate the shared IWI decoder.

For declared white vertex colors, disabled fog and material replacement alpha
zero, the shader output reduces to the sampled texture RGBA. Samples are chosen
from geometry and UVs before GPU creation/readback, with fixed interior/texel
boundary margins, spatial distribution and tolerance of one byte per channel.
They are not selected by observed agreement. Altered UV/channel expectations
must fail the same comparison against the baseline readback at unchanged sample
coordinates; a one-byte change within tolerance is explicitly not a rejection.
Those are CPU-expectation controls, not uploaded wrong-shader variants.

Parent execution confirmed all 16 original-surface predictions exact and all 15
moved-camera predictions exact. The later required moved-declaration check
preserves the original image and sample list. The fixed integer D3D9 sample-point
convention is a current test contract; old exploratory convention-measurement
output was not retained and is not claimed as a current exhaustive test.

## Genuine larger geometry and depth

`KISAK_WORLD_SURFACE_MIN_TRIANGLES` adds an optional bounded inventory ranking
floor (2..256); exact-index export remains explicit and reports a floor bypass.
Parent exported `mp_crash` surface 415, material `wc/me_brick_01`, with the same
unlit shader pair: 16 triangles, 48 unchanged indices, 945 unchanged vertices,
24 referenced vertices across four planes. A material's unused texture-table
entries are not extra live samplers.

The initial proposed oblique camera was completely culled by both oracle and
GPU; that run failed and was not reclassified. The accepted explicit camera is
`--camera-eye=-800,2680,200 --camera-target=-1536,2680,192 --tan-half-fov 0.8117`.
It retains 12 triangles and culls four, so no all-plane visibility claim is made.
Parent rebuilt and reproduced the final 19-check run:

- 14 baseline and 15 moved-camera predictions match exactly;
- split and reversed submission with depth enabled reproduce the baseline;
- CPU predicts 73 pixels of order-dependent color contrast (74 owner changes);
  forward versus reversed depth-disabled frames differ on 119 pixels;
- skip-draw/offscreen and texture/constant controls hold; reset reproduces output.

The order control compares the correct two depth-disabled frames and requires
predicted color contrast, not just different owners. Moved predictions are
required: a bounded fixed list is evaluated on CPU before drawing, and no valid
prediction is a failure rather than a skip. Eye-straddling triangles are refused
because clipping is unsupported. Positive depth-gap checks reject ambiguous
samples. Asset-free tests include near-first/equal-color overlap, sloped depth
where affine and perspective depth disagree, and mixed valid/straddling geometry.
Targeted review closed those acceptance findings.

## Lit work: export is not rendering acceptance

A separate technique-7 inventory never falls back to unlit. Sky passes are not
counted as lighting proof. Surface 415's lit pass requests a reflection probe and
is refused by the minimal lightmap-only exporter; its successful unlit replay
does not establish lit compatibility.

The export-only Load_Texture hook captures supported lightmap bytes before the
dedicated loader discards the pointer. It is absent from normal dedicated builds.
Capture is armed before the map-zone load, bounded, synchronized, allocation/I/O-
free in the loader callback, and checks exact supported 2D layouts. Ambiguous
same-name offers poison lookup even if one offer is rejected; reused pointers do
not establish identity. Unsupported non-power-of-two mip chains fail closed.
Per-surface availability considers only requested resources.

Parent successfully exported actual surface 0's `lm_r0c0_sm3` technique-7 pass:
VS `lm_tc0_sm3.hlsl` (179 DWORDs), PS `lm_r0c0_sm3.hlsl` (148 DWORDs), four-element
WORLD declaration including raw packed normal, and the actual secondary lightmap
at sampler 3. Captured lightmap: ARGB8, 512x2048, one level, 4,194,304 bytes.
Its shader samples both vertical halves and derives a weight from their alpha
channels; it is not a guessed RGB multiplier. No material-color code40 is used.

The first lit implementation's PASS was not accepted: review found admission,
bounds, hardware, control and pixel-coordinate defects. The subsequent 16-sample
run executed on Intel and matched sampled values, but review showed a half-pixel
projection error could be hidden by magnified point sampling. Its density bound
was also only a heuristic. Those results are recorded as diagnostic history,
not the final same-pixel oracle acceptance.

The final corrected implementation removes the projection offset entirely and
uses ordinary viewport mapping with integer sample positions. The density-based
camera adjustment and its guarantee are removed; the fixed fit camera derives
from referenced geometry. Directional texel travel is reported only as a
diagnostic, not a margin guarantee. Shared analytic tests pin the viewport,
barycentrics, perspective interpolation, directional derivatives and boundaries.

Parent rebuilt and reproduced the native Intel/Wayland run with 16 predeclared
samples, fixed tolerance one, and worst channel delta zero. All 28 checks passed:
base, skip-draw, base-texture perturbation, lightmap perturbation, corresponding
CPU predictions and alpha checks execute before and after a real device Reset.
The perturbed inputs are diagnostic controls, not replacement assets in the
baseline. The captured lightmap and genuine base IWI remain the baseline inputs.

The lit loader/analytic suite now passes 38 checks. The final seven-suite
asset-free matrix passes locally in both configurations, with 17 workflow and
18 runner tests also passing. These results do not establish full-surface,
other-driver or original-frame parity; texel-boundary travel remains reported as
a limitation. The earlier one-sample/tolerance-two and shifted-coordinate runs
remain rejected acceptance attempts, not retroactive successes.
