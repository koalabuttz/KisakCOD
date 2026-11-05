# KisakCOD documentation

## Start here

**[Linux port status](status.md)**: what works, what is verified and where, and
what is still missing. Build steps are in the
[top-level README](../README.md#how-to-build).

## Playing on Linux (opt-in)

- [Multiplayer client](linux-mp-client.md): `KisakCOD-mp`, the whole game in one
  native process; playable on a listen server (i386 and x86_64), remote servers
  refused without Steam authentication
- [Cinematics](linux-cinematics.md): Bink movies through an FFmpeg-backed shim
  (`KISAK_LINUX_CINEMATICS`)

## Linux dedicated server

- [Native Linux dedicated server](linux-dedicated.md)
- [Server-side test clients](server-test-clients.md)
- [Retail CoD4 client compatibility evidence](retail-client-compatibility.md)

## 64-bit track (opt-in)

- [x86_64 track](linux-x64.md): the 64-bit dedicated server and multiplayer
  client, the checks that catch 32-bit assumptions, and the Linux and MSVC x64
  compile censuses

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

## Tests

- [Asset-free tests](../scripts/tests/README.md)
- [D3DX shader runtime check](../scripts/tests/d3dx_shader_runtime.md)

## History

Superseded progress notes, including the 2026-11-01 handoff, and the
2026-10-31 overnight review are in
[history](history/README.md). They are kept as written and are not current state.
