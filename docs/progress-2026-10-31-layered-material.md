# Shared layered-material storage correction

`Material_LoadLayered` uses `Material_CreateLayered` for combined materials in the
shared renderer. The latter allocated an 80-byte header even when the actual
editor material is 96 bytes. It also accumulated texture and constant counts in
bytes: totals above 255 wrapped before allocation, potentially allocating smaller
tables than the subsequent copy loops required. The count defect affects MP/SP
as well as editor builds; this is shared resource correctness, not an editor feature.

The actual creation function is now compiled from `r_material_layered.cpp`, with
shader/state derivation helpers remaining in the original loader. It uses actual
structure sizes for the header, tables and name, accumulates counts before narrowing,
includes generated tint constants in its limit, and rejects invalid input or
oversized storage with a diagnostic and null result. Valid zero-texture materials
avoid null-pointer subtraction and sorting. Ordinary hashing, sampler adjustment,
tint generation, metadata and validation behavior are preserved.

The baseline fixture reproduced two count-rejection failures in both MP and SP,
and six failures including header/table overlap in Radiant. It intercepts unsafe
baseline allocations instead of deliberately overflowing memory. Expanded final
fixtures run the production function with real engine types and pass 19 checks
for MP, 19 for SP and 20 for Radiant. They cover layout, nonzero table contents,
empty textures, the 255-entry boundary, synthesized tint overflow, malformed batches,
missing storage and allocator failures. Independent review found no blocking issue.

These fixtures supply explicit shader/state-helper doubles and an aligned arena;
they establish construction/storage behavior, not real shader validation, GPU
execution or an editor application run. Arena rollback policy is unchanged.

Reproduction and final evidence are in
`/workspace/kisakcod-client-ui-results/layered/`. The optional Linux window checker
also runs all three variants in Debug and RelWithDebInfo. Common and explicit
Radiant source lists both include the new production file. Packet handling and
authentication are untouched.
