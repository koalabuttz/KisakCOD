# Renderer world front end on native Linux

The static map view milestone needs the engine's own renderer to decide what part
of a map is drawn, batch it, and hand the batches to the D3D9 back end. This
guide covers the half of that which needs no device: the world front end. It is
proven on i386 Linux in two ways, both running the unmodified production code:
an asset-free CPU test over a synthetic world (below), and an
[offline probe](#a-real-map-the-offline-front-end-probe) that runs the same code
over a real map's world loaded from an explicit private game folder. Nothing here
creates a device or draws a pixel.

## What runs

`tests/linux_world_frontend/world_frontend_test.cpp` drives, in order, the same
functions `R_GenerateSortedDrawSurfs` calls for the world. It does not call
`R_GenerateSortedDrawSurfs` itself: that function also sets up model lighting,
fog, sun and shadow state and files entities into cells, which needs world data
(sun light, primary lights) the synthetic world does not have.

| Stage | Production code |
| --- | --- |
| scene buffers and DPVS state | `R_InitSceneBuffers`, `R_InitSceneData`, `R_ClearDpvsScene` (`r_dpvs.cpp`) |
| scene stages and view | `R_InitScene`, `R_SetViewParmsForScene` from a `refdef_s` (`r_scene.cpp`, through the native D3D9 boundary) |
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
and 46 unmodified production units with the client definitions and the flags of
the native Linux targets, links with `--gc-sections`, and runs the ELF32 result.
The production list is the exact closure: the runner's self-test fails if a
listed unit is missing or listed twice, and each unit was confirmed necessary
when it was added.

Two of those units call the D3D9 API: `r_scene.cpp` (the scene setup and the
`scene` storage) and `r_buffers.cpp` (the `gfxBuf` storage). They are compiled
through the [native D3D9 boundary](linux-native-d3d9.md), with
`KISAK_LINUX_NATIVE_D3D9` and the pinned DXVK Native 2.7.1 headers
(`--dxvk-source` or `--dxvk-include-dir`, as the boundary runner takes them); every
other unit keeps the type-only stand-in, the per-unit opt-in the boundary
documents. They are the first renderer units with D3D9 calls linked into an
executable on Linux. No DXVK library is linked: the D3D9 methods they call sit in
functions the test never reaches, and `--gc-sections` drops them. Both must be on
the boundary's proof list (`native_d3d9_renderer_sources.txt`), which the runner's
self-test checks.

`tests/linux_world_frontend/frontend_link_boundary.cpp` defines everything else
those units reach, in exactly three kinds:

1. **Host services**: console output, and an assertion handler and `Com_Error`
   that abort the run. `RELEASE_ASSERTS` keeps production assertions live in
   both configurations.
2. **Storage** whose owner cannot be linked yet (`dx`, `rgp`, `gfxCfg` from
   `r_init.cpp`, which does not compile on Linux yet; `rg`; a few dvar
   pointers), each with the owner's exact type, zero-initialized as the owner's
   definition is before its init runs. Where the owner links cleanly it is
   linked instead: `scene` comes from `r_scene.cpp`, `gfxBuf` from
   `r_buffers.cpp`, `gfxDrawMethod` from `r_draw_method.cpp`, the collision map
   `cm` from `cm_load.cpp`, and the dynamic-entity, XModel, XSurface and DObj
   accessors from their own units.
3. **Traps**: functions the synthetic world must never reach (entity posing and
   culling, effects, model lighting, skinning, shadows, debug drawing,
   localization). Each one aborts with its name, and each signature is checked
   against the production header. Every trap was confirmed necessary for the
   link.

## What a pass does not claim

- No device, texture, shader, draw call or pixel is involved. DXVK contributes
  headers only; no D3D9 method is called and no DXVK library is linked.
- The pre-tessellated index path (`r_pretess`) writes GPU index buffers and is
  switched off; static models, entities, effects and shadows are absent.
- A synthetic world is not a real map. Real BSP data exercises deeper trees,
  many portals, solid leaves and static models that this world does not have.
- Local results are not hosted CI evidence. The `world_frontend` workflow job
  runs the runner in Debug and RelWithDebInfo on hosted runners; both passed
  there when the job was added (2026-11-01).

## A real map: the offline front-end probe

`KisakCOD-world-frontend-probe` runs the front end over a genuine `GfxWorld`. It
is the offline asset exporter ([linux-world-surface-probe.md](linux-world-surface-probe.md)),
which already loads one allowlisted map through the engine's own database without
networking or a device, with the renderer front end linked in. It is opt-in
(`KISAK_BUILD_WORLD_FRONTEND_PROBE`, which needs `KISAK_BUILD_LINUX_NATIVE_D3D9`
and the DXVK Native headers) and reads game data only from the folder it is
given. CI builds it and runs its asset-free tests but never runs it: that needs a
game install, which CI neither has nor looks for.

### What runs

`src/linux/tools/world_frontend_probe.cpp`, after the exporter's dedicated
initialization (which latches `r_loadForRenderer` off, so zones keep geometry in
CPU memory):

1. `DB_LoadXAssets` for the map zone, the fast-file branch of the server's map
   load. Exactly one packaged `GFXWORLD` naming the map must be in the zone,
   found by enumeration so a default asset cannot stand in for it.
2. `R_SetWorldPtr_FastFile` (`r_bsp.cpp`), the lookup `R_LoadWorld` makes, must
   land on that asset. The rest of `R_LoadWorld` prepares lighting and device data
   the surface walk does not read, and is not run.
3. The first frame's sort from `R_BeginFrame`: `Material_Sort`, then
   `R_SortWorldSurfaces`, which writes every surface's draw-surface key into
   `dpvs.surfaceMaterials` and ends by filing each shadow-casting surface under
   the primary lights that reach it (`R_SetPrimaryLightShadowSurfaces`, culled
   against the lights of the zone's `COMWORLD`).
4. The scene setup the synthetic test uses, then one view per player spawn point
   in the map's entity string (`mp_*_spawn*` classes, eye 60 units above the
   origin, the entity's own angles, 65 degree horizontal field of view at 4:3)
   plus the `mp_global_intermission` camera. Each view runs `R_UpdateLodParms`,
   `R_CorrectLodScale`, `R_SetViewParmsForScene`, the DPVS walk from the camera
   cell, `R_AddAllBspDrawSurfacesCamera`, the emissive range and
   `R_MergeAndEmitDrawSurfLists` per stage.

### What every frame is held to

The rules hold for any map because they follow from the code, not from the data
(`src/linux/tools/world_frontend_invariants.h`):

- `R_CellForPoint` put the camera in a cell, and the walk marked it visible. A
  player spawn's eye must be in a cell; the intermission camera may hang outside
  the level, in which case the engine walks by frustum alone and the frame is
  held to the frustum-only rules below.
- A surface is visible only through a visible cell's AABB tree or cull groups.
- Each emitted batch holds visible surfaces of its own stage's surface range, and
  every surface's key matches the batch key outside the objectId bits.
- Every visible surface of a stage's range is emitted exactly once.
- The same view run twice gives the same frame, and the portal walk sees a subset
  of what the frustum-only walk (no camera cell) sees from the same eye. The
  frustum-only frame is held to the surface and batch rules too.

Not a rule: that emitted keys ascend. A single BSP stage is emitted in surface
order, so key order belongs to the map compiler; the probe reports descents as an
observation. A stage whose draw-surface list fills up fails the view, because a
full list may have dropped surfaces.

It fails closed before reading anything it cannot bound: the map name must be on
the exporter's allowlist; at most 1024 cells (the width of
`dpvsGlob.cellVisibleBits`); world model 0 must start at surface 0 and fit the
visibility and sort tables; every AABB node and cull group range must lie inside
`sortedSurfIndex`; the zone's `COMWORLD` must be loaded under the world's name
and hold at least as many primary lights as the world names; `r_loadForRenderer`
must be off and no device may exist; and the loader and the front end, which are
compiled with different definitions, must agree on the size of every struct they
hand each other (`world_frontend_layout.h`).

The report, `world-frontend-probe.json` in the private output directory (created
exclusively, mode 0600), holds counts and the map's own name only: no geometry,
material names or asset bytes. The process exits non-zero if any view breaks a
rule, and prints `world_frontend_problem` lines naming the view and the rule.

### How it links

`scripts/platform/linux/world_frontend_probe.cmake` builds the exporter's sources
(the `KisakCOD-dedi` list plus the export tool files) together with the synthetic
test's renderer closure, `r_bsp.cpp`, the light-culling owners the surface sort
reaches (`r_bsp_load_obj.cpp`, `r_primarylights.cpp`), `rb_logfile.cpp`, and:

- **No dedicated counterparts.** `src/linux/server_renderer_counterparts.cpp`
  holds the dedicated server's stand-ins for `r_dvars.cpp`, `r_buffers.cpp`,
  `r_rendercmds.cpp`, `r_xsurface.cpp` and `r_bsp_load_obj.cpp`; the probe leaves
  it out and links the owners. With the render-thread state zeroed and `r_loadForRenderer` off, the
  real functions take the same "no render thread, no device" branch the
  counterparts reproduce.
- **Client definitions for the renderer units**, as the census and the synthetic
  test compile them (`RELEASE_ASSERTS` keeps assertions live); the dedicated units
  keep theirs. `r_scene.cpp` and `r_buffers.cpp` go through the D3D9 boundary.
- **`Material_Sort` from the real source.** `r_material_load_obj.cpp` does not
  compile on Linux as a whole (D3DX, Win32 file APIs). Its tail, from
  `Material_GetVertexShaderName` to the end of the file, is `Material_Sort` and
  the comparison helpers; the CMake step copies that span verbatim into a
  generated unit behind a `#line` directive, and refuses if the marker is missing
  or repeated, a helper disappears, or the span gains an `#include`.
- **A two-kind link boundary** (`src/linux/tools/world_frontend_probe_boundary.cpp`):
  zero-initialized storage whose owner (`r_init.cpp`, the window code) does not
  compile on Linux yet, and functions the probe must never reach, each aborting
  with its name. The dedicated server already owns the host services,
  `Com_Error`, the database, `rg`, `rgp` and `s_world`.

The probe and its boundary compile with `-Wall -Wextra -Werror` (engine headers as
system headers). The rules and the spawn scan have their own asset-free test,
`tests/linux_asset_export/world_frontend_invariants_test.cpp`, which
`run_world_surface_checks.py` runs.

### Running it

Build outside the checkout, then point it at the game folder you own and a new
private output directory. The game folder is read only; nothing is copied.

```sh
cmake -S . -B ~/kisak-build/frontend -G Ninja -DKISAK_PLATFORM=linux \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$HOME/kisak-build/frontend/bin \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<prefix>/include/dxvk \
  -DKISAK_BUILD_WORLD_FRONTEND_PROBE=ON
cmake --build ~/kisak-build/frontend --target KisakCOD-world-frontend-probe

KISAK_UI_ASSET_EXPORT_DIR=/private/new-output KISAK_WORLD_FRONTEND_MAP=mp_crash \
  ~/kisak-build/frontend/bin/KisakCOD-world-frontend-probe \
  +set fs_basepath '/authorized/Call of Duty 4' +set fs_homepath /private/new-home \
  +set dedicated 2 +set useFastFile 1
```

### What a pass does not claim

- No device, texture, shader or pixel. Static models, entities, effects, lighting
  and shadows are not checked; the walk reaches static models only to mark them
  visible.
- The spawn-point views are where a player stands, not a sweep of the map.
- It says the front end is internally consistent on that map. Whether those are
  the surfaces the original game draws from that eye is a question for the draw
  step and its GPU evidence.

## Integration with the native D3D9 boundary

The boundary ([linux-native-d3d9.md](linux-native-d3d9.md)) proves 21 renderer
units compile through DXVK Native's real `d3d9.h`, `r_scene.cpp` and
`rb_backend.cpp` among them. Checked locally against the pinned DXVK 2.7.1
headers with the engine's own dialect (`-std=gnu++20`, which is what CMake gives
the engine targets), the renderer units that matter for a map view stand as
follows:

| Unit | Through the boundary |
| --- | --- |
| `r_scene.cpp`, `r_buffers.cpp` | compile unmodified, are on the proof list, and are linked into this test |
| `rb_backend.cpp` | compiles unmodified and is on the proof list. Like `r_scene.cpp` it needs `-std=gnu++20`: under strict `-std=c++20` the `linux` macro is not predefined and `deps/binklib/radbase.h` fails platform detection |
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

1. *Done:* `r_scene.cpp` is linked through the boundary and the two mirrors of
   it (`R_InitScene`'s stage table, `R_SetViewParmsForScene`) are deleted; the
   test calls the production functions.
2. Feed the front end a real `GfxWorld`: the
   [offline probe](#a-real-map-the-offline-front-end-probe) loads one map's world
   from an explicit private game folder (never committed, never searched for) and
   holds every spawn-point view to rules that hold for any map. Batch key order
   turned out not to be one of them (a single stage is emitted in surface order),
   so it is reported, not enforced.
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

Steps 2 to 5 need licensed assets (and from step 3, DXVK Native and a GPU), so
they run on a developer machine, not hosted CI.
