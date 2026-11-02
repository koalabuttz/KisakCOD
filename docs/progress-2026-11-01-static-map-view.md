# Static map view: device-free groundwork — 2026-11-01

Goal of the milestone: one fixed view of a genuine map drawn through the engine's
own renderer on native i386 Linux. This change does the part that needs neither a
device nor assets, so the device work can start from proven ground on top of the
native D3D9 boundary ([linux-native-d3d9.md](linux-native-d3d9.md), PR #15).
Nothing here loads a map or draws.

## Added

| Piece | Where | What it establishes |
| --- | --- | --- |
| World front-end test | `tests/linux_world_frontend/`, `scripts/tests/run_world_frontend_checks.py` | the unmodified production portal walk, AABB-tree and cull-group culling, BSP batching and merge produce the batches the geometry predicts (26 checks, synthetic two-cell world) |
| Client compile census | `scripts/tests/run_client_compile_census.py`, `client_compile_census_blocked.txt` | 370 of 411 multiplayer client sources compile for i386 with the client definitions; each of the other 41 is listed with what the compiler stops on, and the list is enforced in both directions |
| CRT shims | `src/linux/compat.h`, `tests/linux_compat/crt_shims_test.cpp` | `_itoa`, the array `strcpy_s`, `_time64` and `_localtime64`, failing closed on bad input; `_ctime64` deliberately not shimmed (its one caller frees the CRT's static buffer) |
| CI | `world_frontend` (Debug, RelWithDebInfo) and `client_census` jobs | the above on hosted runners, with no DXVK, SDL, GPU, assets or engine build |

Details, link rules and limits: [linux-world-frontend.md](linux-world-frontend.md).

## Verified, and where

Locally, in a sandbox with clang 18 and GCC 13 multilib, no GPU and no game
assets, on the branch merged with master after #15 and #16:

- `run_world_frontend_checks.py` passes in Debug (`-O0 -D_DEBUG`) and
  RelWithDebInfo (`-O2 -DNDEBUG`), both with `RELEASE_ASSERTS`.
- Four hand-made breakages of the synthetic world each fail the test.
- `run_client_compile_census.py` passes (411 sources, 41 expected failures).
- Runner self-tests: 25 for the front-end runner, 25 for the census, plus the
  window workflow, boundary, world surface and check-runner contract tests.
- `run_native_d3d9_boundary_checks.py` passes in both configurations with the
  pinned DXVK 2.7.1 headers, so the `compat.h` additions do not disturb it.
- The repository's required native checks for a renderer change (see the commit
  messages for the exact list and results).

Hosted, before the merge with master: the `client_census` job and both
`world_frontend` jobs passed on GitHub runners together with the existing Linux
jobs. The Windows DirectX SDK jobs failed on a `BOOL` redefinition that was
already failing on master and that #16 fixed there.

While #15 was in review, this work found that `r_scene.cpp` and `rb_backend.cpp`
compile unmodified through the boundary under the engine's `-std=gnu++20`, but
not under the `-std=c++20` #15's runner first used (Bink's platform detection
needs the `linux` macro). #15 moved its engine compiles to `gnu++20` and lists
both.

## Follow-up: `r_scene.cpp` linked

After #17 merged, the front-end test links `r_scene.cpp` and `r_buffers.cpp`
through the native D3D9 boundary (DXVK Native 2.7.1 headers only, the rest of the
link keeps the stand-in). The two mirrors are gone: the test calls the production
`R_InitScene` and `R_SetViewParmsForScene`, with two new checks on them (28 in
all). `scene` and `gfxBuf` storage and five traps left the link boundary because
their owners are linked now, together with the four small owners the real
surface adders need (`r_draw_method.cpp`, `r_xsurface.cpp`, `xmodel_utils.cpp`,
`dobj_utils.cpp`). The `world_frontend` CI job fetches the same pinned DXVK
headers as `native_d3d9_boundary`.

## Follow-up: a real map, 2026-11-02

`KisakCOD-world-frontend-probe` (opt-in `KISAK_BUILD_WORLD_FRONTEND_PROBE`, on top
of the native D3D9 boundary) is the offline asset exporter with the renderer
front end linked in. It loads one allowlisted map from an explicit private game
folder, picks the shader model 3 path and checks every shader the materials
reach, runs `Material_Sort` and `R_SortWorldSurfaces`, then walks, batches and
merges a view from every player spawn and the intermission camera. Each frame is
held to `world_frontend_invariants.h`. The dedicated server's renderer stand-ins
moved to `src/linux/server_renderer_counterparts.cpp` so the probe can link the
real owners instead.

