# Material cloning and final renderer checkpoint

The actual `Material_Duplicate` new-entry path allocated and copied a hardcoded
80-byte material prefix, then stored the name at byte 80. Radiant extends that
structure to 96 bytes: duplication discarded its editor fields and placed name
bytes over them. The existing-entry path already copied `sizeof(Material)`.

The function now lives in `r_material_clone.cpp`, which can be compiled and run
independently of D3D shader compilation. It uses the actual structure size and typed
table fields while retaining existing registration, name ownership, deep-copy and
resort behavior. Both the common renderer source list and Radiant's explicit list
include it. The latter also now includes `r_command_buffer.cpp`, fixing an omitted
source entry from the earlier command-producer extraction.

The regression links the production function with the actual engine types and an
isolated registry/aligned-arena fixture. Distinct nonzero table contents catch
missing copies; allocation guards and injected table-allocation failure check
memory boundaries and ensure incomplete clones are not published. This does not
add allocation rollback to the existing hunk policy: partial allocations still
belong to the arena until its eventual cleanup.

The unchanged baseline function passes ten MP checks but fails three of ten
Radiant checks: allocation size, name placement and retained editor fields. The
fixed production function passes all ten in MP, SP and Radiant. The Radiant probe
uses `KISAK_RADIANT` and `MAX_GENTITIES=2176`, without `KISAK_MP`. Both Debug and
RelWithDebInfo build and run the MP/Radiant fixtures through the standard window
checker. ELF32 execution uses the existing QEMU adapter, not native32 execution.

Final evidence is in `/workspace/kisakcod-client-render-primitives-results`:

- `material-clone-{baseline,fixed}-*.log`: baseline and fixed runtime checks.
- `core-{debug,release}-final.log`: 480/480 aggregate checks each.
- `window-{debug,release}-final.log`: 98/98 checks each, including the two clone
  fixtures, owned-image tests and upload-allocation fault coverage.
- `x11-{debug,release}-final.log`: actual uploaded image, exact boundaries and
  background sampled at three sizes, plus input and shutdown behavior.
- `rendercmds-radiant-only-compile.log`: actual render-command source compiles with
  the editor-only defines; the earlier `rendercmds-radiant-compile.log` used MP
  plus Radiant and should not be mistaken for that configuration.

This is a substantial three-part checkpoint: D3D header/layout portability, owned
one-to-one image drawing, and material cloning. The CPU image path rejects scaling
and external raw-pointer commands, retains generation-bound ownership and validates
the complete command list before replay. It does not provide materials/shaders,
geometry rendering, full `R_Init`/`CL_Frame`, audio startup or gameplay.

The remaining hardcoded 80-byte offsets in `Material_CreateLayered` need a separate
runtime-tested correction before claiming full Radiant material support. Full
Windows/editor application builds and GPU rendering remain unverified. Prior
combined server/client test evidence is preserved for its original snapshot.
Denied packet extraction/validation and authentication code remain untouched.
