# Renderer graphics interface (`src/gfx_d3d/rhi`)

Every Direct3D 9 call the renderer makes goes through one interface,
[`src/gfx_d3d/rhi/r_rhi.h`](../src/gfx_d3d/rhi/r_rhi.h). Its Direct3D 9
implementation is the `r_rhi_d3d9_*.cpp` files next to it, and no other file in
`src/gfx_d3d` calls a D3D9 method. This is step **M1a** of the renderer plan
(the mechanical half of the seam a Vulkan back end will plug into): the calls
moved, nothing else changed, so the D3D9 build (Windows, and DXVK Native on
Linux) renders exactly as before.

## What is in the interface

Two kinds of functions, both named in the engine's `R_HW_*` style:

| Kind | Examples | Where |
| --- | --- | --- |
| **Operations** the renderer already had as its own hardware layer, moved verbatim with their callers unchanged, plus the call blocks cut out of mixed functions | `R_HW_SetBlend`, `R_HW_SetSamplerState`, `R_HW_SetRenderTarget`, `R_ClearScreenInternal`, `R_HW_SetVertexShaderConstant`, `R_HW_DrawIndexedPrimitive`, `R_HW_SetIndices`, `R_HW_EnableScissor` | `r_rhi_d3d9_state.cpp` |
| **D3D9-shaped calls**: one typed forwarder per D3D9 interface method the renderer still calls directly, `object->Method(args)` becoming `R_HW_Method(object, args)` | `R_HW_CreateTexture`, `R_HW_LockRect`, `R_HW_Release`, `R_HW_Issue`, `R_HW_Present`, `R_HW_CreateDevice` | `r_rhi_d3d9_calls.cpp` |

The forwarders are declared only where the full D3D9 headers are visible
(Windows, and Linux units compiled through the
[native D3D9 boundary](linux-native-d3d9.md)); units built with the stand-in
D3D9 types never see them and could not have called D3D9 anyway.

The signatures still use D3D9 types. **M1b** (after the shader spike S0) turns
them into backend handles and folds the D3D9-shaped calls into the operations
the plan's `RhiBackend` lists (create/upload texture, map/unmap buffer, set
state bits, draw, present, read back). That is a design step; M1a only moved
code.

## What did not change

- **Behaviour.** Each moved function and call block is the same code; the
  forwarders pass their arguments through and return the D3D9 result. The
  `r_logFile` lines are the retail strings, so a log taken before and after
  this change is identical.
- **Error handling.** The decompiled `do { ... } while (alwaysfails)` blocks
  that log, call and `Com_Error` on failure stay where they were (or moved with
  their call, for the operations); the forwarders do not add checks.
- **Calling conventions and layouts.** D3D9 COM methods keep their own
  convention; no asset or VM struct changed. On Linux the forwarders take the
  engine's `HWND`, which converts to DXVK's window handle inside the forwarder
  exactly as it did at the direct call.

## The boundary check

`scripts/tests/check_rhi_boundary.py` scans every renderer source outside
`src/gfx_d3d/rhi/` for a D3D9 method called through a pointer (by name, against
the methods `d3d9.h` declares) and for the API's free functions
(`Direct3DCreate9`, `D3DPERF_*`), skipping comments and strings. The result must
match `scripts/tests/rhi_boundary_allowlist.txt` exactly (a two-way ratchet like
the client compile census): a new direct call fails, and so does an entry that
claims more calls than a file has.

The allowlist holds only calls on COM objects that are not the Direct3D 9 API
but share its method names: the D3DX shader compiler's buffers in
`r_material_load_obj.cpp`, the GDI+ JPEG writer's stream in `r_screenshot.cpp`
and DirectDraw's video-memory query in `r_texturemem.cpp`. The scan does not
preprocess, so it also sees Windows-only and Radiant-only code.

```sh
python3 scripts/tests/check_rhi_boundary.py          # the check
python3 scripts/tests/check_rhi_boundary.py --list   # every call it sees
python3 scripts/tests/test_rhi_boundary.py           # its self-tests, then the check
```

CI runs `test_rhi_boundary.py` in the Windows workflow's runtime regression
job.

## Adding renderer code

Call an existing `R_HW_*` function. If the renderer needs a D3D9 method that
has no forwarder yet, add it to `r_rhi.h` and `r_rhi_d3d9_calls.cpp` (or an
operation to `r_rhi_d3d9_state.cpp`) in the same change. A new file in
`src/gfx_d3d/rhi/` joins the same build lists as the others: the Windows lists
(`scripts/common_files.cmake`, `scripts/radiant/radiant_files.cmake`), the
native D3D9 proof list (`scripts/platform/linux/native_d3d9_renderer_sources.txt`,
which the Linux clients compile through the boundary), both compile census
block lists, and the native lists of the probes and tests that link renderer
units through the boundary (`device_probe.cmake`, `world_frontend_probe.cmake`,
`world_draw_probe.cmake`, `scripts/tests/run_world_frontend_checks.py`).

## Verification

M1's gate in the renderer plan is exact output: the laptop's world draw probe
and free-camera replay digests must match master in Debug and RelWithDebInfo,
and a Windows build on real D3D9 must look the same before and after. Hosted
CI compiles every build (Windows MSVC Win32/x64/ARM64, Linux i386, x86_64 and
aarch64 clients, probes and censuses) and runs the asset-free checks; it has no
GPU or game data, so the digest comparison is a laptop run.
