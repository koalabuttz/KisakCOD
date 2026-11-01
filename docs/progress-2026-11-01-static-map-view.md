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

## Not done

- No real `GfxWorld`, no device, no resource creation, no draw, no GPU evidence.
- `R_GenerateSortedDrawSurfs` is not called as a whole: its lighting, fog, sun,
  shadow and entity stages need world data the synthetic world lacks.
- The census compiles with the D3D9 stand-in only.

## Next

Follow the order in [linux-world-frontend.md](linux-world-frontend.md#order-of-work-toward-one-static-map-view):
run the front end over one real world from an explicit private asset path;
then device, resources, one fixed-view draw behind the free-camera
`SceneRenderer` interface, and GPU evidence on a developer machine.
