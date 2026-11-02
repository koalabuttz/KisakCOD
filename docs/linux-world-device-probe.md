# World device probe (opt-in)

`KisakCOD-world-device-probe` loads a real map onto a Direct3D 9 device on native
i386 Linux. It is the offline asset exporter (the dedicated server's sources plus
`src/linux/tools`) with the renderer's own device code linked in. It creates the
window and device through `r_init.cpp`, as the [device probe](linux-device-probe.md)
does. It then loads the graphics zones and one allowlisted map with
`r_loadForRenderer` on, so the engine's own loaders create every texture, shader,
vertex declaration and vertex/index buffer on that device. Last, it checks that
every one of them exists and matches its asset.

It is the step between the device and the first draw in the static map view
milestone ([order of work](linux-world-frontend.md#order-of-work-toward-one-static-map-view)).
Nothing is drawn yet.

It reads game data only through the engine's file system, from the folder it is
given. It writes one report, `world-device-probe.json` (counts, asset names and
problems, no asset bytes), into a private output directory, and keeps no image.
Like the device probe, it sets `DXVK_LOG_PATH=none` unless the caller set it.

## What runs

The exporter's normal start runs: `Com_Init`, the file system, and the probe's
own `CL_InitDedicated`. That `CL_InitDedicated` gives the renderer the client's
configuration (`code_post_gfx_mp`, `common_mp`, the localized zones, `mod` if
present, no `ui_mp`) and registers its dvars. Unlike the dedicated server's, it
latches nothing off and loads no zone. The probe's own code is in
`src/linux/tools/world_device_probe.cpp`.

| Stage | When and what | Fails closed when |
| --- | --- | --- |
| `arguments` | before any device work | `KISAK_WORLD_DEVICE_MAP` is unset, combined with another export mode, or not an allowlisted map name |
| `layout` | the probe and the database loader are compiled with different definitions, and the probe sees the D3D9 types through DXVK's headers | any of 32 shared structs (world, material, image, zone) differs in size between the two views |
| `start` | inside `$init`, where a client's `CL_InitRenderer` runs | a zone, a D3D object or a world exists already, or `r_loadForRenderer` is off |
| `environment`, `direct3d`, `renderer`, `window`, `device` | `R_PreCreateWindow`, `R_SetWndParms`, `R_CreateWindow`, `R_CreateDevice` for a 640x480 window, as in the device probe | as in the [device probe](linux-device-probe.md#what-runs) |
| graphics zones | `r_init.cpp`'s own `R_LoadGraphicsAssets`, queued on the database thread right after the device, as `R_InitHardware` does; the exporter waits for them | the engine's own errors (a missing zone is a fatal error) |
| `graphics`, `world` | the map zone (`DB_LoadXAssets`, synchronous), then `R_SetWorldPtr_FastFile`, the renderer's own world lookup | no graphics zone loaded; not exactly one packaged GfxWorld, or it does not name the map, or the lookup finds another |
| `check` | the checks below | any device object breaks a rule |

The output is `key=value` lines between the engine's own messages, flushed as
they happen:

- `world_device_zone`, one line per loaded zone;
- `world_device_images`, `world_device_techniques`, `world_device_world` and
  `world_device_memory`;
- up to 48 `world_device_problem` lines;
- `world_device_probe=passed|failed problems=N`.

A pass ends with
`asset_export=complete mode=world_device_probe map=<map>` and exit 0. Any failure
ends with `Offline asset export failed: world_device_probe stage=<stage>: <reason>`
and exit 1.

## What every device object is held to

The rules are pure functions in `src/linux/tools/world_device_rules.h`. Their
asset-free test is `world_device_rules` in the world surface runner.

| Objects | Rule |
| --- | --- |
| every image of every loaded zone | It has a texture and no pending delayed load. The texture's resource type equals the image's map type (2D, 3D or cube). It has at least one level, and level 0 has the width, height and depth the loader recorded. A cube face is square, and only a volume has depth. |
| every shader program of every technique set | A program built for the path in use (shader model 3) has its device object. A program built for the other path has none. |
| every pass of every technique | It has a vertex shader, a pixel shader and a built vertex declaration. |
| every loaded zone | The vertex and index blocks (blocks 7 and 8) each have a static buffer exactly when the block is not empty. The buffer is unlocked and at least the block's size. |
| the map's world | The world vertex buffer and the vertex layer buffer exist exactly when the world has that data, and hold all of it. The lightmaps, reflection probes, sky image and outdoor image have textures. |
| every material a world surface uses | Every pass of every technique in the set `Material_GetTechniqueSet` returns has device shaders. Each also has the vertex declaration that the BSP path selects (`R_SetVertexDeclTypeWorld`, reproduced as `kisak_world_inventory::DeclarationType`). Every texture the material samples (the water image for a water map) has a texture. |

The device must still report `D3D_OK` after loading.

## How it links

`scripts/platform/linux/world_device_probe.cmake`, with
`-DKISAK_BUILD_WORLD_DEVICE_PROBE=ON`. It needs the [world front-end
probe](linux-world-frontend.md#a-real-map-the-offline-front-end-probe) and the
device probe options on as well, because it starts from their source lists:

- **Renderer units.** It compiles the front end's renderer units and the device
  probe's again, with the client definitions (`RELEASE_ASSERTS` keeps assertions
  live). It adds the owners of the device objects a zone load creates and
  releases:
  - `r_image.cpp`, `r_image_load_common.cpp`, `r_image_load_obj.cpp` and
    `r_image_wavelet.cpp` for textures;
  - `rb_uploadshaders.cpp` and `r_material_override.cpp` for the shader queue
    and the technique remap;
  - `r_utils.cpp`.

  Units that call D3D9 go through the [native boundary](linux-native-d3d9.md).
- **`r_utils.cpp`'s guard.** `R_ReleaseD3DSurface` is the release every static
  buffer and shader goes through. Its guard is now
  `!defined(__linux__) || defined(KISAK_LINUX_NATIVE_D3D9)`, so it is compiled
  wherever the D3D9 API is real. With the type-only stand-in it still does not
  exist, so nothing there can release a device object.
- **Left out of the dedicated sources:**
  - `src/linux/server_device_counterparts.cpp`, which holds the dedicated
    renderer start and its `r_loadForRenderer`-off counterparts for `r_init.cpp`,
    `r_image.cpp`, `r_image_load_obj.cpp`, `r_material.cpp`,
    `r_material_override.cpp` and `rb_uploadshaders.cpp`;
  - `src/linux/server_renderer_counterparts.cpp`, as in the front-end probe;
  - `src/linux/sys_splash.cpp`, whose `Sys_DestroySplashWindow` `r_init.cpp`
    owns here.

  The configure stops if the dedicated target no longer lists one of them.
  `server_draw_counterparts.cpp` stays, because the owners of what it holds
  (`R_RegisterModel`, the shader-cache clears, the static model cache check and
  `Load_PicmipWater`) are units this probe does not draw with; the
  [world draw probe](linux-world-draw-probe.md) links those owners instead.
  `server_assets.cpp` keeps what every target needs and no device touches.
- **The link boundary.** `src/linux/tools/world_device_probe_boundary.cpp` holds:
  - storage whose owner is not linked (`onlinegame`, `r_fullscreen`,
    `vid_xpos`, `vid_ypos`);
  - four functions the probe must never reach, each aborting with its name:
    `CL_IsLocalClientInGame`, `Material_Load`,
    `Material_FindTechniqueSet_LoadObj` and `Material_Duplicate`.
- **Libraries.** The executable links the D3D9 library and SDL3 for their symbols
  only (`$<LINK_ONLY:SDL3::SDL3>`). The dedicated sources compiled into it never
  see `KISAK_LINUX_NATIVE_D3D9` or SDL's include path.

The probe's own sources build with `-Wall -Wextra -Werror`.

## Running it

Build outside the checkout, with the same DXVK Native and SDL3 as the device
probe:

```sh
cmake -S . -B ~/kisak-build/world-device-rel -G Ninja -DKISAK_PLATFORM=linux \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$HOME/kisak-build/world-device-rel/bin \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON -DKISAK_DXVK_NATIVE_INCLUDE_DIR=<dxvk-prefix>/include/dxvk \
  -DKISAK_DXVK_NATIVE_LIBRARY=<dxvk-prefix>/lib32/libdxvk_d3d9.so -DSDL3_DIR=<sdl3-sdk>/lib/cmake/SDL3 \
  -DKISAK_BUILD_DEVICE_PROBE=ON -DKISAK_BUILD_WORLD_FRONTEND_PROBE=ON -DKISAK_BUILD_WORLD_DEVICE_PROBE=ON
cmake --build ~/kisak-build/world-device-rel --target KisakCOD-world-device-probe
```

Run it from a staging tree that holds only symlinks to the game's `main`, `zone`
and `localization.txt`, with a separate empty writable home as the working
directory. Mesa caches compiled shaders under `~/.cache` unless
`MESA_SHADER_CACHE_DIR` points elsewhere:

```sh
MESA_SHADER_CACHE_DIR=~/kisak-private/mesa-cache \
LD_LIBRARY_PATH=<dxvk-prefix>/lib32:<sdl3-sdk>/lib \
KISAK_UI_ASSET_EXPORT_DIR=$HOME/kisak-private/device-world-1 KISAK_WORLD_DEVICE_MAP=mp_crash \
  ~/kisak-build/world-device-rel/bin/KisakCOD-world-device-probe \
  +set fs_basepath <staging> +set fs_homepath <home> +set dedicated 2 +set useFastFile 1
```

Keep the log outside the output directory: the probe needs that directory new
or empty.

The output directory must be absolute, new or empty, and mode 0700.
`KISAK_WORLD_DEVICE_MAP` is required, and the probe refuses every other export
mode.

## CI

The `device_probe` job builds the probe in Debug and RelWithDebInfo against the
stand-in `libd3d9_unavailable.so`. It checks that the binary is ELF32 i386 and
needs only SDL3, the D3D9 library and the C and C++ runtimes. It then runs
`scripts/tests/test_world_device_probe.py`, which starts the engine from a
temporary tree holding nothing but a one-line stub `fileSysCheck.cfg`:

- `mp_crash` must pass the layout check and stop at `stage=direct3d`, before any
  zone is queued;
- an unset map and an unknown map must be refused first;
- all three runs exit 1 and write nothing.

No DXVK library, Vulkan, GPU or game data is involved. A pass there means the
probe builds and fails closed. It says nothing about a device or a map.

The `world_surface` job runs the rules' asset-free test (`world_device_rules`).

## Evidence

### Software Vulkan: the cloud sandbox, 2026-11-02

This is not hardware evidence and not a map load. The sandbox has no game data.

- **Setup.** Ubuntu 24.04 and clang 18.1.3. DXVK Native 2.7.1 i386
  (`-Dnative_sdl3=enabled`), SDL 3.2.10 i386 with X11, Xvfb, and Mesa lavapipe.
- **RelWithDebInfo run with a stub tree:**
  - `world_device_layout=agreed types=32`;
  - `video_driver=x11`;
  - `device=created renderer=shader_model_3 window=640x480`;
  - `world_device_zones=queued code=code_post_gfx_mp common=common_mp ...`.

  Then the engine stopped with its own fatal error: `Could not find zone
  .../zone/english/code_post_gfx_mp.ff`, exit 1. Device creation, the shader
  model 3 path and the queueing of the graphics zones behind it work. The loading
  itself did not run.
- **Stand-in library.** With `libd3d9_unavailable.so`, the fail-closed script
  passes 13 of 13 checks.
- **Negative control.** The same script run against real DXVK on lavapipe fails
  3 checks, because the device is created there. This shows the script does
  detect a run that gets past `Direct3DCreate9`.

### A GPU and mp_crash: David's laptop, 2026-11-02

Commit c2aea46, local evidence, not CI.

| | |
| --- | --- |
| Machine | Debian 13, GNOME Wayland (`video_driver=wayland`); Intel Processor U300 with Raptor Lake-P graphics (Vulkan "Intel(R) Graphics (RPL-U)") |
| Graphics stack | Mesa ANV 25.0.7; DXVK Native 2.7.1 i386; SDL 3.2.10 i386 |
| Compiler | clang 19.1.7; no warnings from `src/linux/` in either build |
| Data | the owner's installed game, read through symlinks; the game folder listing was identical before and after |

**Both runs pass.** RelWithDebInfo and Debug each exit 0 with
`world_device_probe=passed problems=0` in about 11 s. The two reports are
identical apart from timings, and so are the engine's log lines apart from
paths, times and the build tag. No line has `ERROR`, `FATAL`, `ASSERT` or
`Couldn't`, and DXVK printed no warning or error.

- **Device.** DXVK skipped llvmpipe and created the device on the Intel GPU
  (the adapter line shows its AMD stand-in identity again). Shader model 3,
  640x480 windowed, 4086 MB of texture memory reported.
- **Zones.** The four graphics zones loaded in 4.2 s (4.7 s in Debug), then
  mp_crash in 6.2 s (6.0 s). Each zone's vertex and index blocks are in buffers
  of their size:

  | Zone | Vertex bytes | Index bytes |
  | --- | ---: | ---: |
  | `code_post_gfx_mp` | 12,224 | 1,264 |
  | `localized_code_post_gfx_mp` | 0 | 0 |
  | `common_mp` | 9,572,992 | 1,128,448 |
  | `localized_common_mp` | 0 | 0 |
  | `mp_crash` | 13,404,672 | 2,100,116 |

- **Images.** All 1,630 have a texture of the recorded kind and size: 1,608 2D,
  1 volume and 21 cube maps, 344.5 MB on the card. By category, 1,601 were
  loaded from file, 27 are auto-generated and 2 are lightmaps. None was
  substituted.
- **Shaders.** 352 technique sets, 1,412 techniques and 1,414 passes. All 420
  vertex and 850 pixel programs built for shader model 3 have device shaders,
  and the 242 and 424 built for shader model 2 have none. Each of the 32 vertex
  declarations was built. Not every declaration has every vertex type (per
  type: 16, 23, 23, 32 ... 5, 32), which is why the world rule asks for the one
  type the BSP path selects rather than all of them.
- **The world.** `maps/mp/mp_crash.d3dbsp`: 5,558 surfaces use 134 materials.
  Their 2,333 techniques and 2,503 passes all have device shaders and the world
  vertex declaration the BSP path selects. Their 334 textures and the world's
  23 lightmap, reflection-probe, sky and outdoor images all exist. The world
  vertex buffer holds all 166,418 vertices (7,322,392 bytes), and the vertex
  layer buffer holds its 4 bytes.
- **Left behind.** The private output holds the 1,170-byte report (mode 0600)
  and the homes hold only `main/console_mp.log`. DXVK wrote no log file.

What this does not show: that anything renders, the shader model 2 path, other
GPUs or drivers, or any map but mp_crash.

## Limits

- **Nothing is drawn.** The probe proves the device objects a map view needs
  exist. It does not prove they render correctly.
- **No unload.** The probe never unloads a zone, never releases the device and
  never calls `R_ShutdownWorld`, which needs the model lighting and static model
  cache state. The process ends with `_Exit`, after the report.
- **One configuration:** a 640x480 window, the shader model 3 path only, the
  first adapter and the renderer dvars' defaults.
- **No `ui_mp`**, as on a dedicated server.
- **`R_InitHardware` stops after the zones.** Render targets, the initial
  state, the scene and `R_InitSystems` (code images, materials, fonts, water,
  lights) do not run. Images the zones do not carry are read by the engine from
  `images/<name>.iwi` through the file system (`R_DelayLoadImage`). When a file
  cannot be read, the client would substitute a code image (white, black or a
  flat normal map). Those are registered by `R_InitSystems`, so here the engine
  should stop with its own "Couldn't load image" error instead. That path has
  not been observed yet.
- **DXVK.** Its failure to create a Vulkan instance aborts the process, as
  described in the device probe's limits.
