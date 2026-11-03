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

The same binary has a second mode, the free-camera replay
(`KISAK_FREE_CAMERA_MAP=<map>` instead of `KISAK_WORLD_DRAW_MAP`). It starts the
renderer the same way, then hands it to the free camera's frame owner through
the `SceneRenderer` adapter and replays the free camera's reference traversals
on the map. [Linux free camera](linux-free-camera.md#the-replay) describes it.
With `KISAK_FREE_CAMERA_INPUT=window` or `script` the same mode flies the camera
through the renderer's own window instead, live or by a checked script of SDL
events ([the window driver](linux-free-camera.md#the-window-driver)).
The frame, map load and camera code both modes use is in
`src/linux/tools/world_draw_engine.{h,cpp}`.

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
- registers the renderer and effects dvars and turns `fx_marks` off;
- sets `sys_sysMB` to the machine's memory (`sys_info.sysMB`), as a configured
  client's profile holds it (`Sys_ArchiveInfo`). `R_SetPicmip` reads it to pick
  the texture detail. The dedicated server never registers it, and without it
  `R_SetPicmip` saw 0 MB and loaded every texture two mip levels down
  ("Using picmip 2"), which every run before this change did.

Unlike the dedicated server's, it latches nothing off and loads no zone. The
probe's own code is in `src/linux/tools/world_draw_probe.cpp`.

| Stage | When and what | Fails closed when |
| --- | --- | --- |
| `arguments` | before any device work | `KISAK_WORLD_DRAW_MAP` is unset, combined with another export mode, or not an allowlisted map name; `KISAK_WORLD_DRAW_SHOW_SECONDS` is set and not a whole number from 0 to 60 |
| `layout` | the probe and the database loader are compiled with different definitions | any of the shared structs differs in size between the two views, as in the world device probe |
| `start` | inside `$init`, where a client's `CL_InitRenderer` runs | a zone, a D3D object, a world or a registration exists already; `r_loadForRenderer` is off; the renderer would start threads; `fx_marks` is on; `sys_sysMB` is not set; the render commands are not set up |
| `environment` | `DXVK_WSI_DRIVER`, `DXVK_LOG_PATH`, SDL video | the WSI is set to something other than SDL3, or SDL video cannot start |
| `direct3d` | the probe's own `Direct3DCreate9`, released at once | no Direct3D 9 interface. `R_InitGraphicsApi` goes on without one, so the probe asks first |
| renderer start | `R_Init`: the window and device for a 640x480 window, the graphics zones queued behind them, render targets, initial state, the scene, code images and materials | the engine's own errors |
| `renderer` | after `R_Init` | no device, not registered, not the shader model 3 path, the scene not 640x480 windowed, or the device not `D3D_OK`; a code image (white, black, identity normal map) without a texture |
| `graphics`, `world` | the map zone (`DB_LoadXAssets`, synchronous), then the map load the cgame does before its first frame: `CM_LoadMap`, `CM_LinkWorld` and `Com_LoadWorld` as `CL_CM_LoadMap` does with no local server, then `R_LoadWorld` and `R_EndRegistration` | no graphics zone loaded; not exactly one packaged GfxWorld naming the map; a local server runs; the collision map or world data is not the map's; `R_LoadWorld` registers another world |
| `cameras` | the map's entity string, scanned as the front-end probe does | no spawn point; fewer than two distinct cameras |
| `frame`, `readback` | the empty first frame, then each frame (below) | before a frame, the backend would draw the pre-tessellated surfaces from another index buffer than the one the front end fills; the front end did not produce exactly one view; the device is not `D3D_OK` after the frame; the back buffer cannot be read or is not 32-bit RGB |
| `check` | the rules below | a frame breaks a rule |

Camera A is `mp_global_intermission` when the map has one, else the first spawn
point. Camera B is the first spawn point that is not camera A.

### The first frame

Before the first world frame the probe presents one empty frame:
`R_BeginFrame`, `R_BeginSharedCmdList`, `R_ClearClientCmdList2D`, `R_EndFrame`
and `R_IssueRenderCommands(0xFFFFFFFF)` with nothing added, as a client's menu
or loading screen frame is. A client never draws a world as its first frame,
and the renderer depends on that.

The front end writes the indices of the world's batched (pre-tessellated)
surfaces into `gfxBuf.preTessIndexBuffer`, and the backend draws them from the
frame data's `preTessIb` (`R_TessTrianglesPreTessList`). `R_ToggleSmpFrame`
moves both to the next buffer of a pair at the end of every frame. But
`R_ConfigureRenderer` runs `R_InitRenderCommands`, whose toggle sets `preTessIb`
before any device or buffer exists, and `R_Init`'s `R_InitRenderBuffers` then
creates the buffers and points only the front end at one. A world frame drawn
straight after the map load writes its batched surfaces into one buffer and
draws them from none. The empty frame's toggle pairs them. A lost-device
recovery leaves them the same way (`R_ShutdownRenderBuffers` clears `preTessIb`)
and the engine drops that frame and toggles too, so anything that resets the
device must not draw a world frame before one toggle either.

The probe reports `world_draw_first_frame=empty pretess_ib_before=agreed|differed
pretess_ib_after=agreed`, and checks before every frame that the two agree
(stage `frame`).

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
buffer. `R_UpdateStatsTarget` keeps only the shadow cookie and dynamic shadow
targets apart, so the world statistics include the world's draws into the sun's
shadow map. Each frame also carries the static model and sun shadow counts the
free camera holds to rules
([linux-free-camera.md](linux-free-camera.md#static-models-and-the-suns-shadow));
the draw mode reports them and checks none of them. The back buffer is read the way the engine's `R_GetBackBufferData` reads
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

- `world_draw_layout`, `world_draw_renderer=starting|started` (`started`
  gives the adapter, `sys_mb` and the texture detail `R_SetPicmip` chose as
  `picmip=<colour>/<normal map>/<specular>`),
  `world_draw_graphics_zones`, `world_draw_map`;
- two `world_draw_camera` lines, then `world_draw_first_frame`;
- one `world_draw_frame` line per draw, with the measurements and counts
  (world, static models, sun shadow), the
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

With `+set r_logFile <n>` the engine writes its own D3D9 call log, `dx.log`, in
the working directory. The engine opens it only after a frame it presented, so
the probe opens it before the empty frame and writes
`===== world_draw_probe view <A|B> draw <n> =====` into it before each draw. The
log names the calls and the game's images and materials they use, though not
their contents. Keep it in the private home with the rest of the output and
out of the repository.

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
     ever plays (so `R_Cinematic_Shutdown`, which the free-camera replay's
     `R_Shutdown` reaches, has nothing to stop), and `R_AvailableTextureMemory`
     is `r_texturemem.cpp`'s own branch for when DirectDraw cannot report video
     memory;
  4. 29 functions the probe must never reach, each aborting with its name: the
     client screen and loading screen, scene entities and effects (the probe
     adds none, and only the cgame queues effects work), mark generation
     (`fx_marks` is off), material and BSP loading from source (including
     `Material_FreeAll`, which `R_Shutdown` calls only without fast files), and
     the screenshot command.
- **Libraries.** The executable links the D3D9 library and SDL3 for their
  symbols only. The dedicated sources compiled into it never see
  `KISAK_LINUX_NATIVE_D3D9` or SDL's include path.

The probe's own sources build with `-Wall -Wextra -Werror`: the probe, the
shared engine code, the boundary, the SDL3 window functions, and for the
free-camera replay the adapter (`engine_scene_renderer.cpp`), the replay
(`free_camera_replay.cpp`) and the free camera's own sources
(`src/linux/client/view`), and for its window input the window driver
(`free_camera_window_driver.cpp`) and the SDL3 window host
(`src/linux/client/window/window_host.cpp`, `pixel_buffer.cpp`).

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
- the free-camera replay with `mp_crash` must stop at its own
  `stage=direct3d`, and both map variables at once, an unknown map and a show
  time (which belongs to the draw mode) must be refused first;
- the window driver's `window` and `script` inputs with `mp_crash` must stop at
  their own `stage=direct3d`, before the window is adopted, and an unknown
  input, an input in the draw mode and a bad or misplaced time limit must be
  refused first;
- the replay, the live camera and the script must refuse at their start stage
  when `r_drawSModels`, `sm_enable` or `sm_sunEnable` is off, and the draw mode
  with static models and the sun shadow off must still stop at `stage=direct3d`;
- all nineteen runs exit 1 and write nothing.

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

### A GPU and mp_crash: David's laptop, 2026-11-02

Commit ea9d953, local evidence, not CI.

| | |
| --- | --- |
| Machine | Debian 13, GNOME Wayland (`video_driver=wayland`); Intel Processor U300 with Raptor Lake-P graphics (Vulkan "Intel(R) Graphics (RPL-U)") |
| Graphics stack | Mesa ANV 25.0.7; DXVK Native 2.7.1 i386; SDL 3.2.10 i386 |
| Compiler | clang 19.1.7; no warnings from `src/linux/` in either build |
| Data | the owner's installed game, read through symlinks; the game folder listing was identical before and after every run |

**Both runs pass.** RelWithDebInfo and Debug each exit 0 with
`world_draw_probe=passed problems=0`, in 5.1 s and 7.1 s of wall time. Every
frame is bit-identical between the two builds, and the two reports are equal
once timings are removed. No line has `ERROR`, `FATAL`, `ASSERT` or
`Couldn't`, and DXVK printed no warning or error.

- **Start.** `R_Init` created the device on the Intel GPU (DXVK reports its AMD
  stand-in identity as the adapter name), shader model 3, 640x480 windowed, in
  0.65 s. The four graphics zones were loaded 2.6 s after the start, and
  mp_crash 1.3 s later.
- **Cameras.** A is `mp_global_intermission` at (336, 60, 260), yaw 65. B is the
  first `mp_tdm_spawn`, at (-156, 15, 222), yaw 0.
- **Frames.** Each view's last draw:

  | View | Draws | Colours | Dominant colour | Mean luma | Detail tiles | Lit / decal / emissive surfaces | World draw calls | World triangles |
  | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: |
  | A | 9 | 63,434 | 0.3% | 122.0 ± 44.6 | 16 of 16 | 262 / 7 / 0 | 399 | 43,788 |
  | B | 6 | 49,968 | 0.2% | 117.3 ± 38.8 | 16 of 16 | 276 / 3 / 0 | 387 | 36,864 |

  A's settled digest is `7e8468e89644e49f` and B's `5162ef64555f566c`. The
  first draw of A took 0.23 s and later draws about 0.06 s (0.1 s in Debug).
- **Settling.** The world draw calls and triangles are the same on every draw
  of a view. The static model cache took 102, 21, 52, 86, 148, 256, 94, 55 and
  0 surfaces over A's nine draws, and 92, 52, 114, 56, 3 and 0 over B's. Draws 4
  and 5 of A, and 0 and 1 of B, had the same pixels while the cache was still
  filling, which is why the settle rule counts the cache too. The lit surface
  count moves by a few (260 to 264 for A) as static models move between their
  rigid and cached forms.
- **The first draw of A was dark.** It had mean luma 28.5 against 122.0 for
  every later draw, 21,646 colours and one colour on 5.2% of the frame, with
  the same draw calls and triangles. The probe now presents an empty frame
  first, and the first draw is right ([below](#the-dark-first-frame-2026-11-02)).
- **On screen.** At 4145c00 (the same draw, with the earlier settle rule), a run
  with `KISAK_WORLD_DRAW_SHOW_SECONDS=20` kept view A on the laptop's screen for
  20 s: 350 frames, about 17.5 a second including the read-back of each. No
  image was saved.

The fixes it took to get here, each found on this machine: the probe now
configures the renderer with `R_ConfigureRenderer` as the client's `CL_InitRef`
does (without it `R_Init` stopped on the model lighting atlas's size), and
loads the collision map as `CL_CM_LoadMap` does before the first scene (static
model lighting traces against it).

### The dark first frame, 2026-11-02

Same laptop, local evidence, not CI. Runs at ea9d953 to 552ef1e looked for the
cause, all RelWithDebInfo with one change each. Every run passed:

| Run | First draw of A (mean luma) | What it showed |
| --- | --- | --- |
| baseline | 28.5 | dark, with the same draw calls and triangles as later draws |
| `+set sm_enable 0` | 29.2 | not the sun shadow maps |
| `+set r_smc_enable 0` | 28.3 | not the static model cache |
| `+set r_drawSModels 0` | 0.0, 15 colours | without the static models nothing is left on that frame: the world's surfaces are missing, not just dark |
| `+set r_pretess 0` | 122.0 | right: only the pre-tessellated (batched) world path goes wrong |
| PreLoad on all 23 of the world's images | still dark | not texture residency |
| device state against the engine's state cache, before draws 0 and 1 of each view | 28.5 | every render state, sampler state, texture and the viewport matched; setting the cache's values on the device changed nothing |
| only the first draw with `r_pretess 0` | 122.0, and 122.0 for the next | the first pre-tessellated draw after one plain frame is right, with the baseline's second-draw digest |

So the trouble was the first frame of the process, not the first
pre-tessellated one. That points at the frame data's index buffer pair, which
the first frame is the only one to use before a toggle: see
[The first frame](#the-first-frame). The device-state and residency checks were
temporary and are not in the probe.

At 6dc8198 the probe presents one empty frame first. It reports
`pretess_ib_before=differed pretess_ib_after=agreed`, and:

| Build | First draw of A | A settles | B settles | Wall time |
| --- | --- | --- | --- | ---: |
| RelWithDebInfo | 63,431 colours, luma 122.0 ± 44.6, digest `46a7517b8c271058` | 9 draws, `7e8468e89644e49f` | 6 draws, `5162ef64555f566c` | 4.1 s |
| Debug | the same, every frame bit-identical to RelWithDebInfo | the same | the same | 5.6 s |

The settled digests are the ones the probe gave before the fix, so only the
first draw changed. A's first draw is now the image `r_pretess 0` gives for it.
With `r_pretess 0` the run also passes with that path's own settled images. A
run with `KISAK_WORLD_DRAW_SHOW_SECONDS=10` presented 207 frames of view A on
the screen. No image was saved, no line had `ERROR`, `FATAL`, `ASSERT` or
`Couldn't`, and the game folder listing was identical before and after.
