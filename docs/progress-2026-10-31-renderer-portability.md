# Renderer header and layout portability

The Linux compiler can now compile the actual `r_rendercmds.cpp` translation unit
for both MP and Radiant without inventing a D3D SDK or defining COM interfaces.

The shared `r_utils.h` release wrapper previously invoked a method on an opaque
`IDirect3DSurface9` while parsing the header. Its existing signature and assertions
now delegate that method call to `R_ReleaseD3DSurface`, defined in the real D3D
backend's `r_utils.cpp`. Linux intentionally has no definition: attempting an
actual GPU release without a backend fails to link. No release is silently skipped.
The existing D3D call and calling convention are preserved.

Radiant's `Material` now explicitly requests eight-byte alignment, matching its
documented Windows layout. Linux x86 otherwise aligns the embedded 64-bit key to
four bytes and produced a 92-byte structure instead of the required 96 bytes.
Static assertions preserve the common pointer fields and every editor extension
offset, including `editorUsage` at 86. MP remains an 80-byte structure. The material
allocator uses `Hunk_Alloc`, which requests 32-byte alignment, sufficient for this
requirement. This does not change game asset formats or add editor runtime support.

Evidence in `/workspace/kisakcod-client-render-primitives-results`:

- `rendercmds-{mp,radiant}-compile.log`: actual 32-bit source compilation passed.
- `portability-compile-arguments.json`: compiler arguments used for the check.
- `unsupported-gpu-release-link.log`: expected undefined reference to the real
  release function, confirming that unavailable GPU work cannot report success.

Independent source review found no blocking issue. Full Windows/editor builds and
GPU execution are not verified. The pre-existing `Material_Duplicate` allocation
at `r_material_load_obj.cpp:4738` and layered material creation still contain
80-byte layout assumptions; these need a separate runtime-tested correction before
claiming complete Radiant resource support. A subsequent
[material-clone checkpoint](progress-2026-10-31-material-clone.md) repairs and
runtime-tests the duplication path; layered material creation remains outstanding.
The current CPU backend does not call those material paths.

Packet extraction/validation remains denied and untouched. This change is confined
to renderer headers, the genuine backend release boundary, and editor layout.