Verified locally on David's laptop (Intel Processor U300, Debian 13, clang 19.1.7,
the owner's `mp_crash.ff` read in place): at 629c67d, mp_crash passes all 178
views in RelWithDebInfo and Debug, with identical reports apart from timing.
The numbers are in
[linux-world-frontend.md](linux-world-frontend.md#evidence-mp_crash-2026-11-02).
Hosted CI builds the probe in both configurations, checks it is a 32-bit i386
binary needing only the C and C++ runtimes, and runs its asset-free test; it
never runs the probe.

The first runs found three probe defects (no shader path chosen, a visibility
bound read from a word count, and a portal-subset rule that the engine's
portal culling does not satisfy), each fixed in its own commit.

## Follow-up: the device, 2026-11-02

`KisakCOD-device-probe` (opt-in `KISAK_BUILD_DEVICE_PROBE`, on top of the native
D3D9 boundary, with DXVK Native and SDL3) runs `r_init.cpp`'s own
`R_PreCreateWindow`, `R_SetWndParms`, `R_CreateWindow` and `R_CreateDevice`,
requires the shader model 3 path, reads a cleared back buffer back in memory,
presents frames and releases everything. `r_init.cpp` and `rb_sky.cpp` now
compile unmodified through the boundary (23 units): the Win32 window, monitor and
`Sleep` calls are declared in `src/linux/win32_window.h` and implemented over
SDL3 in `src/linux/client/window/win32_window_sdl3.cpp`, with an asset-free test
on SDL's dummy driver. CI builds both in Debug and RelWithDebInfo against a
stand-in D3D9 library and checks the probe fails closed.

Verified locally, not in CI. On David's laptop GPU (Intel Raptor Lake-P, Mesa
ANV 25.0.7, DXVK 2.7.1 i386), the probe passes in Debug and RelWithDebInfo on
both Wayland and XWayland: shader model 3 path, 0 of 307,200 read-back pixels
wrong, 120 frames presented. It also passes in the sandbox on Mesa lavapipe
under Xvfb. Details and limits: [linux-device-probe.md](linux-device-probe.md).

## Follow-up: a map on the device, 2026-11-02

`KisakCOD-world-device-probe` (opt-in `KISAK_BUILD_WORLD_DEVICE_PROBE`, on top
of both earlier probes) is the asset exporter with the renderer's device code
linked in. It creates the device through `r_init.cpp`, then loads
`code_post_gfx_mp`, `common_mp`, their localized zones and one allowlisted map
with `r_loadForRenderer` on, so the engine's own loaders create every texture,
shader, vertex declaration and vertex/index buffer on the device.

It then checks all of them, and everything the world's surfaces draw with, as
pure rules with an asset-free test. It writes one report into a private
directory.

Supporting changes:

- The dedicated server's renderer start and its device counterparts moved from
  `server_assets.cpp` to `server_device_counterparts.cpp`, unchanged, so the
  probe can leave them out.
- `r_utils.cpp`'s `R_ReleaseD3DSurface` now compiles wherever the D3D9 API is
  real.

CI builds the probe against the stand-in D3D9 library and checks it fails
closed. Verified locally, not in CI: on David's laptop GPU (Intel Raptor Lake-P,
Mesa ANV 25.0.7, DXVK 2.7.1 i386) mp_crash passes in Debug and RelWithDebInfo
with no problems. 1,630 images, 1,270 shader model 3 shaders and both world
vertex buffers are on the device, and so is everything the world's 5,558
surfaces draw with. Details and limits:
[linux-world-device-probe.md](linux-world-device-probe.md).

## Not done

- No draw. The world's device objects exist only in the world device probe,
  which stops after checking them.
- `R_GenerateSortedDrawSurfs` is not called as a whole: its lighting, fog, sun,
  shadow and entity stages need world data the synthetic world lacks.
- The census compiles with the D3D9 stand-in only.

## Next

Follow the order in [linux-world-frontend.md](linux-world-frontend.md#order-of-work-toward-one-static-map-view):
one fixed-view draw behind the free-camera `SceneRenderer` interface (which
needs `R_InitHardware`'s later steps: render targets, initial state, scene and
`R_InitSystems`), and GPU evidence of that draw on a developer machine.
