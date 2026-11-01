# Renderer world front end on native Linux

The static map view milestone needs the engine's own renderer to decide what part
of a map is drawn, batch it, and hand the batches to the D3D9 back end. This
guide covers the half of that which needs no device: the world front end. It is
proven on i386 Linux by an asset-free CPU test that runs the unmodified
production code. Nothing here creates a device, loads a map or draws a pixel.

## What runs

`tests/linux_world_frontend/world_frontend_test.cpp` drives, in order, the same
functions `R_GenerateSortedDrawSurfs` calls for the world:

| Stage | Production code |
| --- | --- |
| scene buffers and DPVS state | `R_InitSceneBuffers`, `R_InitSceneData`, `R_ClearDpvsScene` (`r_dpvs.cpp`) |
| camera cell | `R_CellForPoint` over the uint16 BSP node stream (`r_dpvs.cpp`) |
| world visibility | `R_SetupWorldSurfacesDpvs`, `R_AddWorldSurfacesDpvs`: portal walk, frustum and portal clipping, per-cell AABB trees, cull groups, the no-decal ranges (`r_dpvs.cpp`, `r_dpvs_static.cpp`) |
| per-cell worker commands | `R_AddWorkerCmd` running inline with `r_smp_worker` off (`r_workercmds.cpp`), including the dynamic-model, scene-entity and dynamic-brush cell commands |
| BSP batching | `R_AddAllBspDrawSurfacesCamera` and the emissive range (`r_add_bsp.cpp`, `r_add_cmdbuf.cpp`) |
| merge and emit | `R_MergeAndEmitDrawSurfLists` (`r_pretess.cpp`) |

The world is two cells joined by one portal window, ten surfaces in four
materials (lit, decal, emissive), one cull group and a one-plane BSP. Each
expected result is derived from the geometry in the test's comments, not read
back from the code: which cells the portal walk reaches, which surfaces survive
the near plane, the far plane and the portal planes, and the exact draw-surface
batches per stage, including one material batch spanning both cells. Further
views cover a far plane that stops the walk at the portal, the reverse walk from
cell 1, the no-camera-cell path, `r_skipPvs` and `r_drawDecals`.

Four deliberate breakages of the world were each caught when this was written:
moving the portal window out of view (5 failures), giving one surface a different
material (4), flipping the portal plane (4) and counting the decal as a
non-decal surface (1).

## How it links

`scripts/tests/run_world_frontend_checks.py` compiles the test, its boundary file
and 40 unmodified production units with the client definitions and the flags of
the native Linux targets, links with `--gc-sections`, and runs the ELF32 result.
The production list is the exact closure: the runner's self-test fails if a
listed unit is missing or listed twice, and each unit was confirmed necessary
when the list was written.

`tests/linux_world_frontend/frontend_link_boundary.cpp` defines everything else
those units reach, in exactly three kinds:

1. **Host services**: console output, and an assertion handler and `Com_Error`
   that abort the run. `RELEASE_ASSERTS` keeps production assertions live in
   both configurations.
2. **Storage** whose owner cannot be linked yet (`dx`, `rgp`, `gfxCfg`, `rg`,
   `scene`, `gfxBuf` and a few dvar pointers), each with the owner's exact type,
   zero-initialized as the owner's definition is before its init runs. Where the
   owner links cleanly it is linked instead: the collision map `cm` comes from
   `cm_load.cpp` and the dynamic-entity accessors from `DynEntity_load_obj.cpp`.
3. **Traps**: functions the synthetic world must never reach (entities, effects,
   model lighting, skinning, shadows, debug drawing, fences, localization). Each
   one aborts with its name, and each signature is checked against the
   production header. Every trap was confirmed necessary for the link.

Two pieces of orchestration are mirrored from `r_scene.cpp` and marked in the
test: three stages of `R_InitScene`'s stage table and the four calls of
`R_SetViewParmsForScene`. `r_scene.cpp` needs the D3D9 query interface, so it
cannot be linked with the type-only stand-in. With the native D3D9 boundary it
compiles (see below), and the mirrors should then be replaced by the real file.

## What a pass does not claim

- No device, texture, shader, draw call or pixel is involved.
- The pre-tessellated index path (`r_pretess`) writes GPU index buffers and is
  switched off; static models, entities, effects and shadows are absent.
- A synthetic world is not a real map. Real BSP data exercises deeper trees,
  many portals, solid leaves and static models that this world does not have.
- Local results are not hosted CI evidence. The `world_frontend` workflow job
  runs the runner in Debug and RelWithDebInfo on hosted runners; both passed
  there when the job was added (2026-11-01).

## Integration with the native D3D9 boundary

The boundary ([linux-native-d3d9.md](linux-native-d3d9.md)) proves 21 renderer
units compile through DXVK Native's real `d3d9.h`, `r_scene.cpp` and
`rb_backend.cpp` among them. Checked locally against the pinned DXVK 2.7.1
headers with the engine's own dialect (`-std=gnu++20`, which is what CMake gives
the engine targets), the renderer units that matter for a map view stand as
follows:

| Unit | Through the boundary |
| --- | --- |
| `r_scene.cpp`, `rb_backend.cpp` | compile unmodified and are on the boundary's proof list. They need `-std=gnu++20`: under strict `-std=c++20` the `linux` macro is not predefined and `deps/binklib/radbase.h` fails platform detection |
| `r_init.cpp` | `HMONITOR`, `WS_*` window styles: window ownership, which the SDL host replaces |
| `r_screenshot.cpp` | `MONITORINFO`, `ClientToScreen` |
| `rb_sky.cpp` | `Sleep` |
| `r_material_load_obj.cpp` | D3DX (`ID3DXFile`, `TEXTMETRICA`) |
| `r_cinematic.cpp` | Windows-only Bink texture helpers |
| `r_texturemem.cpp` | `<ddraw.h>` |

The client census (`run_client_compile_census.py`) compiles with the stand-in,
so its 25 D3D9 entries stay blocked even with the boundary on master; a second
census mode with the boundary is the way to ratchet those.

## Order of work toward one static map view

1. Link `r_scene.cpp` through the boundary into this test and delete the two
   mirrors, so `R_RenderScene`'s front half is the production file end to end.
2. Feed the front end a real `GfxWorld`: load one map's world from an explicit
   private asset path (never committed, never searched for) and check
   invariants that hold for any map: every visible surface index is in range,
   batches are sorted by key and the camera cell is valid, so real BSP data runs
   through the same code.
3. Device-side: create the device through the boundary from the SDL window host,
   run `R_InitGraphicsApi`-level resource creation for the world's vertex and
   index buffers, images and the techniques the world's materials use; refuse
   any technique or vertex declaration not yet supported, by name.
4. Draw one fixed view through `RB_Draw3D`, `R_DrawLit` and `R_DrawBspDrawSurfs`
   and present it, behind the free-camera frame owner's `SceneRenderer`
   interface (`LoadMap`, `DrawFrame` returning false is terminal). The adapter
   zeroes its `refdef_s` and uses local client 0.
5. GPU evidence on a real machine, accepted the way the standalone lit and unlit
   replays are (selected pixels against independent predictions, plus controls),
   stated per device and map.

Steps 2 to 5 need DXVK Native, a GPU and licensed assets, so they run on a
developer machine, not hosted CI.
