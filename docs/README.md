# KisakCOD documentation

## Start here

**[Linux port status](status.md)**: what works, what is verified and where, and
what is still missing. Build steps are in the
[top-level README](../README.md#how-to-build).

## Playing on Linux (opt-in)

- [Multiplayer client](linux-mp-client.md): `KisakCOD-mp`, the whole game in one
  native process; playable on a listen server (i386 and x86_64). It joins a
  remote server with a key code or, opt-in, through Steam
- [Singleplayer game](linux-sp-game.md): `KisakCOD-sp`, built like the
  multiplayer client from the singleplayer lists; killhouse and cargoship load
  and play on i386, and the x86_64 build has not loaded a level yet
- [Singleplayer levels](linux-sp-levels.md): what the other levels use, the
  level sweep tool and the laptop run plan
- [Cinematics](linux-cinematics.md): Bink movies through an FFmpeg-backed shim
  (`KISAK_LINUX_CINEMATICS`)
- [Steam authentication](linux-steam.md): join and host Steam-authenticated
  servers through the running Steam client (`KISAK_LINUX_STEAM`), or host as
  an anonymous Steam game server
- [Key codes](key-codes.md): join a server that accepts unverified GUIDs with
  a CoD4 key code and no Steam (all platforms)
- [Finding servers](server-browser.md): local list, favourites, `connect`, and
  the opt-in internet list (`masterServerName`, empty by default)

## Linux dedicated server

- [Native Linux dedicated server](linux-dedicated.md)
- [Server-side test clients](server-test-clients.md)
- [Retail CoD4 client compatibility evidence](retail-client-compatibility.md)

## Other architectures and platforms (opt-in)

- [x86_64 track](linux-x64.md): the 64-bit dedicated server and multiplayer
  client, the checks that catch 32-bit assumptions, the Linux and MSVC x64
  compile censuses, and the Windows x64 build
- [64-bit Arm track](linux-arm64.md): the aarch64 Linux dedicated server and
  client build, the x86 assumptions they needed fixed (including pages over
  4 KiB), and the Windows ARM64 build
- [32-bit Arm track](linux-armv7.md): the armv7 Linux dedicated server, the
  first step toward a PS Vita port
- [PS Vita dedicated server](vita.md): `KISAK_PLATFORM=vita`, Clang with
  VitaSDK, packed as a `.vpk`; runs on a console and has taken a player
- [PS Vita multiplayer client](vita-client.md): `KISAK_BUILD_VITA_MP_CLIENT`;
  the menu draws through a sceGxm back end on a console, with controls and
  sound, and fitting a map into memory is in progress
- [macOS track](macos.md): the opt-in macOS dedicated server and multiplayer
  client build (arm64 and x86_64), the app bundle, what differs from the Linux
  build, and the hosted CI checks, including the Vulkan back end on MoltenVK

## Linux client foundations (opt-in)

- [Client core test targets](linux-client.md) and
  [frame startup dependencies](linux-client-frame-dependencies.md)
- [Optional SDL3 platform prototype](linux-sdl3-prototype.md)
- UI and menus: [UI assets](linux-ui-assets.md),
  [UI registration](linux-ui-registration.md),
  [UI sampling](linux-ui-sampling.md), [UI stability](linux-ui-stability.md),
  [interned strings](linux-interned-strings.md),
  [offline preparation](linux-offline-preparation.md),
  [offline menu frame](linux-offline-menu-frame.md),
  [menu composition](linux-menu-composition.md),
  [menu input](linux-menu-input.md), [menu traversal](linux-menu-traversal.md),
  [viewport commands](linux-viewport.md)

## Linux renderer

- [Renderer graphics interface](renderer-interface.md): every Direct3D 9 call the renderer makes goes through `src/gfx_d3d/rhi/r_rhi.h` (all platforms), and a CI check keeps it that way
- [Renderer validation and limitations](linux-renderer-validation.md)
- [Native D3D9 device experiment](linux-renderer-probe.md)
- [Packaged world-surface replay](linux-world-surface-probe.md)
- [Renderer frame ownership](linux-render-frame-owner.md)
- [Native D3D9 engine boundary](linux-native-d3d9.md) (opt-in, DXVK Native headers)
- [Renderer world front end](linux-world-frontend.md): asset-free proof of DPVS/BSP batching and the offline probe that runs it over a real map's world
- [D3D9 device probe](linux-device-probe.md) (opt-in): the engine's `r_init.cpp` creates the device through DXVK Native from an SDL3 window
- [World device probe](linux-world-device-probe.md) (opt-in): the graphics zones and one map loaded onto that device through the engine's own loaders, and every device object they create checked
- [World draw probe](linux-world-draw-probe.md) (opt-in): views of that map drawn through the engine's own renderer (`R_Init`, `R_LoadWorld`, `R_RenderScene`, the backend's material passes), read back and checked
- [Free camera](linux-free-camera.md) (opt-in): a host camera flown through the engine renderer on a real map, live or by a checked script, with the frame owner that sequences input, device reset, resize, map reload and shutdown
- [Standalone renderer build/run](../tests/linux_renderer/README.md)

## Vulkan back end (opt-in)

The game's D3D9 renderer drawn through Vulkan without DXVK: `r_backend vulkan`
in a build with `KISAK_BUILD_VULKAN_BACKEND`. Bottom to top:

- [Vulkan core and window probe](vulkan-core.md): `src/gfx_vk`, the device, swapchain, memory, uploads, passes and pipelines, with a probe that clears, draws, presents and resizes an SDL3 window
- [Legacy shader translator](vulkan-legacy-shaders.md): the game's D3D9 shader bytecode to SPIR-V through dxbc-spirv ([how the converter was picked](shader-translator-spike.md))
- [Legacy adapter](vulkan-legacy-adapter.md): the part of D3D9 the renderer uses, on the core, with a pixel check
- [D3D9 front](vulkan-d3d9-front.md): the D3D9 interfaces on the adapter, and [the back end in the game](vulkan-d3d9-front.md#in-the-game-r_backend-vulkan) on Linux and Windows, its dvars, caches and the comparison with DXVK

## Performance

- [Measuring a change from inside the game](perf-ab.md): `perf_ab` and
  `perf_sample` compare two settings in one launch (Linux and the other
  native builds, not Windows yet), with the switches to measure:
  `r_gpuSyncSleep` and `xanim_blendOnce`, both off by default
- [Speed builds](linux-speed-builds.md): opt-in build options for measuring
  (`KISAK_LINUX_CPU`, `KISAK_LINUX_LTO`, `KISAK_LINUX_CLIENT_ASSERTS=OFF`);
  none is measured yet

## Upstream

- [Upstream patch queue](upstream/README.md): changes prepared for the
  upstream repository, one patch per future pull request. Nothing in it has
  been sent.

## Tests

- [Asset-free tests](../scripts/tests/README.md)
- [What hosted CI runs, and when](ci.md): what a pull request runs, what waits
  for the merge, and why a draft runs nothing
- [Runs on the test machines](runs.md): the `framelog` frame-rate line, the
  recipe-driven frame-rate driver and its checksummed results file, short PS
  Vita starts, and what a run on a machine with a GPU or game data may and may
  not keep
- [D3DX shader runtime check](../scripts/tests/d3dx_shader_runtime.md)

## History

Superseded progress notes, including the 2026-11-01 handoff, and the
2026-10-31 overnight review are in
[history](history/README.md). They are kept as written and are not current state.
