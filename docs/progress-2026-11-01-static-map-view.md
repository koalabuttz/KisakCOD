# Static map view: device-free groundwork — 2026-11-01

Goal of the milestone: one fixed view of a genuine map drawn through the engine's
own renderer on native i386 Linux. This change does the part that needs neither a
device nor assets, so the device work can start from proven ground once the
native D3D9 boundary (PR #15) is merged. Nothing here loads a map or draws.

## Added

| Piece | Where | What it establishes |
| --- | --- | --- |
| World front-end test | `tests/linux_world_frontend/`, `scripts/tests/run_world_frontend_checks.py` | the unmodified production portal walk, AABB-tree and cull-group culling, BSP batching and merge produce the batches the geometry predicts (26 checks, synthetic two-cell world) |
| Client compile census | `scripts/tests/run_client_compile_census.py`, `client_compile_census_blocked.txt` | 370 of 411 multiplayer client sources compile for i386 with the client definitions; each of the other 41 is listed with what the compiler stops on, and the list is enforced in both directions |
| CRT shims | `src/linux/compat.h`, `tests/linux_compat/crt_shims_test.cpp` | `_itoa`, the array `strcpy_s`, `_time64` and `_localtime64`, failing closed on bad input; `_ctime64` deliberately not shimmed (its one caller frees the CRT's static buffer) |
| CI | `world_frontend` (Debug, RelWithDebInfo) and `client_census` jobs | the above on hosted runners, with no DXVK, SDL, GPU, assets or engine build |

Details, link rules and limits: [linux-world-frontend.md](linux-world-frontend.md).

## Verified, and where

All local, in a sandbox with clang 18 and GCC 13 multilib, no GPU and no game
assets:

- `run_world_frontend_checks.py` passes in Debug (`-O0 -D_DEBUG`) and
  RelWithDebInfo (`-O2 -DNDEBUG`), both with `RELEASE_ASSERTS`.
- Four hand-made breakages of the synthetic world each fail the test.
- `run_client_compile_census.py` passes (411 sources, 41 expected failures).
- Runner self-tests: 25 for the front-end runner, 23 for the census.
- The repository's required native checks for a renderer change (see the commit
  message for the exact list and results).

The two new CI jobs have not been observed hosted yet.

Also checked locally, not committed: with the boundary branch of PR #15 and the
pinned DXVK 2.7.1 headers, `r_scene.cpp` and `rb_backend.cpp` compile unmodified
under the engine's `-std=gnu++20`. The boundary runner uses `-std=c++20`, which
hides them (Bink's platform detection needs the `linux` macro); this was passed
to the boundary work.

## Not done

- No `r_scene.cpp` link yet: the test mirrors two small pieces of it
  (`R_InitScene`'s stage table, `R_SetViewParmsForScene`), marked in the source.
- No real `GfxWorld`, no device, no resource creation, no draw, no GPU evidence.
- The census compiles with the D3D9 stand-in only.

## Next

Follow the order in [linux-world-frontend.md](linux-world-frontend.md#order-of-work-toward-one-static-map-view):
link `r_scene.cpp` through the boundary and delete the mirrors; run the front
end over one real world from an explicit private asset path; then device,
resources, one fixed-view draw behind the free-camera `SceneRenderer` interface,
and GPU evidence on a developer machine.
