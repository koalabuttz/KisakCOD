# World draw probe (opt-in)

`KisakCOD-world-draw-probe` draws views of a real map through the engine's own
renderer on native i386 Linux. It is the offline asset exporter (the dedicated
server's sources plus `src/linux/tools`) with the whole renderer linked in,
except the four units the client compile census still blocks.

It starts the renderer with `r_init.cpp`'s own `R_Init`, loads one allowlisted
map, registers it with `R_LoadWorld`, and draws it from the map's own cameras
the way the engine's cgame-free scene render does
(`R_GenerateReflectionRawData` in `r_reflection_probe.cpp`). Each frame goes
through the front end (DPVS, BSP surface collection, sorting and batching) and
the backend's material passes to the D3D9 device, and is presented to a window.
The probe then reads the back buffer into memory and checks it.

It is the first draw of the static map view milestone
([order of work](linux-world-frontend.md#order-of-work-toward-one-static-map-view)),
after the [world device probe](linux-world-device-probe.md), which loads the same
map onto the device without drawing it.

It reads game data only through the engine's file system, from the folder it is
given. It writes one report, `world-draw-probe.json` (cameras, counts and frame
measurements, no pixels), into a private output directory. No frame is saved.
Like the device probes, it sets `DXVK_LOG_PATH=none` unless the caller set it.

## What runs

The exporter's normal start runs: `Com_Init`, the file system, and the probe's
own `CL_InitDedicated`. That `CL_InitDedicated`:

- configures the renderer as the client's `CL_InitRef` does, with
  `R_ConfigureRenderer`: the client's configuration (`code_post_gfx_mp`,
  `common_mp`, the localized zones, `mod` if present, no `ui_mp`, one client
  view), then `R_InitRenderCommands`, which sizes the model lighting atlas and
  allocates the render command and scene buffers;
- turns `sys_smp_allowed` off before the renderer registers its dvars, so the
  two worker thread dvars default off. Worker commands then run inline, the
  backend runs on the main thread, and `R_InitHardware` starts no worker;
- registers the renderer and effects dvars and turns `fx_marks` off.

Unlike the dedicated server's, it latches nothing off and loads no zone. The
probe's own code is in `src/linux/tools/world_draw_probe.cpp`.

| Stage | When and what | Fails closed when |
| --- | --- | --- |
| `arguments` | before any device work | `KISAK_WORLD_DRAW_MAP` is unset, combined with another export mode, or not an allowlisted map name; `KISAK_WORLD_DRAW_SHOW_SECONDS` is set and not a whole number from 0 to 60 |
| `layout` | the probe and the database loader are compiled with different definitions | any of the shared structs differs in size between the two views, as in the world device probe |
| `start` | inside `$init`, where a client's `CL_InitRenderer` runs | a zone, a D3D object, a world or a registration exists already; `r_loadForRenderer` is off; the renderer would start threads; `fx_marks` is on; the render commands are not set up |
| `environment` | `DXVK_WSI_DRIVER`, `DXVK_LOG_PATH`, SDL video | the WSI is set to something other than SDL3, or SDL video cannot start |
| `direct3d` | the probe's own `Direct3DCreate9`, released at once | no Direct3D 9 interface. `R_InitGraphicsApi` goes on without one, so the probe asks first |
| renderer start | `R_Init`: the window and device for a 640x480 window, the graphics zones queued behind them, render targets, initial state, the scene, code images and materials | the engine's own errors |
| `renderer` | after `R_Init` | no device, not registered, not the shader model 3 path, the scene not 640x480 windowed, or the device not `D3D_OK`; a code image (white, black, identity normal map) without a texture |
| `graphics`, `world` | the map zone (`DB_LoadXAssets`, synchronous), then the map load the cgame does before its first frame: `CM_LoadMap`, `CM_LinkWorld` and `Com_LoadWorld` as `CL_CM_LoadMap` does with no local server, then `R_LoadWorld` and `R_EndRegistration` | no graphics zone loaded; not exactly one packaged GfxWorld naming the map; a local server runs; the collision map or world data is not the map's; `R_LoadWorld` registers another world |
| `cameras` | the map's entity string, scanned as the front-end probe does | no spawn point; fewer than two distinct cameras |
| `frame`, `readback` | each frame (below) | the front end did not produce exactly one view; the device is not `D3D_OK` after the frame; the back buffer cannot be read or is not 32-bit RGB |
| `check` | the rules below | a frame breaks a rule |

Camera A is `mp_global_intermission` when the map has one, else the first spawn
point. Camera B is the first spawn point that is not camera A.

### One frame

Each frame repeats `R_GenerateReflectionRawData`'s sequence without its effects
update: `R_InitPrimaryLights`, `R_BeginFrame`, `R_BeginSharedCmdList`,
`R_ClearClientCmdList2D`, `R_ClearScene`, a refdef for the camera (the scene's
full size, a 65° horizontal field of view as `cg_fov`'s default gives, scene
time 0), `R_SetLodOrigin`, `R_RenderScene`, `R_EndFrame` and
`R_IssueRenderCommands(0xFFFFFFFF)`.

Before `R_EndFrame` the probe reads the front end's counts for the view: lit,
decal and emissive draw surfaces. After the backend ran and presented, it reads
the backend's own world statistics (draw calls and triangles) and the back
buffer. The back buffer is read the way the engine's `R_GetBackBufferData` reads
it after its cubemap frames: `GetBackBuffer`, an offscreen plain surface,
`GetRenderTargetData`, `LockRect`.

Each view is drawn until it settles, at most 64 times: view A first, then view
B. A view has settled when two draws in a row give the same pixels and the
second moved nothing new into the static model cache. The first draws of a view
differ while the renderer's per-frame state settles. The static model cache
takes at most 256 surfaces and 8192 vertices a frame
(`R_CacheStaticModelSurface`), for the camera and for the sun shadow map, so a
view's static models move into it over several frames. Pixels can match by
chance while it is still filling, which is why the cache counts too.

## What a frame is held to

There is no reference image to compare with, and none may be stored. The rules
say what any drawn view of a real map has and an undrawn or broken frame lacks.
They are pure functions in `src/linux/tools/world_draw_rules.h`; their
asset-free test is `world_draw_rules` in the world surface runner, on frames
built in memory.

| Rule | Applies to | Threshold |
| --- | --- | --- |
| size | each checked frame | the back buffer is the 640x480 scene |
| colours | each checked frame | at least 256 distinct colours |
| no dominant colour | each checked frame | no single colour covers more than half the frame |
| coverage | each checked frame | at least 12 of 16 tiles (4x4) hold more than one colour |
| brightness | each checked frame | mean luma (Rec. 601) between 4 and 251 |
| emitted and drawn | each checked frame | the front end emitted at least one world surface, and the backend counted world draw calls and triangles |
| settles | views A and B | the last two draws of the view have the same pixels, and the last one moved nothing into the static model cache |
| follows the camera | views A and B | different pixels |

The last draw of each view is checked. The thresholds are
first estimates for a 640x480 view; the evidence below says what mp_crash gives.

The output is `key=value` lines between the engine's own messages, flushed as
they happen:

- `world_draw_layout`, `world_draw_renderer=starting|started`,
  `world_draw_graphics_zones`, `world_draw_map`;
- two `world_draw_camera` lines;
- one `world_draw_frame` line per draw, with the measurements and counts, the
  surfaces it moved into the static model cache (`smc_patches`) and a 64-bit
  digest of the pixels, and one `world_draw_view` line per view with its number
  of draws and whether it settled;
- `world_draw_problem` lines, then `world_draw_probe=passed|failed problems=N`.

A pass ends with `asset_export=complete mode=world_draw_probe map=<map>` and exit
0. Any failure ends with
`Offline asset export failed: world_draw_probe stage=<stage>: <reason>` and exit 1.

`KISAK_WORLD_DRAW_SHOW_SECONDS=<n>` (0 to 60) keeps drawing view A for that many
seconds after a pass, so a person at the machine can see it. Each frame is only
presented; nothing is saved.

## How it links

`scripts/platform/linux/world_draw_probe.cmake`, with
`-DKISAK_BUILD_WORLD_DRAW_PROBE=ON` on top of the world device probe's options:

- **Renderer units.** Every unit in `src/gfx_d3d` except the four the client
  compile census blocks (`scripts/tests/client_compile_census_blocked.txt`):
  `r_material_load_obj.cpp` (D3DX; only fast files load here),
  `r_screenshot.cpp` (Win32 monitor API), `r_cinematic.cpp` (Bink) and
  `r_texturemem.cpp` (DirectDraw). On top of the world device probe's units:
  - 15 units on the native D3D9 proof list
    (`scripts/platform/linux/native_d3d9_renderer_sources.txt`) go through the
    [native boundary](linux-native-d3d9.md): `r_draw_bsp`, `r_draw_lit`,
    `r_draw_sunshadow`, `r_model_lighting`, `r_setstate_d3d`, `r_shade`,
    `r_state`, `r_state_utils`, `rb_backend`, `rb_depthprepass`, `rb_draw3d`,
    `rb_imagetouch`, `rb_pixelcost`, `rb_shade`, `rb_sky`;
  - 53 more keep the type-only stand-in, which the census proves they compile
    with and which gives them no way to call the device;
  - `EffectsCore/fx_dvars.cpp`, for the effects dvars the scene reads.

  The configure stops if a unit in `src/gfx_d3d` is in no list or in two, or if
  a census-blocked unit is listed. All renderer units use the client
  definitions (`RELEASE_ASSERTS` keeps assertions live).
- **Left out of the dedicated sources:** `server_device_counterparts.cpp`,
  `server_renderer_counterparts.cpp` and `server_draw_counterparts.cpp`, whose
  owners are all linked here, and `sys_splash.cpp`, as in the world device
  probe. The configure stops if the dedicated target no longer lists one of
  them. `server_assets.cpp` stays (`FX_Register` and the sound archiving
  no-ops).
- **The link boundary.** `src/linux/tools/world_draw_probe_boundary.cpp` holds
  what the linked units reach and no linked unit defines, in four kinds:
  1. storage whose owner is not linked (`onlinegame`, `r_fullscreen`,
     `vid_xpos`, `vid_ypos`);
  2. per-frame calls the scene makes into client systems this process never
     starts, each doing what its owner does when that system holds nothing:
     sound (`CL_UpdateSound`), client debug data, the cgame's FPS history,
     effects physics, dynamic entity physics and broken pieces.
     `DynEntCl_ProcessEntities` checks its claim: it aborts if any client
     dynamic entity of the map has a physics body;
  3. the two Windows-only renderer services as Linux has them: no cinematic
     ever plays, and `R_AvailableTextureMemory` is `r_texturemem.cpp`'s own
     branch for when DirectDraw cannot report video memory;
  4. 28 functions the probe must never reach, each aborting with its name: the
     client screen and loading screen, scene entities and effects (the probe
     adds none, and only the cgame queues effects work), mark generation
     (`fx_marks` is off), material and BSP loading from source, and the
     screenshot command.
- **Libraries.** The executable links the D3D9 library and SDL3 for their
  symbols only. The dedicated sources compiled into it never see
  `KISAK_LINUX_NATIVE_D3D9` or SDL's include path.

The probe's own sources build with `-Wall -Wextra -Werror`.

## Limits

What this probe draws is the world as the renderer sees it with no game
running:

- no effects, no marks, no dynamic entity physics, no sound, no cgame, no scene
  entities (no players, weapons or script models), no 2D;
- one thread: no render thread, no worker threads, the backend on the main
  thread;
- scene time 0 on every frame;
- no vision set, film, glow or depth of field settings from a cgame, so postfx
  runs with the refdef's zeroed values;
- fast files only; no cinematics.

It proves the engine's front end, backend and material passes draw a real map
on a real device and that the frame looks like a drawn view. It does not
compare the frame with the retail game's image.

## Running it

Build outside the checkout, with the same DXVK Native and SDL3 as the device
probes:

```sh
cmake -S . -B ~/kisak-build/world-draw-rel -G Ninja -DKISAK_PLATFORM=linux \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$HOME/kisak-build/world-draw-rel/bin \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk-prefix>/include/dxvk \
  -DKISAK_DXVK_NATIVE_LIBRARY=<dxvk-prefix>/lib32/libdxvk_d3d9.so -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3 \
  -DKISAK_BUILD_DEVICE_PROBE=ON -DKISAK_BUILD_WORLD_FRONTEND_PROBE=ON -DKISAK_BUILD_WORLD_DEVICE_PROBE=ON \
  -DKISAK_BUILD_WORLD_DRAW_PROBE=ON
cmake --build ~/kisak-build/world-draw-rel --target KisakCOD-world-draw-probe
```

Run it as the world device probe runs, from a staging tree that holds only
symlinks to the game's `main`, `zone` and `localization.txt`, with a separate
empty writable home as the working directory:

```sh
MESA_SHADER_CACHE_DIR=~/kisak-private/mesa-cache \
LD_LIBRARY_PATH=<dxvk-prefix>/lib32:<sdl3-sdk>/lib \
KISAK_UI_ASSET_EXPORT_DIR=$HOME/kisak-private/draw-world-1 KISAK_WORLD_DRAW_MAP=mp_crash \
  ~/kisak-build/world-draw-rel/bin/KisakCOD-world-draw-probe \
  +set fs_basepath <staging> +set fs_homepath <home> +set dedicated 2 +set useFastFile 1
```

The output directory must be absolute, new or empty, and mode 0700. Keep the
log outside it.

## CI

The `device_probe` job builds the probe in Debug and RelWithDebInfo against the
stand-in `libd3d9_unavailable.so`, in the same build directory as the world
device probe. It checks that the binary is ELF32 i386 and needs only SDL3, the
D3D9 library and the C and C++ runtimes. It then runs
`scripts/tests/test_world_draw_probe.py`, which starts the engine from a
temporary tree holding nothing but a one-line stub `fileSysCheck.cfg`:

- `mp_crash` must pass the layout check and stop at `stage=direct3d`, before
  `R_Init` runs;
- an unset map, an unknown map and a show time over 60 seconds must be refused
  first;
- all four runs exit 1 and write nothing.

No DXVK library, Vulkan, GPU or game data is involved. A pass there means the
probe builds and fails closed. It says nothing about a device, a map or a frame.

The `world_surface` job runs the frame rules' asset-free test
(`world_draw_rules`).

## Evidence

### Software Vulkan: the cloud sandbox, 2026-11-02

This is not hardware evidence and not a draw. The sandbox has no game data.

- **Setup.** Ubuntu 24.04 and clang 18.1.3. DXVK Native 2.7.1 i386, SDL 3.2.10
  i386 with X11, Xvfb, and Mesa lavapipe.
- **RelWithDebInfo run with a stub tree.** The layout check passed, `R_Init`
  created the window and the device, queued the graphics zones and reached
  `Initializing render targets...`. The engine then stopped with its own fatal
  error, `Could not find zone .../code_post_gfx_mp.ff`, exit 1.
- **Stand-in library.** The fail-closed script passes 16 of 16 checks.
