# KisakCOD documentation

## Start here

**[Current Linux handoff — 2026-11-01](progress-2026-11-01.md)**: what works,
what is verified and where, and the next milestone. Build steps are in the
[top-level README](../README.md#how-to-build).

## Linux dedicated server

- [Native Linux dedicated server](linux-dedicated.md)
- [Server-side test clients](server-test-clients.md)
- [Retail CoD4 client compatibility evidence](retail-client-compatibility.md)

## Linux client (opt-in)

- [Native Linux client](linux-client.md) and
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
- [Renderer world front end](linux-world-frontend.md): asset-free proof of DPVS/BSP batching, and the [static map view handoff](progress-2026-11-01-static-map-view.md)
- [Standalone renderer build/run](../tests/linux_renderer/README.md)

## Tests

- [Asset-free tests](../scripts/tests/README.md)
- [D3DX shader runtime check](../scripts/tests/d3dx_shader_runtime.md)

## History

Superseded progress notes and the 2026-10-31 overnight review are in
[history](history/README.md). They are kept as written and are not current state.
