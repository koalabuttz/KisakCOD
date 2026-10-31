# CPU UI sampling and ordinary window resizing

The admitted metadata sampler235 (`0xEB`) selects the engine's two-degree
anisotropic filter, point mip selection and clamped texture addressing under
its default texture settings. `R_SetTexFilter` can change that behavior according
to hardware capabilities and texture dvars. Direct3D describes anisotropy as the
ratio between the major and minor axes of a pixel's texture-space footprint;
it does not prescribe exact sample positions for all drivers. See Microsoft's
[Direct3D9 anisotropic filtering documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/anisotropic-texture-filtering).

The native CPU backend now defines a bounded two-tap approximation for its
axis-aligned UI image/text path. This is an explicit supported CPU quality
contract, not a claim of GPU-driver pixel equivalence. Other shader/sampler
profiles still require their existing metadata admission. No material or font
is dropped when a nonuniformly scaled window produces different texture
footprints along its axes.

For positive draw dimensions, let `dx` and `dy` be the absolute UV span times
the base texture dimension divided by the physical draw dimension:

- If `max(dx,dy) <= 1`, retain the existing base-level bilinear result.
- For the existing isotropic tolerance (`abs(dx-dy) <= 1e-5 * max(dx,dy)`),
  retain the original single tap and nearest mip selected from `log2(dx)`.
- Otherwise set `major = max(dx,dy)` and
  `minor = max(1, min(dx,dy), major/2)`. Select the nearest stored mip to
  `log2(minor)`, clamped to the final level.
- Sample twice along the major axis, at the original UV center plus/minus
  `(major-minor)/2` base texels. Normalize that offset by the corresponding
  base dimension. Clamp and bilinearly filter each tap independently.
- Average unassociated RGBA, then apply the original packed tint and single
  source-alpha/inverse-source-alpha blend. Do not blend the taps separately.

Offsets approach zero as the footprint becomes isotropic. For ratios above two,
the selected footprint becomes coarser rather than increasing the number of
taps without bound. This retains point-mip transitions; no trilinear smoothing
or unregistered texture settings are implied. Stored mip levels are used, never
synthesized from the base image.

Every minifying metadata-backed draw still requires a complete owned mip chain.
Whole-list preflight validates the sampling plan before any preceding clear or
draw changes pixels. Finite geometry/UV limits, resource-generation checks and
unsupported-command rejection are unchanged. Text preflight charges both taps
to the existing work budget, including shadow passes.

Regression coverage includes independently calculated colored-mip samples in
both major-axis directions, flipped UVs, clipping, independent edge clamping,
large ratios, nearest-mip boundaries, the old isotropic tolerance, near-isotropic
continuity, RGBA/tint blending, missing-mip rejection and atomic text-budget
failure. Genuine menu resize/readback tests provide separate asset-backed proof.
